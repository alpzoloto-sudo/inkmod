// Legacy FB2 metadata/chapter adapter. It deliberately wraps the already
// loaded Fb2 object, so asking the library for book data never scans the FB2
// source again or converts chapter text.
#pragma once

#include <cstdint>

#include "DocumentModel.h"

class Fb2;

namespace reader {

class Fb2CatalogAdapter final : public DocumentCatalog {
 public:
  explicit Fb2CatalogAdapter(const Fb2& fb2) : fb2_(fb2) {}

  BookMetadata metadata() const override;
  uint32_t chapterCount() const override;
  bool chapterInfo(uint32_t chapter, ChapterInfo& out) const override;

 private:
  const Fb2& fb2_;
};

}  // namespace reader
