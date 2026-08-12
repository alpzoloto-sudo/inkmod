#include "EpubCatalogAdapter.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "Epub.h"

namespace reader {

namespace {

StringRef toStringRef(const std::string& value) {
  return {value.data(),
          static_cast<uint32_t>(std::min<size_t>(value.size(), std::numeric_limits<uint32_t>::max()))};
}

}  // namespace

BookMetadata EpubCatalogAdapter::metadata() const {
  // Epub owns these three strings for its whole lifetime, so no metadata copy
  // or allocation is needed here.
  return {.title = toStringRef(epub_.getTitle()),
          .author = toStringRef(epub_.getAuthor()),
          .language = toStringRef(epub_.getLanguage())};
}

uint32_t EpubCatalogAdapter::chapterCount() const {
  const int count = epub_.getSpineItemsCount();
  return count > 0 ? static_cast<uint32_t>(count) : 0;
}

bool EpubCatalogAdapter::chapterInfo(const uint32_t chapter, ChapterInfo& out) const {
  out = {};
  if (chapter >= chapterCount()) return false;

  const size_t cumulative = epub_.getCumulativeSpineItemSize(static_cast<int>(chapter));
  const size_t previous = chapter == 0 ? 0 : epub_.getCumulativeSpineItemSize(static_cast<int>(chapter - 1));
  out.id = chapter;
  out.approximateTextBytes = cumulative >= previous ? cumulative - previous : 0;

  const int tocIndex = epub_.getTocIndexForSpineIndex(static_cast<int>(chapter));
  if (tocIndex < 0) return true;

  // getTocItem() belongs to the old cache API and returns a temporary string.
  // Copy its title into the fixed adapter buffer before that temporary dies.
  const auto toc = epub_.getTocItem(tocIndex);
  out.title = copyChapterTitle(toc.title.data(), toc.title.size(), out.titleTruncated);
  return true;
}

StringRef EpubCatalogAdapter::copyChapterTitle(const char* source, const size_t sourceSize, bool& truncated) const {
  truncated = sourceSize >= chapterTitle_.size();
  const size_t count = std::min(sourceSize, chapterTitle_.size() - 1);
  if (count > 0) std::memcpy(chapterTitle_.data(), source, count);
  chapterTitle_[count] = '\0';
  return {chapterTitle_.data(), static_cast<uint32_t>(count)};
}

}  // namespace reader
