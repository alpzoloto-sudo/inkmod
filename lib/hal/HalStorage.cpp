#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <FS.h>
#include <Logging.h>
#include <SDCardManager.h>

#include <cassert>
#include <new>

#include "HalSpiBus.h"

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  storageMutex = xSemaphoreCreateMutex();
  assert(storageMutex != nullptr);
}

bool HalStorage::begin() {
  HalSpiBus::Lock spiLock;
  return SDCard.begin();
}

bool HalStorage::ready() const { return SDCard.ready(); }

class HalStorage::StorageLock {
 public:
  StorageLock() : spiLock() { xSemaphoreTake(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGive(HalStorage::getInstance().storageMutex); }

 private:
  HalSpiBus::Lock spiLock;
};

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  FsFile file;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() { close(); }
HalFile::HalFile(HalFile&&) = default;

HalFile& HalFile::operator=(HalFile&& other) {
  if (this == &other) return *this;
  close();
  impl = std::move(other.impl);
  return *this;
}

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;
  FsFile fsFile = SDCard.open(path, oflag);
  if (!fsFile) return HalFile();
  auto fileImpl = std::unique_ptr<HalFile::Impl>(new (std::nothrow) HalFile::Impl(std::move(fsFile)));
  if (!fileImpl) {
    LOG_ERR("HAL", "OOM allocating file handle: %s", path ? path : "?");
    return HalFile();
  }
  return HalFile(std::move(fileImpl));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }
bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }
bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) { HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath); }
bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  FsFile fsFile;
  bool ok = false;
  {
    StorageLock lock;
    ok = SDCard.openFileForRead(moduleName, path, fsFile);
  }
  if (ok && fsFile) {
    auto fileImpl = std::unique_ptr<HalFile::Impl>(new (std::nothrow) HalFile::Impl(std::move(fsFile)));
    if (fileImpl) {
      file = HalFile(std::move(fileImpl));
    } else {
      LOG_ERR("HAL", "OOM allocating read handle: %s", path ? path : "?");
      file = HalFile();
      ok = false;
    }
  } else {
    file = HalFile();
  }
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  FsFile fsFile;
  bool ok = false;
  {
    StorageLock lock;
    ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  }
  if (ok && fsFile) {
    auto fileImpl = std::unique_ptr<HalFile::Impl>(new (std::nothrow) HalFile::Impl(std::move(fsFile)));
    if (fileImpl) {
      file = HalFile(std::move(fileImpl));
    } else {
      LOG_ERR("HAL", "OOM allocating write handle: %s", path ? path : "?");
      file = HalFile();
      ok = false;
    }
  } else {
    file = HalFile();
  }
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }
uint64_t HalStorage::getCardTotalBytes() { HAL_STORAGE_WRAPPED_CALL(sdTotalBytes); }
uint64_t HalStorage::getCardUsedBytes() { HAL_STORAGE_WRAPPED_CALL(sdUsedBytes); }

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_FORWARD_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_FORWARD_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::sync() { HAL_FILE_WRAPPED_CALL(sync, ); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }

bool HalFile::close() {
  if (!impl) return true;
  HalStorage::StorageLock lock;
  const bool ok = impl->file.close();
  impl.reset();
  return ok;
}

HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  FsFile next = impl->file.openNextFile();
  if (!next) return HalFile();
  auto fileImpl = std::unique_ptr<Impl>(new (std::nothrow) Impl(std::move(next)));
  if (!fileImpl) {
    LOG_ERR("HAL", "OOM allocating directory file handle");
    return HalFile();
  }
  return HalFile(std::move(fileImpl));
}

bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }
HalFile::operator bool() const { return isOpen(); }
