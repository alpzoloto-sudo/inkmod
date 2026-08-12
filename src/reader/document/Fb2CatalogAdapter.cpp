#include "Fb2CatalogAdapter.h"

#include <algorithm>
#include <limits>

#include "Fb2.h"

namespace reader {

namespace {

StringRef toStringRef(const std::string& value) {
  return {value.data(),
          static_cast<uint32_t>(std::min<size_t>(value.size(), std::numeric_limits<uint32_t>::max()))};
}

}  // namespace

BookMetadata Fb2CatalogAdapter::metadata() const {
  // Fb2 owns these fields for as long as its loaded package remains open.
  return {.title = toStringRef(fb2_.getTitle()),
          .author = toStringRef(fb2_.getAuthor()),
          .language = toStringRef(fb2_.getLanguage())};
}

uint32_t Fb2CatalogAdapter::chapterCount() const {
  const int count = fb2_.getChapterCount();
  return count > 0 ? static_cast<uint32_t>(count) : 0;
}

bool Fb2CatalogAdapter::chapterInfo(const uint32_t chapter, ChapterInfo& out) const {
  out = {};
  if (chapter >= chapterCount()) return false;

  out.id = chapter;
  // This reads the compact persisted FB2 section index. It does not open the
  // original book, render XHTML, or decode an image, so catalog reads stay
  // safe while the reader holds its own source-file handle.
  out.approximateTextBytes = Fb2::getApproxChapterSize(fb2_.getCachePath(), static_cast<int>(chapter));
  return true;
}

}  // namespace reader
