#include "Section.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <ReaderWork.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"
constexpr uint8_t SECTION_FILE_VERSION = 49;
constexpr uint8_t LEGACY_EPUB_SECTION_FILE_VERSION = 48;
constexpr uint32_t HEADER_SIZE = sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) + sizeof(int) + sizeof(float) +
                                 sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                 sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                 sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
  uint32_t nextRecordOffset;
};
static_assert(sizeof(PageLutEntry) == 12, "Unexpected page spool record padding");
constexpr uint32_t PAGE_SPOOL_NEXT_OFFSET = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t);
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (pageCount == UINT16_MAX) {
    LOG_ERR("SCT", "Section exceeds the 65535-page cache limit");
    return 0;
  }
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed (pos=%lu, free=%u, maxAlloc=%u)", pageCount, static_cast<unsigned long>(position),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  pageCount++;
  return position;
}

bool Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                     const uint16_t viewportWidth, const uint16_t viewportHeight,
                                     const bool hyphenationEnabled, const bool embeddedStyle,
                                     const uint8_t imageRendering, const bool bionicReadingEnabled,
                                     const bool guideReadingEnabled) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return false;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_CACHE_MAGIC) + sizeof(SECTION_FILE_VERSION) + sizeof(fontId) +
                                   sizeof(lineCompression) + sizeof(extraParagraphSpacing) +
                                   sizeof(forceParagraphIndents) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(bionicReadingEnabled) +
                                   sizeof(guideReadingEnabled) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  return serialization::tryWritePod(file, SECTION_CACHE_MAGIC) &&
         serialization::tryWritePod(file, SECTION_FILE_VERSION) && serialization::tryWritePod(file, fontId) &&
         serialization::tryWritePod(file, lineCompression) && serialization::tryWritePod(file, extraParagraphSpacing) &&
         serialization::tryWritePod(file, forceParagraphIndents) &&
         serialization::tryWritePod(file, paragraphAlignment) && serialization::tryWritePod(file, viewportWidth) &&
         serialization::tryWritePod(file, viewportHeight) && serialization::tryWritePod(file, hyphenationEnabled) &&
         serialization::tryWritePod(file, embeddedStyle) && serialization::tryWritePod(file, imageRendering) &&
         serialization::tryWritePod(file, bionicReadingEnabled) &&
         serialization::tryWritePod(file, guideReadingEnabled) &&
         serialization::tryWritePod(file,
                                    pageCount) &&  // Placeholder for page count (will be initially 0, patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // Placeholder for LUT offset (patched later)
         serialization::tryWritePod(file,
                                    static_cast<uint32_t>(0)) &&  // Placeholder for anchor map offset (patched later)
         serialization::tryWritePod(
             file,
             static_cast<uint32_t>(0)) &&  // Placeholder for paragraph LUT offset (patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                              const uint16_t viewportWidth, const uint16_t viewportHeight,
                              const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                              const bool bionicReadingEnabled, const bool guideReadingEnabled) {
  const std::string backupPath = filePath + ".bak";
  const std::string tmpPath = filePath + ".tmp";
  const std::string lutTmpPath = filePath + ".lut.tmp";
  if (!Storage.exists(filePath.c_str()) && Storage.exists(backupPath.c_str())) {
    if (!Storage.rename(backupPath.c_str(), filePath.c_str())) {
      LOG_ERR("SCT", "Failed to restore interrupted cache promotion");
      return false;
    }
    LOG_INF("SCT", "Restored previous cache after interrupted promotion");
  } else if (Storage.exists(filePath.c_str()) && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  if (Storage.exists(lutTmpPath.c_str())) Storage.remove(lutTmpPath.c_str());
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint32_t magic;
    if (!serialization::tryReadPod(file, magic)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read cache magic");
      clearCache();
      return false;
    }
    if (magic != SECTION_CACHE_MAGIC) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: cache magic mismatch");
      clearCache();
      return false;
    }

    uint8_t version;
    if (!serialization::tryReadPod(file, version)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read version");
      clearCache();
      return false;
    }
    // Version 49 only changed how FB2-origin packages choose their virtual
    // section boundaries; the serialized header/page format stayed intact.
    // Reuse v48 for normal EPUBs, but force FB2 packages to rebuild so an old
    // unsafe split cannot cut a paragraph between two cache files.
    const bool compatibleLegacyEpubCache =
        version == LEGACY_EPUB_SECTION_FILE_VERSION && !epub->isFb2Package();
    if (version != SECTION_FILE_VERSION && !compatibleLegacyEpubCache) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    bool fileForceParagraphIndents;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileBionicReadingEnabled;
    bool fileGuideReadingEnabled;
    if (!serialization::tryReadPod(file, fileFontId) || !serialization::tryReadPod(file, fileLineCompression) ||
        !serialization::tryReadPod(file, fileExtraParagraphSpacing) ||
        !serialization::tryReadPod(file, fileForceParagraphIndents) ||
        !serialization::tryReadPod(file, fileParagraphAlignment) ||
        !serialization::tryReadPod(file, fileViewportWidth) || !serialization::tryReadPod(file, fileViewportHeight) ||
        !serialization::tryReadPod(file, fileHyphenationEnabled) ||
        !serialization::tryReadPod(file, fileEmbeddedStyle) || !serialization::tryReadPod(file, fileImageRendering) ||
        !serialization::tryReadPod(file, fileBionicReadingEnabled) ||
        !serialization::tryReadPod(file, fileGuideReadingEnabled)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated section header");
      clearCache();
      return false;
    }

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || forceParagraphIndents != fileForceParagraphIndents ||
        paragraphAlignment != fileParagraphAlignment || viewportWidth != fileViewportWidth ||
        viewportHeight != fileViewportHeight || hyphenationEnabled != fileHyphenationEnabled ||
        embeddedStyle != fileEmbeddedStyle || imageRendering != fileImageRendering ||
        bionicReadingEnabled != fileBionicReadingEnabled || guideReadingEnabled != fileGuideReadingEnabled) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  if (!serialization::tryReadPod(file, pageCount)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing page count");
    clearCache();
    return false;
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  const std::string backupPath = filePath + ".bak";
  const std::string tmpPath = filePath + ".tmp";
  const std::string lutTmpPath = filePath + ".lut.tmp";
  const bool hasAnyCache = Storage.exists(filePath.c_str()) || Storage.exists(backupPath.c_str()) ||
                           Storage.exists(tmpPath.c_str()) || Storage.exists(lutTmpPath.c_str());
  if (!hasAnyCache) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  bool removed = true;
  if (Storage.exists(filePath.c_str())) removed = Storage.remove(filePath.c_str()) && removed;
  if (Storage.exists(backupPath.c_str())) removed = Storage.remove(backupPath.c_str()) && removed;
  if (Storage.exists(tmpPath.c_str())) removed = Storage.remove(tmpPath.c_str()) && removed;
  if (Storage.exists(lutTmpPath.c_str())) removed = Storage.remove(lutTmpPath.c_str()) && removed;
  if (!removed) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                                const bool bionicReadingEnabled, const bool guideReadingEnabled,
                                const std::function<void()>& popupFn, bool* imagesWereSuppressed,
                                bool* layoutAbortedForLowMemory,
                                const reader::ReaderCancellationToken* cancellationToken, bool* cancelled) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";
  const auto tmpSectionPath = filePath + ".tmp";
  const auto tmpLutPath = filePath + ".lut.tmp";
  pageCount = 0;
  if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = false;
  if (cancelled) *cancelled = false;
  LOG_DBG("SCT", "Create section start: spine=%d viewport=%ux%u image=%u bionic=%u guide=%u free=%u maxAlloc=%u",
          spineIndex, viewportWidth, viewportHeight, imageRendering, bionicReadingEnabled, guideReadingEnabled,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (cancellationToken && cancellationToken->isCancellationRequested()) {
      if (cancelled) *cancelled = true;
      break;
    }
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024, cancellationToken);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    if (cancellationToken && cancellationToken->isCancellationRequested()) {
      if (cancelled) *cancelled = true;
      LOG_INF("SCT", "Section stream cancelled: spine=%d", spineIndex);
      return false;
    }
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes, free=%u, maxAlloc=%u)", tmpHtmlPath.c_str(), fileSize,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  if (!writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents, paragraphAlignment,
                              viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering,
                              bionicReadingEnabled, guideReadingEnabled)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  if (Storage.exists(tmpLutPath.c_str())) {
    Storage.remove(tmpLutPath.c_str());
  }
  bool lutWriteFailed = false;
  uint16_t lutEntryCount = 0;
  uint32_t firstLutRecordOffset = 0;
  uint32_t lastLutRecordOffset = 0;

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      const auto cssHeapBefore = MemoryBudget::snapshot();
      const bool cssLoaded = cssParser->loadFromCache();
      const auto cssHeapAfter = MemoryBudget::snapshot();
      LOG_DBG("SCT", "CSS cache load: ok=%u partial=%u rules=%u free=%u->%u delta=%d maxAlloc=%u->%u delta=%d",
              cssLoaded ? 1U : 0U, cssParser->isCachePartial() ? 1U : 0U, static_cast<unsigned>(cssParser->ruleCount()),
              cssHeapBefore.freeHeap, cssHeapAfter.freeHeap,
              static_cast<int32_t>(cssHeapAfter.freeHeap) - static_cast<int32_t>(cssHeapBefore.freeHeap),
              cssHeapBefore.maxAllocHeap, cssHeapAfter.maxAllocHeap,
              static_cast<int32_t>(cssHeapAfter.maxAllocHeap) - static_cast<int32_t>(cssHeapBefore.maxAllocHeap));
      if (!cssLoaded) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, bionicReadingEnabled, guideReadingEnabled,
      [this, &lutWriteFailed, &lutEntryCount, &firstLutRecordOffset,
       &lastLutRecordOffset](std::unique_ptr<Page> page, const uint16_t paragraphIndex,
                            const uint16_t listItemIndex) {
        if (lutWriteFailed) return;
        const uint32_t pageOffset = this->onPageComplete(std::move(page));
        const uint32_t recordOffset = file.position();
        const PageLutEntry entry{pageOffset, paragraphIndex, listItemIndex, 0};
        if (pageOffset == 0 || !serialization::tryWritePod(file, entry.fileOffset) ||
            !serialization::tryWritePod(file, entry.paragraphIndex) ||
            !serialization::tryWritePod(file, entry.listItemIndex) ||
            !serialization::tryWritePod(file, entry.nextRecordOffset)) {
          lutWriteFailed = true;
          return;
        }
        const uint32_t endOffset = file.position();
        if (lastLutRecordOffset != 0 &&
            (!file.seek(lastLutRecordOffset + PAGE_SPOOL_NEXT_OFFSET) ||
             !serialization::tryWritePod(file, recordOffset) || !file.seek(endOffset))) {
          lutWriteFailed = true;
          return;
        }
        if (firstLutRecordOffset == 0) firstLutRecordOffset = recordOffset;
        lastLutRecordOffset = recordOffset;
        ++lutEntryCount;
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser,
      cancellationToken);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  LOG_DBG("SCT", "Parser start: spine=%d free=%u maxAlloc=%u", spineIndex, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  success = visitor.parseAndBuildPages();
  LOG_DBG("SCT", "Parser done: spine=%d success=%u pages=%u free=%u maxAlloc=%u", spineIndex, success, pageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (imagesWereSuppressed) *imagesWereSuppressed = visitor.wasLowMemoryFallbackTriggered();
  if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = visitor.wasLowMemoryAbortTriggered();
  if (cancelled) *cancelled = visitor.wasCancellationTriggered();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success || lutWriteFailed || lutEntryCount != pageCount) {
    if (visitor.wasCancellationTriggered()) {
      LOG_INF("SCT", "Section parse cancelled: spine=%d", spineIndex);
    } else if (lutWriteFailed || lutEntryCount != pageCount) {
      LOG_ERR("SCT", "Failed to spool page index (entries=%u pages=%u)", lutEntryCount, pageCount);
    } else {
      LOG_ERR("SCT", "Failed to parse XML and build pages");
    }
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    Storage.remove(tmpLutPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  if (cancellationToken && cancellationToken->isCancellationRequested()) {
    if (cancelled) *cancelled = true;
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    Storage.remove(tmpLutPath.c_str());
    if (cssParser) cssParser->clear();
    LOG_INF("SCT", "Section finalization cancelled: spine=%d", spineIndex);
    return false;
  }

  if (!file.sync()) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) cssParser->clear();
    return false;
  }

  auto readSpoolRecord = [&](const uint32_t recordOffset, PageLutEntry& entry) {
    return recordOffset != 0 && file.seek(recordOffset) && serialization::tryReadPod(file, entry.fileOffset) &&
           serialization::tryReadPod(file, entry.paragraphIndex) &&
           serialization::tryReadPod(file, entry.listItemIndex) &&
           serialization::tryReadPod(file, entry.nextRecordOffset);
  };
  auto cancelledDuringFinalization = [&]() {
    if (!cancellationToken || !cancellationToken->isCancellationRequested()) return false;
    if (cancelled) *cancelled = true;
    return true;
  };
  auto failFinalization = [&]() {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) cssParser->clear();
    return false;
  };

  const uint32_t lutOffset = file.size();
  uint32_t recordOffset = firstLutRecordOffset;
  uint32_t outputOffset = lutOffset;
  // Copy page offsets from the linked fixed-record spool embedded in the
  // same temporary section file. Page count no longer determines heap use,
  // and no second SD file handle is needed on X4 hardware.
  for (uint16_t i = 0; i < lutEntryCount; ++i) {
    if (cancelledDuringFinalization()) return failFinalization();
    PageLutEntry entry{};
    if (!readSpoolRecord(recordOffset, entry) || entry.fileOffset == 0 || !file.seek(outputOffset) ||
        !serialization::tryWritePod(file, entry.fileOffset)) {
      LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
      return failFinalization();
    }
    outputOffset = file.position();
    recordOffset = entry.nextRecordOffset;
  }
  if (recordOffset != 0) {
    LOG_ERR("SCT", "Page spool contains more records than expected");
    return failFinalization();
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = outputOffset;
  if (!file.seek(anchorMapOffset)) return failFinalization();
  const auto& anchors = visitor.getAnchors();
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(anchors.size()))) {
    return failFinalization();
  }
  for (const auto& [anchor, page] : anchors) {
    if (cancelledDuringFinalization()) return failFinalization();
    if (!serialization::tryWriteString(file, anchor) || !serialization::tryWritePod(file, page)) {
      return failFinalization();
    }
  }

  const uint32_t paragraphLutOffset = file.position();
  if (!serialization::tryWritePod(file, lutEntryCount)) return failFinalization();
  recordOffset = firstLutRecordOffset;
  outputOffset = file.position();
  for (uint16_t i = 0; i < lutEntryCount; ++i) {
    if (cancelledDuringFinalization()) return failFinalization();
    PageLutEntry entry{};
    if (!readSpoolRecord(recordOffset, entry) || !file.seek(outputOffset) ||
        !serialization::tryWritePod(file, entry.paragraphIndex))
      return failFinalization();
    outputOffset = file.position();
    recordOffset = entry.nextRecordOffset;
  }
  if (recordOffset != 0) return failFinalization();

  const uint32_t liLutFileOffset = outputOffset;
  recordOffset = firstLutRecordOffset;
  for (uint16_t i = 0; i < lutEntryCount; ++i) {
    if (cancelledDuringFinalization()) return failFinalization();
    PageLutEntry entry{};
    if (!readSpoolRecord(recordOffset, entry) || !file.seek(outputOffset) ||
        !serialization::tryWritePod(file, entry.listItemIndex))
      return failFinalization();
    outputOffset = file.position();
    recordOffset = entry.nextRecordOffset;
  }
  if (recordOffset != 0) return failFinalization();

  // Patch header with final pageCount, lutOffset, anchorMapOffset, paragraphLutOffset, and liLutOffset.
  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount)) ||
      !serialization::tryWritePod(file, pageCount) || !serialization::tryWritePod(file, lutOffset) ||
      !serialization::tryWritePod(file, anchorMapOffset) || !serialization::tryWritePod(file, paragraphLutOffset) ||
      !serialization::tryWritePod(file, liLutFileOffset) || !file.sync()) {
    LOG_ERR("SCT", "Failed to finalize section cache");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    Storage.remove(tmpLutPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (cancellationToken && cancellationToken->isCancellationRequested()) {
    if (cancelled) *cancelled = true;
    Storage.remove(tmpSectionPath.c_str());
    Storage.remove(tmpLutPath.c_str());
    if (cssParser) cssParser->clear();
    LOG_INF("SCT", "Section promotion cancelled: spine=%d", spineIndex);
    return false;
  }
  const std::string backupPath = filePath + ".bak";
  if (Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  const bool hadExistingCache = Storage.exists(filePath.c_str());
  if (hadExistingCache && !Storage.rename(filePath.c_str(), backupPath.c_str())) {
    LOG_ERR("SCT", "Failed to preserve previous section cache");
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) cssParser->clear();
    return false;
  }
  if (!Storage.rename(tmpSectionPath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to promote temp section cache into place");
    Storage.remove(tmpSectionPath.c_str());
    if (hadExistingCache && !Storage.rename(backupPath.c_str(), filePath.c_str())) {
      LOG_ERR("SCT", "Failed to roll back previous section cache");
    }
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (hadExistingCache) Storage.remove(backupPath.c_str());
  if (cssParser) {
    cssParser->clear();
  }
  LOG_DBG("SCT", "Create section done: spine=%d pages=%u free=%u maxAlloc=%u", spineIndex, pageCount, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 4)) {
    file.close();
    return nullptr;
  }
  uint32_t lutOffset;
  if (!serialization::tryReadPod(file, lutOffset) || !file.seek(lutOffset + sizeof(uint32_t) * currentPage)) {
    file.close();
    return nullptr;
  }
  uint32_t pagePos;
  if (!serialization::tryReadPod(file, pagePos) || !file.seek(pagePos)) {
    file.close();
    return nullptr;
  }

  auto page = Page::deserialize(file);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t anchorMapOffset;
  if (!serialization::tryReadPod(f, anchorMapOffset)) {
    return std::nullopt;
  }
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(anchorMapOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    if (!serialization::tryReadString(f, key) || !serialization::tryReadPod(f, page)) {
      return std::nullopt;
    }
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    if (!serialization::tryReadPod(f, pagePIdx)) {
      return std::nullopt;
    }
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t pIdx;
  if (!serialization::tryReadPod(f, pIdx)) {
    return std::nullopt;
  }
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset)) {
    return std::nullopt;
  }
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    if (!serialization::tryReadPod(f, pageLiIdx)) {
      return std::nullopt;
    }
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
