#include "ZipFile.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <ReaderWork.h>

#include <algorithm>

struct ZipInflateCtx {
  InflateReader reader;  // Must be first — callback casts uzlib_uncomp* to ZipInflateCtx*
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;
constexpr size_t ZIP_MIN_INPUT_CHUNK = 4096;
constexpr size_t ZIP_CENTRAL_HEADER_SIZE = 46;
constexpr uint32_t ZIP_CENTRAL_HEADER_SIGNATURE = 0x02014b50;

uint16_t readLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool readCentralHeader(HalFile& file, uint8_t (&header)[ZIP_CENTRAL_HEADER_SIZE]) {
  return file.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
         readLe32(header) == ZIP_CENTRAL_HEADER_SIGNATURE;
}

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

int zipReadCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<ZipInflateCtx*>(uncomp);
  if (ctx->fileRemaining == 0) return -1;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const size_t bytesRead = ctx->file->read(ctx->readBuf, toRead);
  ctx->fileRemaining -= bytesRead;

  if (bytesRead == 0) return -1;

  uncomp->source = ctx->readBuf + 1;
  uncomp->source_limit = ctx->readBuf + bytesRead;
  return ctx->readBuf[0];
}
}  // namespace

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  file.seek(zipDetails.centralDirOffset);

  uint8_t header[ZIP_CENTRAL_HEADER_SIZE];
  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  // A central-directory entry starts with a fixed 46-byte header. The old
  // implementation fetched its fields through many 2/4-byte read()+seekCur()
  // calls, each taking the storage/SPI lock. Read the fixed part once and parse
  // it from RAM instead; only the variable file name and final skip touch the
  // file again.
  while (readCentralHeader(file, header)) {
    FileStatSlim fileStat = {};
    fileStat.method = readLe16(header + 10);
    fileStat.compressedSize = readLe32(header + 20);
    fileStat.uncompressedSize = readLe32(header + 24);
    const uint16_t nameLen = readLe16(header + 28);
    const uint16_t extraLen = readLe16(header + 30);
    const uint16_t commentLen = readLe16(header + 32);
    fileStat.localHeaderOffset = readLe32(header + 42);

    if (nameLen < sizeof(itemName)) {
      if (file.read(itemName, nameLen) != static_cast<int>(nameLen)) break;
      itemName[nameLen] = '\0';
      fileStatSlimCache.emplace(itemName, fileStat);
    } else if (!file.seekCur(nameLen)) {
      break;
    }

    if (!file.seekCur(static_cast<int64_t>(extraLen) + commentLen)) break;
  }

  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  file.seek(startPos);

  uint8_t header[ZIP_CENTRAL_HEADER_SIZE];
  char itemName[256];

  while (true) {
    const uint32_t entryStart = file.position();

    if (!readCentralHeader(file, header)) {
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        file.seek(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    if (wrapped && entryStart >= startPos) {
      break;
    }

    fileStat->method = readLe16(header + 10);
    fileStat->compressedSize = readLe32(header + 20);
    fileStat->uncompressedSize = readLe32(header + 24);
    const uint16_t nameLen = readLe16(header + 28);
    const uint16_t extraLen = readLe16(header + 30);
    const uint16_t commentLen = readLe16(header + 32);
    fileStat->localHeaderOffset = readLe32(header + 42);

    if (nameLen < sizeof(itemName)) {
      if (file.read(itemName, nameLen) != static_cast<int>(nameLen)) break;
      itemName[nameLen] = '\0';

      if (strcmp(itemName, filename) == 0) {
        if (!file.seekCur(static_cast<int64_t>(extraLen) + commentLen)) break;
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }
    } else if (!file.seekCur(nameLen)) {
      break;
    }

    if (!file.seekCur(static_cast<int64_t>(extraLen) + commentLen)) break;
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (readLe32(pLocalHeader) != 0x04034b50) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = readLe16(pLocalHeader + 26);
  const uint16_t extraOffset = readLe16(pLocalHeader + 28);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  constexpr size_t EOCD_MIN_SIZE = 22;
  constexpr size_t EOCD_MAX_COMMENT = 0xFFFF;
  constexpr size_t EOCD_MAX_SEARCH = EOCD_MIN_SIZE + EOCD_MAX_COMMENT;
  constexpr size_t EOCD_SCAN_CHUNK = 1024;
  constexpr uint32_t EOCD_SIGNATURE = 0x06054b50;

  if (fileSize < EOCD_MIN_SIZE) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;
  }

  const size_t searchStart = fileSize > EOCD_MAX_SEARCH ? fileSize - EOCD_MAX_SEARCH : 0;
  size_t windowEnd = fileSize;
  uint8_t buffer[EOCD_SCAN_CHUNK];
  uint8_t record[EOCD_MIN_SIZE];

  while (windowEnd > searchStart) {
    const size_t windowStart = windowEnd - searchStart > EOCD_SCAN_CHUNK ? windowEnd - EOCD_SCAN_CHUNK : searchStart;
    const size_t readLen = windowEnd - windowStart;
    if (!file.seek(windowStart) || file.read(buffer, readLen) != static_cast<int>(readLen)) {
      LOG_ERR("ZIP", "Failed to read EOCD scan window");
      return false;
    }

    if (readLen >= 4) {
      for (int i = static_cast<int>(readLen) - 4; i >= 0; --i) {
        if (readLe32(buffer + i) != EOCD_SIGNATURE) continue;

        const size_t absoluteOffset = windowStart + static_cast<size_t>(i);
        if (absoluteOffset + EOCD_MIN_SIZE > fileSize) continue;

        if (static_cast<size_t>(i) + EOCD_MIN_SIZE <= readLen) {
          std::copy_n(buffer + i, EOCD_MIN_SIZE, record);
        } else {
          if (!file.seek(absoluteOffset) || file.read(record, sizeof(record)) != static_cast<int>(sizeof(record))) {
            continue;
          }
        }

        const uint16_t commentLen = readLe16(record + 20);
        if (absoluteOffset + EOCD_MIN_SIZE + commentLen != fileSize) continue;

        const uint32_t centralDirSize = readLe32(record + 12);
        const uint32_t centralDirOffset = readLe32(record + 16);
        if (centralDirOffset > absoluteOffset ||
            static_cast<uint64_t>(centralDirOffset) + centralDirSize > absoluteOffset) {
          continue;
        }

        zipDetails.totalEntries = readLe16(record + 10);
        zipDetails.centralDirOffset = centralDirOffset;
        zipDetails.isSet = true;
        return true;
      }
    }

    if (windowStart == searchStart) break;
    windowEnd = windowStart + 3;
  }

  LOG_ERR("ZIP", "EOCD signature not found in zip file");
  return false;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint8_t header[ZIP_CENTRAL_HEADER_SIZE];
  char itemName[256];

  while (readCentralHeader(file, header)) {
    const uint32_t uncompressedSize = readLe32(header + 24);
    const uint16_t nameLen = readLe16(header + 28);
    const uint16_t extraLen = readLe16(header + 30);
    const uint16_t commentLen = readLe16(header + 32);

    if (nameLen < sizeof(itemName)) {
      if (file.read(itemName, nameLen) != static_cast<int>(nameLen)) break;
      itemName[nameLen] = '\0';

      const uint64_t hash = fnvHash64(itemName, nameLen);
      const SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else if (!file.seekCur(nameLen)) {
      break;
    }

    if (!file.seekCur(static_cast<int64_t>(extraLen) + commentLen)) break;
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    const auto deflatedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
    if (deflatedData == nullptr) {
      LOG_ERR("ZIP", "Failed to allocate memory for decompression buffer");
      free(data);
      return nullptr;
    }

    const size_t dataRead = file.read(deflatedData, deflatedDataSize);

    if (dataRead != deflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data, expected %d got %d", deflatedDataSize, dataRead);
      free(deflatedData);
      free(data);
      return nullptr;
    }

    bool success = false;
    {
      InflateReader r;
      r.init(false);
      r.setSource(deflatedData, deflatedDataSize);
      success = r.read(data, inflatedDataSize);
    }
    free(deflatedData);

    if (!success) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(data);
      return nullptr;
    }
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize,
                               const reader::ReaderCancellationToken* cancellationToken) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;

  if (fileStat.method == ZIP_METHOD_STORED) {
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      if (cancellationToken && cancellationToken->isCancellationRequested()) {
        LOG_INF("ZIP", "Stream extraction cancelled");
        free(buffer);
        return false;
      }
      const size_t dataRead = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
      if (dataRead == 0) {
        LOG_ERR("ZIP", "Could not read more bytes");
        free(buffer);
        return false;
      }

      if (out.write(buffer, dataRead) != dataRead) {
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        free(buffer);
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;

    if (!ctx.reader.init(true)) {
      LOG_ERR("ZIP", "Failed to init inflate reader (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap(), chunkSize);
      return false;
    }

    const size_t inputChunkSize = std::max(chunkSize, ZIP_MIN_INPUT_CHUNK);
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(inputChunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer (free=%u, maxAlloc=%u, chunk=%zu)",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap(), inputChunkSize);
      return false;
    }

    auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap(), chunkSize);
      free(fileReadBuffer);
      return false;
    }

    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = inputChunkSize;
    ctx.reader.setReadCallback(zipReadCallback);

    bool success = false;
    size_t totalProduced = 0;

    while (true) {
      if (cancellationToken && cancellationToken->isCancellationRequested()) {
        LOG_INF("ZIP", "Stream inflation cancelled after %zu bytes", totalProduced);
        break;
      }
      size_t produced;
      const InflateStatus status = ctx.reader.readAtMost(outputBuffer, chunkSize, &produced);

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        if (out.write(outputBuffer, produced) != produced) {
          LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          break;
        }
      }

      if (status == InflateStatus::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStatus::Error) {
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
    }

    free(outputBuffer);
    free(fileReadBuffer);
    return success;
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}
