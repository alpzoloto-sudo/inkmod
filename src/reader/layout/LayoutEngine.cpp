#include "LayoutEngine.h"

#include <algorithm>
#include <limits>

namespace reader {

namespace {

bool isAsciiSpace(const char value) { return value == ' ' || value == '\n' || value == '\r' || value == '\t'; }

bool isTextNode(const NodeType type) {
  return type == NodeType::Paragraph || type == NodeType::Heading || type == NodeType::Quote ||
         type == NodeType::CodeBlock || type == NodeType::Link || type == NodeType::Footnote;
}

uint32_t skipLeadingSpace(const StringRef text, uint32_t offset) {
  while (offset < text.size && isAsciiSpace(text.data[offset])) ++offset;
  return offset;
}

uint32_t lineEnd(const StringRef text, const uint32_t offset, const uint16_t availableWidth, const TextMeasurer& measurer,
                 const TextStyle& style) {
  const uint32_t remaining = text.size - offset;
  if (measurer.measure({text.data + offset, remaining}, style) <= availableWidth) return text.size;

  uint32_t lastBreak = 0;
  for (uint32_t index = 1; index <= remaining; ++index) {
    const StringRef candidate{text.data + offset, index};
    if (measurer.measure(candidate, style) > availableWidth) break;
    if (isAsciiSpace(text.data[offset + index - 1])) lastBreak = index - 1;
  }

  // Do not split a UTF-8 code point or invent a hyphen here. If one word is
  // wider than the viewport, emit the complete word; a later hyphenation stage
  // will replace this conservative behaviour.
  if (lastBreak != 0) return offset + lastBreak;
  uint32_t wordEnd = 0;
  while (wordEnd < remaining && !isAsciiSpace(text.data[offset + wordEnd])) ++wordEnd;
  return offset + (wordEnd == 0 ? 1 : wordEnd);
}

int16_t toCoord(const uint16_t value) {
  return static_cast<int16_t>(std::min<uint16_t>(value, static_cast<uint16_t>(INT16_MAX)));
}

}  // namespace

void LayoutEngine::beginChapter(const uint32_t chapter, const uint32_t resumeNodeOffset) {
  hasPendingNode_ = false;
  pendingNode_ = {};
  pendingByteOffset_ = 0;
  pendingTextOffset_ = 0;
  resumeNodeOffset_ = resumeNodeOffset;
  chapter_ = chapter;
}

LayoutResult LayoutEngine::layoutNext(DocumentCursor& cursor, const LayoutConstraints& constraints,
                                      const TextMeasurer& measurer, RenderPage& out, PageAnchor& nextAnchor) {
  out.clear();
  nextAnchor = {.chapter = chapter_};
  if (constraints.width <= constraints.marginLeft + constraints.marginRight ||
      constraints.height <= constraints.marginTop + constraints.marginBottom) {
    return LayoutResult::InvalidConstraints;
  }

  const uint16_t contentWidth = constraints.width - constraints.marginLeft - constraints.marginRight;
  const uint16_t contentBottom = constraints.height - constraints.marginBottom;
  uint16_t y = constraints.marginTop;

  while (true) {
    if (!hasPendingNode_) {
      if (!cursor.next(pendingNode_)) {
        nextAnchor.byteOffset = cursor.byteOffset();
        return out.count() == 0 ? LayoutResult::EndOfDocument : LayoutResult::PageReady;
      }
      hasPendingNode_ = true;
      pendingByteOffset_ = cursor.byteOffset();
      pendingTextOffset_ = resumeNodeOffset_;
      resumeNodeOffset_ = 0;
    }

    if (pendingNode_.type == NodeType::PageBreak) {
      hasPendingNode_ = false;
      nextAnchor.byteOffset = cursor.byteOffset();
      return LayoutResult::PageReady;
    }

    if (pendingNode_.type == NodeType::Image) {
      const uint16_t imageHeight = std::min<uint16_t>(constraints.fallbackImageHeight, contentBottom - y);
      if (imageHeight == 0 || (y != constraints.marginTop && y + imageHeight > contentBottom)) {
        nextAnchor.byteOffset = pendingByteOffset_;
        return LayoutResult::PageReady;
      }
      const RenderCommand command{.type = RenderCommandType::DrawImage,
                                  .rect = {toCoord(constraints.marginLeft), toCoord(y), toCoord(contentWidth),
                                           toCoord(imageHeight)},
                                  .resource = pendingNode_.resource};
      if (!out.append(command)) {
        nextAnchor.byteOffset = pendingByteOffset_;
        return LayoutResult::OutputFull;
      }
      y = static_cast<uint16_t>(y + imageHeight);
      hasPendingNode_ = false;
      continue;
    }

    if (!isTextNode(pendingNode_.type) || !pendingNode_.text.data) {
      hasPendingNode_ = false;
      continue;
    }

    pendingTextOffset_ = skipLeadingSpace(pendingNode_.text, pendingTextOffset_);
    if (pendingTextOffset_ >= pendingNode_.text.size) {
      y = static_cast<uint16_t>(std::min<uint32_t>(contentBottom, y + constraints.paragraphGap));
      hasPendingNode_ = false;
      continue;
    }

    const uint16_t lineHeight = std::max<uint16_t>(1, measurer.lineHeight(pendingNode_.style));
    if (y + lineHeight > contentBottom) {
      nextAnchor.byteOffset = pendingByteOffset_;
      nextAnchor.nodeOffset = pendingTextOffset_;
      return LayoutResult::PageReady;
    }

    const uint32_t end = lineEnd(pendingNode_.text, pendingTextOffset_, contentWidth, measurer, pendingNode_.style);
    const StringRef line{pendingNode_.text.data + pendingTextOffset_, end - pendingTextOffset_};
    const RenderCommand command{.type = RenderCommandType::DrawText,
                                .rect = {toCoord(constraints.marginLeft), toCoord(y), toCoord(contentWidth),
                                         toCoord(lineHeight)},
                                .style = pendingNode_.style,
                                .text = line};
    if (!out.append(command)) {
      nextAnchor.byteOffset = pendingByteOffset_;
      nextAnchor.nodeOffset = pendingTextOffset_;
      return LayoutResult::OutputFull;
    }
    pendingTextOffset_ = end;
    y = static_cast<uint16_t>(y + lineHeight);
  }
}

}  // namespace reader
