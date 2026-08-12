#include "EpubChapterSplitter.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <new>
#include <unordered_map>

#include "EpubOpfLite.h"
#include "Epub/preprocess/EpubChapterSplitter.h"
#include "Epub/preprocess/EpubOpfRewriter.h"

namespace {

constexpr size_t MAX_OPF_BYTES_FOR_OPTIONAL_SPLIT = 48 * 1024;

// Discovery only holds the small container/OPF metadata. Its allocations are
// released before extraction, so a healthy reader heap around 100 KiB can run
// the splitter instead of handing an unsafe whole chapter to the paginator.
constexpr uint32_t SPLITTER_MIN_FREE_HEAP = 96U * 1024U;
constexpr uint32_t SPLITTER_MIN_MAX_ALLOC = 48U * 1024U;
constexpr uint32_t UNPACK_MIN_FREE_HEAP = 88U * 1024U;
constexpr uint32_t UNPACK_MIN_MAX_ALLOC = 40U * 1024U;
constexpr uint32_t DISCOVERY_RETAINED_HEAP = 56U * 1024U;
constexpr size_t MAX_ZIP_ENTRY_PATH_BYTES = 255;

constexpr char CACHE_MAGIC[] = "EPUBSPLIT";
constexpr size_t CACHE_MAGIC_LEN = 9;
constexpr uint8_t CACHE_VERSION = 2;
constexpr char SIGNATURE_FILE[] = "/.split_signature.bin";
constexpr char PACKAGE_DIR[] = "/package.epub";

struct OversizedSpineItem {
  std::string href;
  std::string zipPath;
  uint32_t size = 0;
};

std::string dirnameWithSlash(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string resolveHref(const std::string& baseDir, const std::string& href) {
  return baseDir + href;
}

bool findContentOpfPath(ZipFile& zip, std::string& outPath) {
  size_t containerSize = 0;
  if (!zip.getInflatedFileSize("META-INF/container.xml", &containerSize) || containerSize == 0 ||
      containerSize > 8192) {
    return false;
  }
  size_t readSize = 0;
  uint8_t* buf = zip.readFileToMemory("META-INF/container.xml", &readSize, true);
  if (!buf) return false;
  
  outPath.clear();
  outPath.assign(reinterpret_cast<char*>(buf), readSize);
  free(buf);

  const std::string needle = "full-path=\"";
  const size_t attrPos = outPath.find(needle);
  if (attrPos == std::string::npos) return false;
  const size_t valueStart = attrPos + needle.size();
  const size_t valueEnd = outPath.find('"', valueStart);
  if (valueEnd == std::string::npos) return false;
  outPath = outPath.substr(valueStart, valueEnd - valueStart);
  return !outPath.empty();
}

bool readWholeFile(ZipFile& zip, const std::string& zipPath, std::string& out) {
  size_t size = 0;
  uint8_t* buf = zip.readFileToMemory(zipPath.c_str(), &size, false);
  if (!buf) return false;
  out.assign(reinterpret_cast<char*>(buf), size);
  free(buf);
  return true;
}

bool writeWholeFile(const std::string& path, const std::string& content) {
  HalFile f;
  if (!Storage.openFileForWrite("EPS", path, f)) return false;
  const bool ok = f.write(content.data(), content.size()) == content.size();
  f.close();
  return ok;
}

bool readWholeStorageFile(const std::string& path, const size_t maxBytes, std::string& out) {
  HalFile file;
  if (!Storage.openFileForRead("EPS", path, file)) return false;
  const size_t size = file.fileSize();
  if (size > maxBytes) {
    file.close();
    return false;
  }
  out.assign(size, '\0');
  const bool ok = size == 0 || file.read(out.data(), size) == static_cast<int>(size);
  file.close();
  return ok;
}

bool unpackWholeZip(const std::string& originalPath, const std::string& destDir,
                    const std::string& entryListPath) {
  ZipFile zip(originalPath);
  HalFile entryList;
  if (!Storage.openFileForWrite("EPS", entryListPath, entryList)) return false;

  bool listOk = true;
  const bool enumerationOk = zip.enumerateFilePaths([&](std::string_view path) {
        if (!listOk) return;
        if (!path.empty() && path.back() != '/') {
          if (path.size() > MAX_ZIP_ENTRY_PATH_BYTES) {
            listOk = false;
            return;
          }
          const uint16_t pathLength = static_cast<uint16_t>(path.size());
          listOk = entryList.write(&pathLength, sizeof(pathLength)) == sizeof(pathLength) &&
                   entryList.write(path.data(), path.size()) == path.size();
        }
      });
  entryList.close();
  if (!enumerationOk || !listOk) {
    Storage.remove(entryListPath.c_str());
    return false;
  }

  bool allOk = true;
  size_t entryListOffset = 0;
  char pathBuffer[MAX_ZIP_ENTRY_PATH_BYTES + 1];
  while (allOk) {
    // Real X4 hardware permits only one active SD reader. Read one tiny path
    // record, close the list, then let ZipFile open the archive for streaming.
    if (!Storage.openFileForRead("EPS", entryListPath, entryList)) {
      allOk = false;
      break;
    }
    if (!entryList.seek(entryListOffset)) {
      entryList.close();
      allOk = false;
      break;
    }
    if (entryList.available() <= 0) {
      entryList.close();
      break;
    }
    uint16_t pathLength = 0;
    if (entryList.read(&pathLength, sizeof(pathLength)) != sizeof(pathLength) || pathLength == 0 ||
        pathLength > MAX_ZIP_ENTRY_PATH_BYTES ||
        entryList.read(pathBuffer, pathLength) != static_cast<int>(pathLength)) {
      entryList.close();
      allOk = false;
      break;
    }
    entryListOffset = entryList.position();
    entryList.close();
    pathBuffer[pathLength] = '\0';
    const std::string path(pathBuffer, pathLength);
    const std::string destPath = destDir + "/" + path;
    const size_t lastSlash = destPath.find_last_of('/');
    if (lastSlash != std::string::npos) {
      Storage.mkdir(destPath.substr(0, lastSlash).c_str(), true);
    }
    HalFile out;
    if (!Storage.openFileForWrite("EPS", destPath, out)) {
      allOk = false;
      continue;
    }
    const bool streamOk = zip.readFileToStream(path.c_str(), out, 4096);
    out.close();
    if (!streamOk) allOk = false;
  }
  Storage.remove(entryListPath.c_str());
  return allOk;
}

std::string basenameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string chunkFileName(const std::string& originalHref, size_t chunkIndex) {
  const size_t dot = originalHref.find_last_of('.');
  const std::string base = dot == std::string::npos ? originalHref : originalHref.substr(0, dot);
  const std::string ext = dot == std::string::npos ? std::string() : originalHref.substr(dot);
  return base + "_split" + std::to_string(chunkIndex) + ext;
}

void rewriteLinksToSplitFile(std::string& xhtmlContent, const std::string& originalHref,
                              const std::unordered_map<std::string, size_t>& idToChunk,
                              const std::vector<std::string>& chunkNames) {
  const std::string needle = "href=\"" + originalHref;
  size_t pos = 0;
  while ((pos = xhtmlContent.find(needle, pos)) != std::string::npos) {
    const size_t hrefValueStart = pos + 6;
    const size_t closeQuote = xhtmlContent.find('"', hrefValueStart);
    if (closeQuote == std::string::npos) break;

    const size_t afterName = pos + needle.size();
    if (afterName != closeQuote && xhtmlContent[afterName] != '#') {
      pos = closeQuote + 1;
      continue;
    }
    std::string anchor;
    if (afterName < closeQuote && xhtmlContent[afterName] == '#') {
      anchor = xhtmlContent.substr(afterName + 1, closeQuote - afterName - 1);
    }
    size_t targetChunk = 0;
    if (!anchor.empty()) {
      const auto it = idToChunk.find(anchor);
      if (it != idToChunk.end()) targetChunk = it->second;
    }
    const std::string replacement = "href=\"" + chunkNames[targetChunk] + (anchor.empty() ? "" : "#" + anchor);
    xhtmlContent.replace(pos, closeQuote - pos, replacement);
    pos += replacement.size() + 1;
  }
}

}  // namespace

namespace {

std::string resolveReadPathForZip(const std::string& originalPath, const std::string& cacheBaseDir) {
  const std::string cacheKey = "epubsplit_" + std::to_string(std::hash<std::string>{}(originalPath));
  const std::string cachePath = cacheBaseDir + "/" + cacheKey;
  const std::string packagePath = cachePath + PACKAGE_DIR;

  uint64_t sourceSize = 0;
  {
    HalFile source;
    if (Storage.openFileForRead("EPS", originalPath, source)) {
      sourceSize = source.fileSize64();
      source.close();
    }
  }

  {
    HalFile sig;
    if (Storage.openFileForRead("EPS", cachePath + SIGNATURE_FILE, sig)) {
      char magic[CACHE_MAGIC_LEN];
      uint8_t version = 0;
      uint64_t cachedSize = 0;
      const bool valid = sig.read(magic, CACHE_MAGIC_LEN) == CACHE_MAGIC_LEN &&
                         memcmp(magic, CACHE_MAGIC, CACHE_MAGIC_LEN) == 0 &&
                         sig.read(&version, sizeof(version)) == sizeof(version) && version == CACHE_VERSION &&
                         sig.read(&cachedSize, sizeof(cachedSize)) == sizeof(cachedSize) && cachedSize == sourceSize;
      sig.close();
      if (valid) return packagePath;
    }
  }

  const auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeap(heap, SPLITTER_MIN_FREE_HEAP, SPLITTER_MIN_MAX_ALLOC)) {
    LOG_INF("EPS", "Skipping optional EPUB split: low heap (%u free, %u max alloc)", heap.freeHeap,
            heap.maxAllocHeap);
    return originalPath;
  }

  std::string opfPath;
  std::vector<OversizedSpineItem> oversizedSpineItems;
  std::vector<std::string> ncxRelativePaths;

  // Keep discovery allocations in a narrow scope. The OPF text, manifest,
  // spine vectors and ZIP size tables must all be gone before extraction.
  {
    ZipFile zip(originalPath);
    if (!findContentOpfPath(zip, opfPath)) return originalPath;
    size_t opfSize = 0;
    if (!zip.getInflatedFileSize(opfPath.c_str(), &opfSize)) return originalPath;
    if (opfSize > MAX_OPF_BYTES_FOR_OPTIONAL_SPLIT) {
      LOG_INF("EPS", "Skipping optional EPUB split: content.opf is %u bytes", static_cast<unsigned>(opfSize));
      return originalPath;
    }
    const auto opfHeap = MemoryBudget::snapshot();
    const size_t safeOpfBytesByFree =
        opfHeap.freeHeap > DISCOVERY_RETAINED_HEAP ? (opfHeap.freeHeap - DISCOVERY_RETAINED_HEAP) / 2 : 0;
    const size_t safeOpfBytes = std::min<size_t>(safeOpfBytesByFree, opfHeap.maxAllocHeap / 2);
    if (opfSize > safeOpfBytes) {
      LOG_INF("EPS", "Skipping optional EPUB split: content.opf needs too much discovery heap (%u bytes, safe %u)",
              static_cast<unsigned>(opfSize), static_cast<unsigned>(safeOpfBytes));
      return originalPath;
    }
    std::string opfContent;
    if (!readWholeFile(zip, opfPath, opfContent)) return originalPath;
    EpubOpfLite opf;
    if (!EpubOpfLite::parse(opfContent, opf) || opf.manifest.empty() || opf.spineIdrefs.empty()) return originalPath;

    const std::string opfDir = dirnameWithSlash(opfPath);
    const auto findManifestItem = [&opf](const std::string& idref) -> const EpubOpfManifestItem* {
      for (const auto& item : opf.manifest) {
        if (item.id == idref) return &item;
      }
      return nullptr;
    };

    std::deque<ZipFile::SizeTarget> targets;
    for (size_t i = 0; i < opf.spineIdrefs.size(); ++i) {
      const auto* item = findManifestItem(opf.spineIdrefs[i]);
      const std::string zipPath = item ? resolveHref(opfDir, item->href) : std::string();
      if (zipPath.empty() || i > 0xFFFF) continue;
      targets.push_back({ZipFile::fnvHash64(zipPath.data(), zipPath.size()), static_cast<uint16_t>(zipPath.size()),
                         static_cast<uint16_t>(i)});
    }
    std::sort(targets.begin(), targets.end(),
              [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
                return a.hash != b.hash ? a.hash < b.hash : a.len < b.len;
              });
    std::deque<uint32_t> sizes(opf.spineIdrefs.size(), 0);
    zip.fillUncompressedSizes(targets, sizes);

    for (size_t i = 0; i < sizes.size(); ++i) {
      if (sizes[i] > EpubStreamingChapterSplitter::SPLIT_THRESHOLD_BYTES) {
        const auto* item = findManifestItem(opf.spineIdrefs[i]);
        if (item) {
          oversizedSpineItems.push_back({item->href, resolveHref(opfDir, item->href), sizes[i]});
        }
      }
    }
    if (oversizedSpineItems.empty()) return originalPath;

    for (const auto& item : opf.manifest) {
      if (item.mediaType.find("ncx") != std::string::npos) {
        ncxRelativePaths.push_back(resolveHref(opfDir, item.href));
      }
    }
  }

  LOG_INF("EPS", "Preparing cached split for %u oversized spine item(s)",
          static_cast<unsigned>(oversizedSpineItems.size()));

  // Extraction needs the inflater's 32 KiB window. Discovery is out of scope
  // now, so this guard measures the real steady-state budget rather than a
  // temporary OPF/manifest peak.
  const auto heap2 = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeap(heap2, UNPACK_MIN_FREE_HEAP, UNPACK_MIN_MAX_ALLOC)) {
    LOG_INF("EPS", "Bailing out before unpack: heap dropped too low (%u free, %u max alloc)", heap2.freeHeap,
            heap2.maxAllocHeap);
    return originalPath;
  }

  if (Storage.exists(cachePath.c_str())) Storage.removeDir(cachePath.c_str());
  Storage.mkdir(packagePath.c_str(), true);
  const std::string entryListPath = cachePath + "/.unpack_entries.bin";
  if (!unpackWholeZip(originalPath, packagePath, entryListPath)) {
    LOG_ERR("EPS", "Failed to unpack %s for splitting", originalPath.c_str());
    Storage.removeDir(cachePath.c_str());
    return originalPath;
  }

  for (const auto& oversizedItem : oversizedSpineItems) {
    LOG_INF("EPS", "Spine item '%s' is %u bytes - splitting", oversizedItem.zipPath.c_str(), oversizedItem.size);

    const std::string oversizedDir = dirnameWithSlash(oversizedItem.zipPath);
    const std::string sourcePath = packagePath + "/" + oversizedItem.zipPath;
    std::string baseName = basenameOf(oversizedItem.zipPath);
    const size_t extension = baseName.find_last_of('.');
    if (extension != std::string::npos) baseName.resize(extension);

    std::unordered_map<std::string, int> anchorFragments;
    auto chunkNames = EpubStreamingChapterSplitter::splitToFragments(
        sourcePath, packagePath + "/" + oversizedDir, baseName, &anchorFragments);
    if (chunkNames.size() < 2) {
      LOG_ERR("EPS", "Oversized spine item did not yield a usable streaming split");
      Storage.removeDir(cachePath.c_str());
      return originalPath;
    }

    std::vector<std::string> chunkHrefs;
    chunkHrefs.reserve(chunkNames.size());
    for (const auto& name : chunkNames) chunkHrefs.push_back(dirnameWithSlash(oversizedItem.href) + name);

    {
      std::string opfContent;
      if (!readWholeStorageFile(packagePath + "/" + opfPath, MAX_OPF_BYTES_FOR_OPTIONAL_SPLIT, opfContent)) {
        LOG_ERR("EPS", "Could not reload content.opf after bounded extraction");
        Storage.removeDir(cachePath.c_str());
        return originalPath;
      }
      std::string originalItemId;
      const std::string rewrittenOpf =
          EpubOpfRewriter::rewriteForSplitItem(opfContent, oversizedItem.href, chunkHrefs, &originalItemId);
      if (rewrittenOpf.empty() || !writeWholeFile(packagePath + "/" + opfPath, rewrittenOpf)) {
        LOG_ERR("EPS", "Could not rewrite content.opf for streaming split");
        Storage.removeDir(cachePath.c_str());
        return originalPath;
      }
    }

    for (const auto& ncxRelativePath : ncxRelativePaths) {
      const std::string ncxPath = packagePath + "/" + ncxRelativePath;
      HalFile ncx;
      if (!Storage.openFileForRead("EPS", ncxPath, ncx)) continue;
      const size_t size = ncx.fileSize();
      std::string content(size, '\0');
      const bool readOk = ncx.read(content.data(), size) == static_cast<int>(size);
      ncx.close();
      if (!readOk) continue;
      const std::string rewritten =
          EpubNcxRewriter::redirectReferences(content, oversizedItem.href, chunkHrefs, anchorFragments);
      if (rewritten != content && !writeWholeFile(ncxPath, rewritten)) {
        LOG_ERR("EPS", "Could not rewrite NCX for streaming split");
        Storage.removeDir(cachePath.c_str());
        return originalPath;
      }
    }

    Storage.remove(sourcePath.c_str());
  }

  {
    HalFile sig;
    if (Storage.openFileForWrite("EPS", cachePath + SIGNATURE_FILE, sig)) {
      sig.write(CACHE_MAGIC, CACHE_MAGIC_LEN);
      const uint8_t version = CACHE_VERSION;
      sig.write(&version, sizeof(version));
      sig.write(&sourceSize, sizeof(sourceSize));
      sig.close();
    }
  }

  return packagePath;
}

}  // namespace

std::string EpubChapterSplitter::resolveReadPath(const std::string& originalPath, const std::string& cacheBaseDir) {
  HalFile probe;
  if (!Storage.openFileForRead("EPS", originalPath, probe)) return originalPath;
  const bool isDirectory = probe.isDirectory();
  probe.close();
  if (isDirectory) return originalPath;

  return resolveReadPathForZip(originalPath, cacheBaseDir);
}

#ifdef EPUB_CHAPTER_SPLITTER_EXPOSE_FOR_TESTS
std::string EpubChapterSplitter::resolveReadPathForZipTestOnly(const std::string& originalPath,
                                                                const std::string& cacheBaseDir) {
  return resolveReadPathForZip(originalPath, cacheBaseDir);
}
#endif
