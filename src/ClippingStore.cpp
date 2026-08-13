#include "ClippingStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <uzlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint8_t CLIPPING_FILE_VERSION = 2;
constexpr char CLIPPING_DIR[] = "/.inkmod/clippings";
constexpr char KINDLE_EXPORT[] = "/My Clippings.txt";

bool writeFixed(FsFile& file, const void* data, const size_t size) {
  return file.write(reinterpret_cast<const uint8_t*>(data), size) == size;
}

bool readFixed(FsFile& file, void* data, const size_t size) {
  return file.read(reinterpret_cast<uint8_t*>(data), size) == static_cast<int>(size);
}
}  // namespace

std::string ClippingStore::makeStorePath(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  return std::string(CLIPPING_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
}

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  filePath_ = filePath;
  title_ = title;
  author_ = author;
  bookType_ = bookType;
  storePath_ = makeStorePath(filePath, bookType);
  clippings_.clear();
  clippings_.reserve(8);
  return load();
}

bool ClippingStore::load() {
  if (storePath_.empty() || !Storage.exists(storePath_.c_str())) return true;

  FsFile file;
  if (!Storage.openFileForRead("CLIP", storePath_, file)) return false;

  uint8_t version = 0;
  uint16_t count = 0;
  std::string storedTitle;
  std::string storedAuthor;
  std::string storedPath;
  const bool headerOk = serialization::tryReadPod(file, version) &&
                        serialization::tryReadPod(file, count) &&
                        serialization::tryReadString(file, storedTitle) &&
                        serialization::tryReadString(file, storedAuthor) &&
                        serialization::tryReadString(file, storedPath);
  if (!headerOk || version != CLIPPING_FILE_VERSION || count > MAX_CLIPPINGS) {
    LOG_ERR("CLIP", "Invalid clipping file: %s", storePath_.c_str());
    file.close();
    return false;
  }

  clippings_.clear();
  clippings_.reserve(std::min<size_t>(count, 16));
  for (uint16_t i = 0; i < count; ++i) {
    Clipping clipping;
    if (!readFixed(file, &clipping, sizeof(clipping))) {
      LOG_ERR("CLIP", "Truncated clipping file at record %u", i);
      file.close();
      clippings_.clear();
      return false;
    }
    clipping.chapterTitle[sizeof(clipping.chapterTitle) - 1] = '\0';
    clipping.text[sizeof(clipping.text) - 1] = '\0';
    clippings_.push_back(clipping);
  }
  file.close();
  return true;
}

bool ClippingStore::save() const {
  if (storePath_.empty()) return false;
  Storage.mkdir(CLIPPING_DIR, true);

  if (clippings_.empty()) {
    if (Storage.exists(storePath_.c_str())) Storage.remove(storePath_.c_str());
    return true;
  }

  const std::string tmpPath = storePath_ + ".tmp";
  FsFile file;
  if (!Storage.openFileForWrite("CLIP", tmpPath, file)) return false;

  const uint8_t version = CLIPPING_FILE_VERSION;
  const uint16_t count = static_cast<uint16_t>(clippings_.size());
  bool ok = serialization::tryWritePod(file, version) &&
            serialization::tryWritePod(file, count) &&
            serialization::tryWriteString(file, title_) &&
            serialization::tryWriteString(file, author_) &&
            serialization::tryWriteString(file, filePath_);
  for (const auto& clipping : clippings_) {
    if (!ok) break;
    ok = writeFixed(file, &clipping, sizeof(clipping));
  }
  ok = ok && file.sync();
  file.close();

  if (!ok) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (Storage.exists(storePath_.c_str())) Storage.remove(storePath_.c_str());
  if (!Storage.rename(tmpPath.c_str(), storePath_.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

bool ClippingStore::appendKindleExport(const Clipping& clipping) const {
  FsFile file = Storage.open(KINDLE_EXPORT, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("CLIP", "Unable to append %s", KINDLE_EXPORT);
    return false;
  }

  const char* chapter = clipping.chapterTitle[0] ? clipping.chapterTitle : "Unknown chapter";
  file.print(title_.c_str());
  if (!author_.empty()) {
    file.print(" (");
    file.print(author_.c_str());
    file.print(")");
  }
  file.print("\r\n");
  if (clipping.endPageNumber > clipping.pageNumber) {
    file.printf("- Your Highlight on %s | InkMOD pages %u-%u\r\n\r\n", chapter,
                static_cast<unsigned>(clipping.pageNumber + 1),
                static_cast<unsigned>(clipping.endPageNumber + 1));
  } else {
    file.printf("- Your Highlight on %s | InkMOD page %u\r\n\r\n", chapter,
                static_cast<unsigned>(clipping.pageNumber + 1));
  }
  file.print(clipping.text);
  file.print("\r\n==========\r\n");
  const bool ok = file.sync();
  file.close();
  return ok;
}

ClippingStore::AddResult ClippingStore::add(const Clipping& clipping) {
  // Creating exactly the same clipping again acts as a toggle. This gives the
  // reader an obvious way to remove a highlight directly from the page,
  // without first visiting the clippings list.
  const auto sameRange = [&clipping](const Clipping& existing) {
    return existing.spineIndex == clipping.spineIndex &&
           existing.pageNumber == clipping.pageNumber &&
           existing.endPageNumber == clipping.endPageNumber &&
           existing.startWordIndex == clipping.startWordIndex &&
           existing.endWordIndex == clipping.endWordIndex;
  };

  const auto existing = std::find_if(clippings_.begin(), clippings_.end(), sameRange);
  if (existing != clippings_.end()) {
    const size_t index = static_cast<size_t>(std::distance(clippings_.begin(), existing));
    const Clipping removed = *existing;
    clippings_.erase(existing);
    if (!save()) {
      clippings_.insert(clippings_.begin() + static_cast<std::ptrdiff_t>(index), removed);
      return AddResult::SaveFailed;
    }
    return AddResult::RemovedExisting;
  }

  if (clippings_.size() >= MAX_CLIPPINGS) return AddResult::LimitReached;
  clippings_.push_back(clipping);
  if (!save()) {
    clippings_.pop_back();
    return AddResult::SaveFailed;
  }
  appendKindleExport(clipping);  // Export remains Kindle-style append-only.
  return AddResult::Added;
}

bool ClippingStore::removeAt(const size_t index) {
  if (index >= clippings_.size()) return false;
  clippings_.erase(clippings_.begin() + static_cast<std::ptrdiff_t>(index));
  return save();
}

void ClippingStore::clearAll() {
  clippings_.clear();
  save();
}
