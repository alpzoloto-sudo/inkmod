#pragma once

#include <OpdsParser.h>
#include <HalStorage.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

class OpdsCacheStore {
 public:
  struct Meta {
    uint32_t version = 1;
    uint32_t urlHash = 0;
    size_t entryCount = 0;
    std::string url;
    std::string searchTemplate;
    std::string nextPageUrl;
    std::string prevPageUrl;
  };

  OpdsCacheStore() = default;
  ~OpdsCacheStore();

  bool beginWrite(const std::string& basePath, const std::string& url, uint32_t urlHash);
  bool append(OpdsEntry&& entry);
  bool finishWrite(const std::string& searchTemplate, const std::string& nextPageUrl,
                   const std::string& prevPageUrl);
  void abortWrite();

  bool openRead(const std::string& basePath);
  void closeRead();
  bool readEntry(size_t index, OpdsEntry& out);
  const Meta& meta() const { return meta_; }

  static bool parserSink(void* context, OpdsEntry&& entry) {
    return static_cast<OpdsCacheStore*>(context)->append(std::move(entry));
  }

 private:
  std::string basePath_;
  std::string feedPath_;
  std::string indexPath_;
  std::string metaPath_;
  std::string feedTmpPath_;
  std::string indexTmpPath_;
  std::string metaTmpPath_;
  Meta meta_;
  FsFile feed_;
  FsFile index_;
  bool writing_ = false;
  bool reading_ = false;

  bool writeString(FsFile& file, const std::string& value);
  bool readString(FsFile& file, std::string& value);
  bool writeMeta();
  bool readMeta();
};
