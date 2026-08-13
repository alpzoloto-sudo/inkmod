#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Clipping {
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t endPageNumber = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint32_t timestamp = 0;
  char chapterTitle[48] = {};
  char text[513] = {};
};

class ClippingStore {
 public:
  static constexpr size_t MAX_CLIPPINGS = 256;
  enum class AddResult : uint8_t { Added, RemovedExisting, LimitReached, SaveFailed };

  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  AddResult add(const Clipping& clipping);
  bool removeAt(size_t index);
  void clearAll();

  const std::vector<Clipping>& getClippings() const { return clippings_; }
  bool empty() const { return clippings_.empty(); }

 private:
  std::vector<Clipping> clippings_;
  std::string filePath_;
  std::string title_;
  std::string author_;
  std::string bookType_;
  std::string storePath_;

  bool save() const;
  bool load();
  bool appendKindleExport(const Clipping& clipping) const;
  static std::string makeStorePath(const std::string& filePath, const std::string& bookType);
};
