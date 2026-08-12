#include "ReaderSession.h"

namespace reader {

bool ReaderSession::openChapter(const uint32_t chapter, DocumentCursor& cursor) {
  if (!document_.openChapter(chapter, cursor)) return false;

  chapter_ = chapter;
  chapterOpen_ = true;
  historyHead_ = 0;
  historyCount_ = 0;
  layout_.beginChapter(chapter);
  return true;
}

bool ReaderSession::restoreAnchor(const PageAnchor& anchor, DocumentCursor& cursor) {
  if (!document_.openChapter(anchor.chapter, cursor) || !cursor.seek(anchor.byteOffset)) return false;

  chapter_ = anchor.chapter;
  chapterOpen_ = true;
  historyHead_ = 0;
  historyCount_ = 0;
  layout_.beginChapter(anchor.chapter, anchor.nodeOffset);
  return true;
}

LayoutResult ReaderSession::nextPage(DocumentCursor& cursor, const LayoutConstraints& constraints,
                                     const TextMeasurer& measurer, RenderPage& page) {
  if (!chapterOpen_) return LayoutResult::EndOfDocument;

  PageAnchor next = {};
  const LayoutResult result = layout_.layoutNext(cursor, constraints, measurer, page, next);
  if (result == LayoutResult::PageReady || result == LayoutResult::OutputFull) pushHistory(next);
  return result;
}

bool ReaderSession::historyAnchor(const uint8_t newestOffset, PageAnchor& out) const {
  if (newestOffset >= historyCount_) return false;

  const uint8_t newest = historyHead_ == 0 ? kHistoryCapacity - 1 : historyHead_ - 1;
  const uint8_t index = static_cast<uint8_t>((newest + kHistoryCapacity - newestOffset) % kHistoryCapacity);
  out = history_[index];
  return true;
}

void ReaderSession::pushHistory(const PageAnchor& anchor) {
  history_[historyHead_] = anchor;
  historyHead_ = static_cast<uint8_t>((historyHead_ + 1) % kHistoryCapacity);
  if (historyCount_ < kHistoryCapacity) ++historyCount_;
}

}  // namespace reader
