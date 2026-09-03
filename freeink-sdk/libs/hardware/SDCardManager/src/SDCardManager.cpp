#include "SDCardManager.h"

#include <BoardConfig.h>
#include <driver/gpio.h>
#include <SPI.h>

#include <algorithm>
#include <array>

#include "SdmmcBlockDevice.h"  // no-op unless FREEINK_SD_SDMMC

SDCardManager SDCardManager::instance;

#ifdef ENABLE_SERIAL_LOG
#define SD_LOGF(...)                         \
  do {                                       \
    if (Serial) Serial.printf(__VA_ARGS__);  \
  } while (0)
#define SD_LOGLN(message)                    \
  do {                                       \
    if (Serial) Serial.println(message);     \
  } while (0)
#else
#define SD_LOGF(...) ((void)0)
#define SD_LOGLN(message) ((void)0)
#endif

namespace {
uint64_t sumDirectoryFileBytes(FsFile& dir) {
  uint64_t total = 0;
  dir.rewind();
  while (true) {
    FsFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) total += sumDirectoryFileBytes(entry);
    else total += static_cast<uint64_t>(entry.fileSize());
    entry.close();
  }
  return total;
}
}  // namespace

#if FREEINK_SD_SDMMC
SDCardManager::SDCardManager() {}
bool SDCardManager::begin() {
  if (_powerHook) _powerHook();
  if (!_dev) _dev = new freeink::SdmmcBlockDevice();
  if (!_dev->begin(BoardConfig::ACTIVE.sdmmc)) {
    SD_LOGF("[%lu] [SD] SDMMC init failed\n", millis());
    initialized = false; cachedTotalBytes = 0; cachedUsedBytesValid = false; return false;
  }
  if (!_vol.begin(_dev)) {
    SD_LOGF("[%lu] [SD] SDMMC volume mount failed\n", millis());
    initialized = false; cachedTotalBytes = 0; cachedUsedBytesValid = false; return false;
  }
  SD_LOGF("[%lu] [SD] SDMMC card mounted\n", millis());
  initialized = true;
  cachedTotalBytes = static_cast<uint64_t>(vol().clusterCount()) * vol().bytesPerCluster();
  cachedUsedBytesValid = false;
  return initialized;
}
#else
SDCardManager::SDCardManager() : sd() {}
bool SDCardManager::begin() {
  if (BoardConfig::ACTIVE.sd.cs < 0) {
    SD_LOGF("[%lu] [SD] SD disabled: CS unassigned in the %s profile\n", millis(), BoardConfig::ACTIVE.name);
    initialized = false; cachedTotalBytes = 0; cachedUsedBytesValid = false; return false;
  }
  const uint8_t SD_CS = BoardConfig::ACTIVE.sd.cs;
  const int8_t SD_SCLK = BoardConfig::ACTIVE.sd.sclk >= 0 ? BoardConfig::ACTIVE.sd.sclk : (BoardConfig::ACTIVE.sd.separateSpi ? -1 : BoardConfig::ACTIVE.display.sclk);
  const int8_t SD_MOSI = BoardConfig::ACTIVE.sd.mosi >= 0 ? BoardConfig::ACTIVE.sd.mosi : (BoardConfig::ACTIVE.sd.separateSpi ? -1 : BoardConfig::ACTIVE.display.mosi);
  const int8_t SD_MISO = BoardConfig::ACTIVE.sd.miso;
  const uint32_t SPI_FQ = BoardConfig::ACTIVE.sd.spiHz != 0 ? BoardConfig::ACTIVE.sd.spiHz : 40000000;
  if (_powerHook) _powerHook();
  if (BoardConfig::ACTIVE.sd.powerEnable >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(BoardConfig::ACTIVE.sd.powerEnable));
    pinMode(BoardConfig::ACTIVE.sd.powerEnable, OUTPUT);
    digitalWrite(BoardConfig::ACTIVE.sd.powerEnable, BoardConfig::ACTIVE.sd.powerActiveHigh ? HIGH : LOW);
    delay(10);
  }
  if (BoardConfig::ACTIVE.display.cs >= 0 && BoardConfig::ACTIVE.display.sclk == SD_SCLK) {
    pinMode(BoardConfig::ACTIVE.display.cs, OUTPUT);
    digitalWrite(BoardConfig::ACTIVE.display.cs, HIGH);
  }
  if (SD_SCLK >= 0 && SD_MOSI >= 0 && SD_MISO >= 0) SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!sd.begin(SD_CS, SPI_FQ)) {
    SD_LOGF("[%lu] [SD] SD card not detected (err=0x%02X data=0x%02X cs=%d sclk=%d miso=%d mosi=%d clk=%luHz)\n", millis(), sd.sdErrorCode(), sd.sdErrorData(), SD_CS, SD_SCLK, SD_MISO, SD_MOSI, (unsigned long)SPI_FQ);
    initialized = false; cachedTotalBytes = 0; cachedUsedBytesValid = false;
  } else {
    SD_LOGF("[%lu] [SD] SD card detected\n", millis());
    initialized = true;
    cachedTotalBytes = static_cast<uint64_t>(vol().clusterCount()) * vol().bytesPerCluster();
    cachedUsedBytesValid = false;
  }
  return initialized;
}
#endif

bool SDCardManager::ready() const { return initialized; }

std::vector<String> SDCardManager::listFiles(const char* path, const int maxFiles) {
  std::vector<String> ret;
  if (!initialized) { SD_LOGF("[%lu] [SD] not initialized, returning empty list\n", millis()); return ret; }
  auto root = vol().open(path);
  if (!root) { SD_LOGF("[%lu] [SD] Failed to open directory\n", millis()); return ret; }
  if (!root.isDirectory()) { SD_LOGF("[%lu] [SD] Path is not a directory\n", millis()); root.close(); return ret; }
  int count = 0; char name[128];
  for (auto f = root.openNextFile(); f && count < maxFiles; f = root.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    f.getName(name, sizeof(name)); ret.emplace_back(name); f.close(); count++;
  }
  root.close(); return ret;
}

String SDCardManager::readFile(const char* path) {
  if (!initialized) { SD_LOGF("[%lu] [SD] not initialized; cannot read file\n", millis()); return {""}; }
  FsFile f;
  if (!openFileForRead("SD", path, f)) return {""};
  constexpr size_t maxSize = 50000;
  const size_t targetSize = std::min<size_t>(static_cast<size_t>(f.fileSize()), maxSize);
  String content; content.reserve(targetSize);
  std::array<uint8_t, 1024> buffer{};
  size_t readSize = 0;
  while (readSize < targetSize) {
    const size_t want = std::min(buffer.size(), targetSize - readSize);
    const int r = f.read(buffer.data(), want);
    if (r <= 0) break;
    if (!content.concat(reinterpret_cast<const char*>(buffer.data()), static_cast<unsigned int>(r))) break;
    readSize += static_cast<size_t>(r);
  }
  f.close(); return content;
}

bool SDCardManager::readFileToStream(const char* path, Print& out, const size_t chunkSize) {
  if (!initialized) { SD_LOGLN("SDCardManager: not initialized; cannot read file"); return false; }
  FsFile f; if (!openFileForRead("SD", path, f)) return false;
  std::array<uint8_t, 1024> buf{};
  const size_t toRead = chunkSize == 0 ? buf.size() : std::min(chunkSize, buf.size());
  bool success = true;
  while (f.available()) {
    const int r = f.read(buf.data(), toRead);
    if (r <= 0 || out.write(buf.data(), static_cast<size_t>(r)) != static_cast<size_t>(r)) { success = false; break; }
  }
  f.close(); return success;
}

size_t SDCardManager::readFileToBuffer(const char* path, char* buffer, const size_t bufferSize, const size_t maxBytes) {
  if (!buffer || bufferSize == 0) return 0;
  if (!initialized) { SD_LOGLN("SDCardManager: not initialized; cannot read file"); buffer[0] = '\0'; return 0; }
  FsFile f; if (!openFileForRead("SD", path, f)) { buffer[0] = '\0'; return 0; }
  const size_t maxToRead = (maxBytes == 0) ? (bufferSize - 1) : min(maxBytes, bufferSize - 1);
  size_t total = 0;
  while (total < maxToRead) {
    const int r = f.read(buffer + total, maxToRead - total);
    if (r <= 0) break;
    total += static_cast<size_t>(r);
  }
  buffer[total] = '\0'; f.close(); return total;
}

bool SDCardManager::writeFile(const char* path, const String& content) {
  if (!initialized) { SD_LOGLN("SDCardManager: not initialized; cannot write file"); return false; }
  FsFile f;
  if (!openFileForWrite("SD", path, f)) { SD_LOGF("Failed to open file for write: %s\n", path); return false; }
  const size_t written = f.print(content);
  const bool closed = f.close();
  return written == content.length() && closed;
}

bool SDCardManager::ensureDirectoryExists(const char* path) {
  if (!initialized) { SD_LOGLN("SDCardManager: not initialized; cannot create directory"); return false; }

  FsFile dir = vol().open(path, O_RDONLY);
  if (dir) {
    const bool isDir = dir.isDirectory();
    dir.close();
    if (isDir) return true;
  }

  if (vol().mkdir(path)) return true;
  SD_LOGF("Failed to create directory: %s\n", path);
  return false;
}

bool SDCardManager::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
  file = vol().open(path, O_RDONLY);
  if (!file) { SD_LOGF("[%lu] [%s] Failed to open file for reading: %s\n", millis(), moduleName, path); return false; }
  return true;
}
bool SDCardManager::openFileForRead(const char* moduleName, const std::string& path, FsFile& file) { return openFileForRead(moduleName, path.c_str(), file); }
bool SDCardManager::openFileForRead(const char* moduleName, const String& path, FsFile& file) { return openFileForRead(moduleName, path.c_str(), file); }

bool SDCardManager::openFileForWrite(const char* moduleName, const char* path, FsFile& file) {
  file = vol().open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) { SD_LOGF("[%lu] [%s] Failed to open file for writing: %s\n", millis(), moduleName, path); return false; }
  return true;
}
bool SDCardManager::openFileForWrite(const char* moduleName, const std::string& path, FsFile& file) { return openFileForWrite(moduleName, path.c_str(), file); }
bool SDCardManager::openFileForWrite(const char* moduleName, const String& path, FsFile& file) { return openFileForWrite(moduleName, path.c_str(), file); }

uint64_t SDCardManager::sdTotalBytes() {
  if (!initialized) return 0;
  if (cachedTotalBytes == 0) {
    const uint64_t clusters = vol().clusterCount(); const uint64_t bytesPerCluster = vol().bytesPerCluster();
    if (clusters != 0 && bytesPerCluster != 0) cachedTotalBytes = clusters * bytesPerCluster;
  }
  if (cachedTotalBytes == 0) {
#if FREEINK_SD_SDMMC
    if (_dev) cachedTotalBytes = static_cast<uint64_t>(_dev->sectorCount()) * 512ULL;
#else
    if (sd.card()) cachedTotalBytes = static_cast<uint64_t>(sd.card()->sectorCount()) * 512ULL;
#endif
  }
  return cachedTotalBytes;
}

uint64_t SDCardManager::sdUsedBytes() {
  if (!initialized) return 0;
  const uint32_t now = millis();
  if (!cachedUsedBytesValid || (now - cachedUsedBytesAt) >= USED_BYTES_CACHE_TTL_MS) {
    uint64_t used = 0; FsFile root = vol().open("/");
    if (root && root.isDirectory()) { used = sumDirectoryFileBytes(root); root.close(); }
    cachedUsedBytes = used; cachedUsedBytesValid = true; cachedUsedBytesAt = now;
    SD_LOGF("[%lu] [SD] Used-space scan: %llu bytes\n", millis(), static_cast<unsigned long long>(cachedUsedBytes));
  }
  return cachedUsedBytes;
}

bool SDCardManager::removeDir(const char* path) {
  FsFile dir = vol().open(path);
  if (!dir) return false;
  if (!dir.isDirectory()) {
    dir.close();
    return false;
  }

  char name[128];
  while (true) {
    FsFile file = dir.openNextFile();
    if (!file) break;

    String filePath = path;
    if (!filePath.endsWith("/")) filePath += "/";
    file.getName(name, sizeof(name));
    filePath += name;
    const bool isDirectory = file.isDirectory();

    // Close the child before deleting/re-entering the directory.  The build also
    // enables destructor-close, but deterministic closure here avoids carrying
    // an extra descriptor through recursive deletion and makes error paths safe
    // even if that SdFat option changes later.
    file.close();

    const bool ok = isDirectory ? removeDir(filePath.c_str()) : vol().remove(filePath.c_str());
    if (!ok) {
      dir.close();
      return false;
    }
  }

  dir.close();
  return vol().rmdir(path);
}

#undef SD_LOGF
#undef SD_LOGLN
