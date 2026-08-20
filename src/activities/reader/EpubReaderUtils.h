#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <string>

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
};

inline bool readProgressFile(const char* moduleName, const std::string& path, Progress& progress) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead(moduleName, path, f)) {
    return false;
  }

  uint8_t data[6];
  const int dataSize = f.read(data, sizeof(data));
  f.close();
  if (dataSize != 4 && dataSize != 6) {
    LOG_ERR(moduleName, "Progress file has unexpected size: %d", dataSize);
    return false;
  }

  progress.spineIndex = static_cast<int>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
  progress.pageNumber = static_cast<int>(static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8));
  if (progress.pageNumber == UINT16_MAX) {
    progress.pageNumber = 0;
  }
  if (dataSize == 6) {
    progress.pageCount = static_cast<int>(static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8));
    progress.hasPageCount = true;
  } else {
    progress.pageCount = 0;
    progress.hasPageCount = false;
  }
  return true;
}

inline bool loadProgress(const Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  if (readProgressFile(moduleName, progressPath, progress)) {
    return true;
  }

  const std::string backupPath = progressPath + ".bak";
  if (readProgressFile(moduleName, backupPath, progress)) {
    LOG_DBG("ERS", "Recovered progress from backup");
    return true;
  }
  return false;
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  const std::string tmpPath = progressPath + ".tmp";
  const std::string backupPath = progressPath + ".bak";

  FsFile f;
  // openFileForWrite uses O_CREAT | O_TRUNC, so it safely replaces a stale
  // progress temp file left by a reset. Avoid the previous exists()+remove()
  // metadata round-trip on every page turn.
  if (!Storage.openFileForWrite("ERS", tmpPath, f)) {
    LOG_ERR("ERS", "Could not open progress temp file for write!");
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    f.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  // SdFat FatFile::close() and ExFatFile::close() both call sync() internally.
  // An explicit sync here therefore wrote the same six-byte update twice on
  // every page turn. close() preserves the durability guarantee and reports
  // sync failure, so one storage flush is sufficient before atomic rotation.
  if (!f.close()) {
    LOG_ERR("ERS", "Failed to close/sync progress temp file");
    Storage.remove(tmpPath.c_str());
    return false;
  }

  // The backup normally exists after the first saved page. Try the operation
  // first and only probe metadata when it fails. This preserves the old error
  // detection while removing one exists() call from the common path.
  if (!Storage.remove(backupPath.c_str()) && Storage.exists(backupPath.c_str())) {
    LOG_ERR("ERS", "Could not remove old progress backup");
    Storage.remove(tmpPath.c_str());
    return false;
  }

  // Same idea for the current progress file: a successful rename needs no
  // preceding exists() query. On the very first save rename() may fail because
  // there is no old progress file; only then do we check whether that is the
  // harmless missing-file case or a real rename failure.
  if (!Storage.rename(progressPath.c_str(), backupPath.c_str()) && Storage.exists(progressPath.c_str())) {
    LOG_ERR("ERS", "Could not rotate progress backup");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), progressPath.c_str())) {
    LOG_ERR("ERS", "Could not replace progress file");
    if (Storage.exists(backupPath.c_str()) && !Storage.exists(progressPath.c_str())) {
      Storage.rename(backupPath.c_str(), progressPath.c_str());
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
