#include "OpdsCacheStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t CACHE_VERSION = 2;
constexpr uint32_t RECORD_MAGIC = 0x3152444Fu;  // ODR1
constexpr size_t MAX_CACHE_STRING = 4096;

bool writePod(FsFile& f, const void* p, size_t n) { return f.write(p, n) == n; }
bool readPod(FsFile& f, void* p, size_t n) { return f.read(p, n) == static_cast<int>(n); }
}

OpdsCacheStore::~OpdsCacheStore() {
  abortWrite();
  closeRead();
}

bool OpdsCacheStore::beginWrite(const std::string& basePath, const std::string& url, const uint32_t urlHash) {
  abortWrite();
  closeRead();
  basePath_ = basePath;
  feedPath_ = basePath_ + ".feed.bin";
  indexPath_ = basePath_ + ".index.bin";
  metaPath_ = basePath_ + ".meta.json";
  feedTmpPath_ = feedPath_ + ".tmp";
  indexTmpPath_ = indexPath_ + ".tmp";
  metaTmpPath_ = metaPath_ + ".tmp";
  Storage.remove(feedTmpPath_.c_str());
  Storage.remove(indexTmpPath_.c_str());
  Storage.remove(metaTmpPath_.c_str());

  if (!Storage.openFileForWrite("OPC", feedTmpPath_, feed_) ||
      !Storage.openFileForWrite("OPC", indexTmpPath_, index_)) {
    abortWrite();
    return false;
  }
  meta_ = Meta{};
  meta_.version = CACHE_VERSION;
  meta_.url = url;
  meta_.urlHash = urlHash;
  writing_ = true;
  return true;
}

bool OpdsCacheStore::writeString(FsFile& file, const std::string& value) {
  const uint16_t len = static_cast<uint16_t>(std::min<size_t>(value.size(), 0xFFFFu));
  return writePod(file, &len, sizeof(len)) && (len == 0 || writePod(file, value.data(), len));
}

bool OpdsCacheStore::readString(FsFile& file, std::string& value) {
  uint16_t len = 0;
  if (!readPod(file, &len, sizeof(len)) || len > MAX_CACHE_STRING) return false;
  value.clear();
  if (!len) return true;
  value.resize(len);
  return file.read(&value[0], len) == static_cast<int>(len);
}

bool OpdsCacheStore::append(OpdsEntry&& entry) {
  if (!writing_) return false;
  const uint32_t offset = static_cast<uint32_t>(feed_.position());
  if (!writePod(index_, &offset, sizeof(offset))) return false;

  const uint32_t magic = RECORD_MAGIC;
  const uint8_t type = static_cast<uint8_t>(entry.type);
  const uint8_t acqCount = std::min<uint8_t>(entry.acquisitionCount, MAX_OPDS_ACQUISITIONS);
  const uint16_t reserved = 0;
  if (!writePod(feed_, &magic, sizeof(magic)) || !writePod(feed_, &type, sizeof(type)) ||
      !writePod(feed_, &acqCount, sizeof(acqCount)) || !writePod(feed_, &reserved, sizeof(reserved)) ||
      !writeString(feed_, entry.title) || !writeString(feed_, entry.author) ||
      !writeString(feed_, entry.description) || !writeString(feed_, entry.href) ||
      !writeString(feed_, entry.id) || !writeString(feed_, entry.navigationHref)) {
    return false;
  }
  for (uint8_t i = 0; i < acqCount; ++i) {
    const uint8_t format = static_cast<uint8_t>(entry.acquisitions[i].format);
    if (!writePod(feed_, &format, sizeof(format)) || !writeString(feed_, entry.acquisitions[i].href)) return false;
  }
  ++meta_.entryCount;
  return true;
}

bool OpdsCacheStore::writeMeta() {
  JsonDocument doc;
  doc["version"] = meta_.version;
  doc["urlHash"] = meta_.urlHash;
  doc["entryCount"] = meta_.entryCount;
  doc["url"] = meta_.url;
  doc["searchTemplate"] = meta_.searchTemplate;
  doc["nextPageUrl"] = meta_.nextPageUrl;
  doc["prevPageUrl"] = meta_.prevPageUrl;

  FsFile f;
  if (!Storage.openFileForWrite("OPC", metaTmpPath_, f)) return false;
  const size_t written = serializeJson(doc, f);
  f.flush();
  f.close();
  return written > 0;
}

bool OpdsCacheStore::finishWrite(const std::string& searchTemplate, const std::string& nextPageUrl,
                                 const std::string& prevPageUrl) {
  if (!writing_) return false;
  meta_.searchTemplate = searchTemplate;
  meta_.nextPageUrl = nextPageUrl;
  meta_.prevPageUrl = prevPageUrl;
  feed_.flush();
  index_.flush();
  feed_.close();
  index_.close();
  writing_ = false;
  if (!writeMeta()) {
    abortWrite();
    return false;
  }

  Storage.remove(feedPath_.c_str());
  Storage.remove(indexPath_.c_str());
  Storage.remove(metaPath_.c_str());
  if (!Storage.rename(feedTmpPath_.c_str(), feedPath_.c_str()) ||
      !Storage.rename(indexTmpPath_.c_str(), indexPath_.c_str()) ||
      !Storage.rename(metaTmpPath_.c_str(), metaPath_.c_str())) {
    return false;
  }
  LOG_INF("OPC", "OPDS cache committed: entries=%zu", meta_.entryCount);
  return true;
}

void OpdsCacheStore::abortWrite() {
  // Do not key ownership off writing_: beginWrite() can successfully open the
  // feed and then fail opening the index before writing_ becomes true.
  // HalFile::close() is idempotent, so close whichever members are live.
  if (feed_) feed_.close();
  if (index_) index_.close();
  writing_ = false;
  if (!feedTmpPath_.empty()) Storage.remove(feedTmpPath_.c_str());
  if (!indexTmpPath_.empty()) Storage.remove(indexTmpPath_.c_str());
  if (!metaTmpPath_.empty()) Storage.remove(metaTmpPath_.c_str());
}

bool OpdsCacheStore::readMeta() {
  FsFile f;
  if (!Storage.openFileForRead("OPC", metaPath_, f)) return false;
  JsonDocument doc;
  const auto err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  meta_.version = doc["version"] | 0u;
  meta_.urlHash = doc["urlHash"] | 0u;
  meta_.entryCount = doc["entryCount"] | 0u;
  meta_.url = (const char*)(doc["url"] | "");
  meta_.searchTemplate = (const char*)(doc["searchTemplate"] | "");
  meta_.nextPageUrl = (const char*)(doc["nextPageUrl"] | "");
  meta_.prevPageUrl = (const char*)(doc["prevPageUrl"] | "");
  return meta_.version == CACHE_VERSION;
}

bool OpdsCacheStore::openRead(const std::string& basePath) {
  closeRead();
  basePath_ = basePath;
  feedPath_ = basePath_ + ".feed.bin";
  indexPath_ = basePath_ + ".index.bin";
  metaPath_ = basePath_ + ".meta.json";
  if (!readMeta()) return false;
  if (!Storage.openFileForRead("OPC", feedPath_, feed_) || !Storage.openFileForRead("OPC", indexPath_, index_)) {
    closeRead();
    return false;
  }
  if (index_.fileSize() < meta_.entryCount * sizeof(uint32_t)) {
    closeRead();
    return false;
  }
  reading_ = true;
  return true;
}

void OpdsCacheStore::closeRead() {
  // Same partial-open case as beginWrite(): openRead() may open feed_ and fail
  // on index_ before reading_ is set.  Always close the actual handles.
  if (feed_) feed_.close();
  if (index_) index_.close();
  reading_ = false;

  // closeRead() is also the ownership boundary between catalogue pages.
  // Merely clear()ing/overwriting these strings leaves their capacities in
  // heap and, after a few feeds, fragments the ESP32-C3 badly enough that TLS
  // cannot obtain a 30-50 KB contiguous block.  Drop the capacities outright.
  std::string().swap(basePath_);
  std::string().swap(feedPath_);
  std::string().swap(indexPath_);
  std::string().swap(metaPath_);
  std::string().swap(feedTmpPath_);
  std::string().swap(indexTmpPath_);
  std::string().swap(metaTmpPath_);
  Meta emptyMeta;
  std::swap(meta_, emptyMeta);
}

bool OpdsCacheStore::readEntry(const size_t index, OpdsEntry& out) {
  if (!reading_ || index >= meta_.entryCount) return false;
  if (!index_.seek(index * sizeof(uint32_t))) return false;
  uint32_t offset = 0;
  if (!readPod(index_, &offset, sizeof(offset)) || !feed_.seek(offset)) return false;
  uint32_t magic = 0;
  uint8_t type = 0, acqCount = 0;
  uint16_t reserved = 0;
  if (!readPod(feed_, &magic, sizeof(magic)) || magic != RECORD_MAGIC ||
      !readPod(feed_, &type, sizeof(type)) || !readPod(feed_, &acqCount, sizeof(acqCount)) ||
      !readPod(feed_, &reserved, sizeof(reserved))) return false;
  out = OpdsEntry{};
  out.type = static_cast<OpdsEntryType>(type);
  if (!readString(feed_, out.title) || !readString(feed_, out.author) || !readString(feed_, out.description) ||
      !readString(feed_, out.href) || !readString(feed_, out.id) || !readString(feed_, out.navigationHref)) return false;
  out.acquisitionCount = std::min<uint8_t>(acqCount, MAX_OPDS_ACQUISITIONS);
  for (uint8_t i = 0; i < acqCount; ++i) {
    uint8_t format = 0;
    std::string href;
    if (!readPod(feed_, &format, sizeof(format)) || !readString(feed_, href)) return false;
    if (i < MAX_OPDS_ACQUISITIONS) {
      out.acquisitions[i].format = static_cast<OpdsBookFormat>(format);
      out.acquisitions[i].href = std::move(href);
    }
  }
  return true;
}
