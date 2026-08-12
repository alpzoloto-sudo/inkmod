#include "DocumentProgress.h"

#include <algorithm>
#include <limits>

namespace reader {

bool DocumentProgressEstimator::estimate(const DocumentCatalog& catalog, const uint32_t currentChapter,
                                         const uint64_t currentChapterBytes, DocumentProgress& out) {
  out = {};
  const uint32_t chapters = catalog.chapterCount();
  if (chapters == 0 || currentChapter >= chapters) return false;

  uint64_t before = 0;
  uint64_t total = 0;
  uint64_t currentSize = 0;
  for (uint32_t chapter = 0; chapter < chapters; ++chapter) {
    ChapterInfo info = {};
    if (!catalog.chapterInfo(chapter, info)) return false;
    if (chapter < currentChapter) before += info.approximateTextBytes;
    if (chapter == currentChapter) currentSize = info.approximateTextBytes;
    total += info.approximateTextBytes;
  }
  if (total == 0) return false;

  const uint64_t completed = before + std::min(currentChapterBytes, currentSize);
  out.completedBytes = std::min(completed, total);
  out.totalBytes = total;
  out.permille = static_cast<uint16_t>(std::min<uint64_t>(1000, (out.completedBytes * 1000) / total));
  return true;
}

}  // namespace reader
