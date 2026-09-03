#include "BookArchiveUtils.h"

#include <ZipFile.h>

#include <cctype>
#include <cstdio>
#include <memory>
#include <new>
#include <string_view>

namespace {

bool hasExtensionIgnoreCase(std::string_view path, std::string_view extension) {
  if (path.size() < extension.size()) return false;

  const size_t start = path.size() - extension.size();
  for (size_t i = 0; i < extension.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(path[start + i])) !=
        std::tolower(static_cast<unsigned char>(extension[i]))) {
      return false;
    }
  }
  return true;
}


class HalFilePrint final : public Print {
 public:
  explicit HalFilePrint(HalFile& file) : file_(file) {}
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    const size_t written = file_.write(data, len);
    if (written != len) failed_ = true;
    return written;
  }
  bool ok() const { return !failed_; }
 private:
  HalFile& file_;
  bool failed_ = false;
};

uint32_t fnv1aPathAndSize(const std::string& path, uint64_t size) {
  uint32_t h = 2166136261u;
  for (unsigned char c : path) { h ^= c; h *= 16777619u; }
  for (int i = 0; i < 8; ++i) { h ^= static_cast<uint8_t>(size >> (i * 8)); h *= 16777619u; }
  return h;
}

bool findSingleEpubEntry(ZipFile& zip, std::string& outEntry) {
  std::string found;
  unsigned count = 0;
  if (!zip.enumerateFilePaths([&](std::string_view entryPath) {
        if (hasExtensionIgnoreCase(entryPath, ".epub")) {
          ++count;
          if (count == 1) found.assign(entryPath.data(), entryPath.size());
        }
      })) return false;
  if (count != 1 || found.empty()) return false;
  outEntry = std::move(found);
  return true;
}

}  // namespace

BookArchiveType detectBookArchiveType(const std::string& path) {
  ZipFile zip(path);

  // Every valid EPUB has this required container descriptor. Looking up its
  // entry scans metadata only; no chapter or image bytes are decompressed.
  size_t containerSize = 0;
  if (zip.getInflatedFileSize("META-INF/container.xml", &containerSize) && containerSize > 0) {
    return BookArchiveType::Epub;
  }

  bool containsFb2 = false;
  unsigned epubEntries = 0;
  if (!zip.enumerateFilePaths([&](std::string_view entryPath) {
        if (hasExtensionIgnoreCase(entryPath, ".fb2")) containsFb2 = true;
        if (hasExtensionIgnoreCase(entryPath, ".epub")) ++epubEntries;
      })) {
    return BookArchiveType::None;
  }

  if (containsFb2) return BookArchiveType::Fb2;
  if (epubEntries == 1) return BookArchiveType::WrappedEpub;
  return BookArchiveType::None;
}

bool extractWrappedEpub(const std::string& archivePath, std::string& outEpubPath) {
  ZipFile zip(archivePath);
  std::string innerEntry;
  if (!findSingleEpubEntry(zip, innerEntry)) return false;

  HalFile outer;
  if (!Storage.openFileForRead("BAR", archivePath, outer)) return false;
  const uint64_t outerSize = outer.fileSize64();
  outer.close();

  Storage.mkdir("/.inkmod/wrapped_epub", true);
  char name[64];
  std::snprintf(name, sizeof(name), "/.inkmod/wrapped_epub/%08lx.epub",
                static_cast<unsigned long>(fnv1aPathAndSize(archivePath, outerSize)));
  const std::string target = name;

  if (Storage.exists(target.c_str())) {
    // Validate the cached file cheaply before reusing it.
    size_t containerSize = 0;
    if (ZipFile(target).getInflatedFileSize("META-INF/container.xml", &containerSize) && containerSize > 0) {
      outEpubPath = target;
      return true;
    }
    Storage.remove(target.c_str());
  }

  const std::string temp = target + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("BAR", temp, out)) return false;
  HalFilePrint writer(out);
  const bool extracted = zip.readFileToStream(innerEntry.c_str(), writer, 4096, nullptr);
  out.close();
  if (!extracted || !writer.ok()) {
    Storage.remove(temp.c_str());
    return false;
  }

  size_t containerSize = 0;
  if (!ZipFile(temp).getInflatedFileSize("META-INF/container.xml", &containerSize) || containerSize == 0) {
    Storage.remove(temp.c_str());
    return false;
  }

  Storage.remove(target.c_str());
  if (!Storage.rename(temp.c_str(), target.c_str())) {
    Storage.remove(temp.c_str());
    return false;
  }
  outEpubPath = target;
  return true;
}
