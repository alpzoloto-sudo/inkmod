// Legacy EPUB metadata/spine adapter. This is the first migration boundary:
// it reads the existing on-SD book.bin cache, but exposes no EPUB renderer,
// Section, Page, filesystem, or ZIP types to library/progress callers.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "DocumentModel.h"

class Epub;

namespace reader {

class EpubCatalogAdapter final : public DocumentCatalog {
 public:
  explicit EpubCatalogAdapter(const Epub& epub) : epub_(epub) {}

  BookMetadata metadata() const override;
  uint32_t chapterCount() const override;
  bool chapterInfo(uint32_t chapter, ChapterInfo& out) const override;

 private:
  static constexpr size_t kChapterTitleCapacity = 192;

  const Epub& epub_;
  // The existing BookMetadataCache API returns std::string values. This
  // caller-owned, fixed buffer avoids retaining one more heap allocation in
  // the new document layer. Its value is valid until the next chapterInfo().
  mutable std::array<char, kChapterTitleCapacity> chapterTitle_ = {};

  StringRef copyChapterTitle(const char* source, size_t sourceSize, bool& truncated) const;
};

}  // namespace reader
