// Progress calculation over the shared document catalog. It intentionally
// measures source/index bytes rather than rendered pages, so it is stable
// across font, orientation and layout changes.
#pragma once

#include <cstdint>

#include "reader/document/DocumentModel.h"

namespace reader {

struct DocumentProgress {
  uint16_t permille = 0;
  uint64_t completedBytes = 0;
  uint64_t totalBytes = 0;
};

class DocumentProgressEstimator {
 public:
  // currentChapterBytes is a source offset within the chapter, not a screen
  // position. Returns false for an invalid/missing catalog chapter.
  static bool estimate(const DocumentCatalog& catalog, uint32_t currentChapter, uint64_t currentChapterBytes,
                       DocumentProgress& out);
};

}  // namespace reader
