// Format-neutral reading session. It owns neither the document nor its cursor:
// adapters keep those lifetimes explicit and the session stores only a bounded
// page-history ring suitable for the ESP32-C3.
#pragma once

#include <array>
#include <cstdint>

#include "reader/document/DocumentModel.h"
#include "reader/layout/LayoutEngine.h"

namespace reader {

class ReaderSession {
 public:
  static constexpr uint8_t kHistoryCapacity = 8;

  explicit ReaderSession(DocumentModel& document) : document_(document) {}

  bool openChapter(uint32_t chapter, DocumentCursor& cursor);
  // Restores a saved logical position. The cursor owns its seek semantics;
  // this session never seeks compressed files or stores a text buffer.
  bool restoreAnchor(const PageAnchor& anchor, DocumentCursor& cursor);
  LayoutResult nextPage(DocumentCursor& cursor, const LayoutConstraints& constraints, const TextMeasurer& measurer,
                        RenderPage& page);

  uint8_t historyCount() const { return historyCount_; }
  bool historyAnchor(uint8_t newestOffset, PageAnchor& out) const;
  uint32_t currentChapter() const { return chapter_; }

 private:
  void pushHistory(const PageAnchor& anchor);

  DocumentModel& document_;
  LayoutEngine layout_;
  std::array<PageAnchor, kHistoryCapacity> history_ = {};
  uint8_t historyHead_ = 0;
  uint8_t historyCount_ = 0;
  uint32_t chapter_ = 0;
  bool chapterOpen_ = false;
};

}  // namespace reader
