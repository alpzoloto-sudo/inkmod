#pragma once
#include <HalStorage.h>

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

namespace reader {
class ReaderCancellationToken;
}

class ZipFile {
 public:
  struct FileStatSlim {
    uint16_t method;             // Compression method
    uint32_t compressedSize;     // Compressed size
    uint32_t uncompressedSize;   // Uncompressed size
    uint32_t localHeaderOffset;  // Offset of local file header
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint16_t totalEntries;
    bool isSet;
  };

  // Target for batch uncompressed size lookup (sorted by hash, then len)
  struct SizeTarget {
    uint64_t hash;   // FNV-1a 64-bit hash of normalized path
    uint16_t len;    // Length of path for collision reduction
    uint16_t index;  // Caller's index (e.g. spine index)
  };

  // FNV-1a 64-bit hash computed from char buffer (no std::string allocation)
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  const std::string& filePath;
  HalFile file;
  ZipDetails zipDetails = {0, 0, false};
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();

 public:
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  ~ZipFile() = default;
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  bool getInflatedFileSize(const char* filename, size_t* size);
  // Batch lookup: scan ZIP central dir once and fill sizes for matching targets.
  // targets must be sorted by (hash, len). sizes[target.index] receives uncompressedSize.
  // Returns number of targets matched.
  int fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize,
                        const reader::ReaderCancellationToken* cancellationToken = nullptr);

  template <typename F>
  bool enumerateFilePaths(F&& callback) {
    if (!fileStatSlimCache.empty()) {
      for (const auto& entry : fileStatSlimCache) {
        callback(std::string_view{entry.first});
      }
      return true;
    }

    const bool wasOpen = isOpen();
    if (!wasOpen && !open()) {
      return false;
    }

    if (!loadZipDetails()) {
      if (!wasOpen) {
        close();
      }
      return false;
    }

    file.seek(zipDetails.centralDirOffset);

    constexpr size_t centralHeaderSize = 46;
    uint8_t header[centralHeaderSize];
    char itemName[256];

    auto readLe16 = [](const uint8_t* p) -> uint16_t {
      return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    };
    auto readLe32 = [](const uint8_t* p) -> uint32_t {
      return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
             (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    };

    // Central-directory entries have a fixed 46-byte prefix. Read that prefix
    // once instead of issuing several tiny read()/seekCur() operations for the
    // signature and length fields. EPUB discovery can touch hundreds of ZIP
    // entries, so this substantially reduces SD/SPI transactions without
    // changing enumeration semantics or retaining extra heap memory.
    while (file.read(header, sizeof(header)) == static_cast<int>(sizeof(header))) {
      if (readLe32(header) != 0x02014b50) {
        break;
      }

      const uint16_t nameLen = readLe16(header + 28);
      const uint16_t extraLen = readLe16(header + 30);
      const uint16_t commentLen = readLe16(header + 32);

      if (nameLen < sizeof(itemName)) {
        if (file.read(itemName, nameLen) != static_cast<int>(nameLen)) break;
        itemName[nameLen] = '\0';
        callback(std::string_view{itemName, nameLen});
      } else if (!file.seekCur(nameLen)) {
        break;
      }

      if (!file.seekCur(static_cast<int64_t>(extraLen) + commentLen)) break;
    }

    if (!wasOpen) {
      close();
    }
    return true;
  }
};