// Format-neutral, allocation-free pagination core.
#pragma once

#include <cstdint>

#include "reader/document/DocumentModel.h"
#include "reader/layout/PageAnchor.h"
#include "reader/render/RenderPage.h"

namespace reader {

struct LayoutConstraints {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t marginTop = 0;
  uint16_t marginRight = 0;
  uint16_t marginBottom = 0;
  uint16_t marginLeft = 0;
  uint16_t paragraphGap = 0;
  uint16_t fallbackImageHeight = 80;
};

class TextMeasurer {
 public:
  virtual ~TextMeasurer() = default;
  virtual uint16_t measure(StringRef text, const TextStyle& style) const = 0;
  virtual uint16_t lineHeight(const TextStyle& style) const = 0;
};

enum class LayoutResult : uint8_t { PageReady, EndOfDocument, InvalidConstraints, OutputFull };

class LayoutEngine {
 public:
  // Call after a cursor has been opened for a new chapter or layout spec.
  // `resumeNodeOffset` is applied to the first node returned after a cursor
  // seek. It is a byte offset inside that node's UTF-8 text, never a display
  // pixel position, so it survives font and orientation changes.
  void beginChapter(uint32_t chapter, uint32_t resumeNodeOffset = 0);

  // Produces one page from a lazy cursor. RenderPage is caller-owned and must
  // be a long-lived member, not a task-stack allocation. nextAnchor refers to
  // the first unrendered logical position and is suitable for page history.
  LayoutResult layoutNext(DocumentCursor& cursor, const LayoutConstraints& constraints,
                          const TextMeasurer& measurer, RenderPage& out, PageAnchor& nextAnchor);

 private:
  bool hasPendingNode_ = false;
  DocumentNode pendingNode_ = {};
  uint64_t pendingByteOffset_ = 0;
  uint32_t pendingTextOffset_ = 0;
  uint32_t resumeNodeOffset_ = 0;
  uint32_t chapter_ = 0;
};

}  // namespace reader
