#pragma once

// FreeInk SDK — SD card manager (singleton). Device-agnostic: it knows no board
// names. Two interchangeable backends behind one FsVolume& seam, so every op
// returns ordinary FsFile objects:
//   * SPI / SdFat (default).
//   * Native 4-bit SDMMC (FREEINK_SD_SDMMC, e.g. de-link) — SdFat can't drive
//     SDIO, so a plain FsVolume is mounted on an esp-idf SDMMC block device
//     (src/SdmmcBlockDevice). Requires the build to set USE_BLOCK_DEVICE_INTERFACE=1.
// Boards whose SD rail needs more than a GPIO (e.g. an I2C PMIC) register their
// power-up via setPowerHook(); the manager calls it but stays device-agnostic.
// The public API is identical for both backends, so consumers are unchanged.
//
// Filenames are UTF-8: the library's build hook (inject_build_flags.py)
// forces SdFat's USE_UTF8_LONG_NAMES on for the whole build — without it,
// SdFat mangles any non-ASCII long filename into an unopenable path.

#include <WString.h>
#include <vector>
#include <string>
#include <SdFat.h>
#include <BoardConfig.h>

#if FREEINK_SD_SDMMC
namespace freeink {
class SdmmcBlockDevice;
}
#endif

class SDCardManager {
 public:
  SDCardManager();
  bool begin();
  bool ready() const;
  uint64_t sdTotalBytes();
  uint64_t sdUsedBytes();
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  String readFile(const char* path);
  // The implementation already owns a bounded 1 KiB stack buffer, so using
  // that same size by default cuts SD transactions without increasing memory.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 1024);
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  bool writeFile(const char* path, const String& content);
  bool ensureDirectoryExists(const char* path);

  FsFile open(const char* path, const oflag_t oflag = O_RDONLY) { return vol().open(path, oflag); }
  bool mkdir(const char* path, const bool pFlag = true) { return vol().mkdir(path, pFlag); }
  bool exists(const char* path) { return vol().exists(path); }
  bool remove(const char* path) { return vol().remove(path); }
  bool rmdir(const char* path) { return vol().rmdir(path); }
  bool rename(const char* path, const char* newPath) { return vol().rename(path, newPath); }

  bool openFileForRead(const char* moduleName, const char* path, FsFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForRead(const char* moduleName, const String& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, FsFile& file);
  bool removeDir(const char* path);

  using PowerHook = void (*)();
  void setPowerHook(PowerHook hook) { _powerHook = hook; }

#if FREEINK_SD_SDMMC
  freeink::SdmmcBlockDevice* rawBlockDevice() { return _dev; }
#endif

 static SDCardManager& getInstance() { return instance; }

 private:
  static SDCardManager instance;

  bool initialized = false;
  PowerHook _powerHook = nullptr;

  static constexpr uint32_t USED_BYTES_CACHE_TTL_MS = 20000;
  uint64_t cachedTotalBytes = 0;
  uint64_t cachedUsedBytes = 0;
  uint32_t cachedUsedBytesAt = 0;
  bool cachedUsedBytesValid = false;

#if FREEINK_SD_SDMMC
  FsVolume _vol;
  freeink::SdmmcBlockDevice* _dev = nullptr;
  FsVolume& vol() { return _vol; }
#else
  SdFat sd;
  FsVolume& vol() { return sd; }
#endif
};

#define SdMan SDCardManager::getInstance()