#pragma once

#include <HalStorage.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <vector>

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
  };

  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize;  // cumulative size stored as 32-bit for on-disk format compatibility
    int16_t tocIndex;

    SpineEntry() : cumulativeSize(0), tocIndex(-1) {}
    SpineEntry(std::string href, const uint32_t cumulativeSize, const int16_t tocIndex)
        : href(std::move(href)), cumulativeSize(cumulativeSize), tocIndex(tocIndex) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
    TocEntry(std::string title, std::string href, std::string anchor, const uint8_t level, const int16_t spineIndex)
        : title(std::move(title)),
          href(std::move(href)),
          anchor(std::move(anchor)),
          level(level),
          spineIndex(spineIndex) {}
  };

 private:
  std::string cachePath;
  uint32_t lutOffset;
  uint16_t spineCount;
  uint16_t tocCount;
  bool loaded;
  bool buildMode;

  HalFile bookFile;
  // Temp file handles during build
  HalFile spineFile;
  HalFile tocFile;
  // OPF/TOC parsers emit many tiny fields. Buffer their temporary sequential
  // files to avoid taking the SD/storage lock for every 1-4 byte write.
  std::vector<uint8_t> spineWriteBuffer;
  std::vector<uint8_t> tocWriteBuffer;
  bool tempWriteFailed = false;

  // Index for fast href→spineIndex lookup. It is compact and only lives while
  // building the TOC, so enabling it for medium books avoids repeated O(n*m)
  // SD scans without a meaningful steady-state RAM cost.
  struct SpineHrefIndexEntry {
    uint64_t hrefHash;  // FNV-1a 64-bit hash
    uint16_t hrefLen;   // length for collision reduction
    int16_t spineIndex;
  };
  std::deque<SpineHrefIndexEntry> spineHrefIndex;
  bool useSpineHrefIndex = false;

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 64;

  // FNV-1a 64-bit hash function
  static uint64_t fnvHash64(const std::string& s) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  uint32_t writeSpineEntry(HalFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(HalFile& file, const TocEntry& entry) const;
  bool appendTempBytes(HalFile& file, std::vector<uint8_t>& buffer, const void* data, size_t length);
  bool flushTempBuffer(HalFile& file, std::vector<uint8_t>& buffer);
  bool writeTempString(HalFile& file, std::vector<uint8_t>& buffer, const std::string& value);
  SpineEntry readSpineEntry(HalFile& file) const;
  TocEntry readTocEntry(HalFile& file) const;

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)), lutOffset(0), spineCount(0), tocCount(0), loaded(false), buildMode(false) {}
  ~BookMetadataCache() = default;

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href);
  bool endContentOpfPass();
  bool beginTocPass();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  using BuildProgressFn = std::function<void(uint16_t completed, uint16_t total)>;
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata, bool unpackedPackage,
                    const BuildProgressFn& onProgress = nullptr);

  // Cheap check (no parsing) for whether a metadata cache exists at cachePath.
  // Lets callers predict a fast cached open without doing the full load().
  static bool exists(const std::string& cachePath);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  size_t getSpineCumulativeSize(int index);
  TocEntry getTocEntry(int index);
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  bool isLoaded() const { return loaded; }
};