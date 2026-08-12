#include "XhtmlTextCursor.h"

#include <cstring>

namespace reader {

namespace {

bool asciiEqualIgnoreCase(const char left, const char right) {
  const char normalized = left >= 'A' && left <= 'Z' ? static_cast<char>(left + ('a' - 'A')) : left;
  return normalized == right;
}

bool beginsTag(const char* const tag, const size_t len, const char* const expected) {
  const size_t expectedLen = std::strlen(expected);
  if (len < expectedLen) return false;
  for (size_t i = 0; i < expectedLen; ++i) {
    if (!asciiEqualIgnoreCase(tag[i], expected[i])) return false;
  }
  return len == expectedLen || tag[expectedLen] == ' ' || tag[expectedLen] == '\t' || tag[expectedLen] == '/';
}

uint32_t fnv1a(const char* const value, const size_t len) {
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint8_t>(value[i]);
    hash *= 16777619U;
  }
  return hash == 0 ? 1U : hash;
}

}  // namespace

bool XhtmlTextCursor::readChar(char& out) {
  if (inputPos_ == inputSize_) {
    inputSize_ = source_.read(input_.data(), input_.size());
    inputPos_ = 0;
    if (inputSize_ == 0) return false;
  }
  out = static_cast<char>(input_[inputPos_++]);
  ++logicalOffset_;
  return true;
}

bool XhtmlTextCursor::seek(const uint64_t byteOffset) {
  if (!source_.seek(byteOffset)) return false;
  inputPos_ = 0;
  inputSize_ = 0;
  textSize_ = 0;
  tagSize_ = 0;
  logicalOffset_ = byteOffset;
  activeType_ = NodeType::Paragraph;
  activeStyle_ = {};
  imagePending_ = false;
  pageBreakPending_ = false;
  blockBoundaryPending_ = false;
  lastWasSpace_ = true;
  return true;
}

bool XhtmlTextCursor::readTag() {
  tagSize_ = 0;
  char value = 0;
  while (readChar(value)) {
    if (value == '>') {
      handleTag();
      return true;
    }
    if (tagSize_ + 1 < tag_.size()) tag_[tagSize_++] = value;
  }
  return false;
}

bool XhtmlTextCursor::isSpace(const char value) {
  return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

bool XhtmlTextCursor::tagNameEquals(const char* const tag, const size_t len, const char* const expected) {
  return beginsTag(tag, len, expected);
}

uint32_t XhtmlTextCursor::imageResourceId(const char* const tag, const size_t len) {
  for (size_t i = 0; i + 3 < len; ++i) {
    if (!asciiEqualIgnoreCase(tag[i], 's') || !asciiEqualIgnoreCase(tag[i + 1], 'r') ||
        !asciiEqualIgnoreCase(tag[i + 2], 'c')) {
      continue;
    }
    size_t pos = i + 3;
    while (pos < len && isSpace(tag[pos])) ++pos;
    if (pos >= len || tag[pos++] != '=') continue;
    while (pos < len && isSpace(tag[pos])) ++pos;
    if (pos >= len) return {};
    const char quote = tag[pos];
    const bool quoted = quote == '\'' || quote == '"';
    if (quoted) ++pos;
    const size_t start = pos;
    while (pos < len && (quoted ? tag[pos] != quote : !isSpace(tag[pos]) && tag[pos] != '/')) ++pos;
    return pos > start ? fnv1a(tag + start, pos - start) : 0;
  }
  return 0;
}

void XhtmlTextCursor::handleTag() {
  size_t start = 0;
  while (start < tagSize_ && isSpace(tag_[start])) ++start;
  bool closing = start < tagSize_ && tag_[start] == '/';
  if (closing) ++start;
  while (start < tagSize_ && isSpace(tag_[start])) ++start;
  const char* const name = tag_.data() + start;
  const size_t nameLen = tagSize_ - start;

  if (!closing && tagNameEquals(name, nameLen, "img")) {
    const uint32_t resource = imageResourceId(name, nameLen);
    if (resource != 0) {
      pendingImage_ = {.value = resource};
      imagePending_ = true;
    }
    return;
  }
  if (!closing && tagNameEquals(name, nameLen, "br")) {
    pageBreakPending_ = true;
    return;
  }
  if (!closing && (tagNameEquals(name, nameLen, "p") || tagNameEquals(name, nameLen, "div") ||
                   tagNameEquals(name, nameLen, "li"))) {
    blockBoundaryPending_ = true;
    activeType_ = NodeType::Paragraph;
    activeStyle_ = {};
  } else if (!closing && (tagNameEquals(name, nameLen, "h1") || tagNameEquals(name, nameLen, "h2") ||
                          tagNameEquals(name, nameLen, "h3") || tagNameEquals(name, nameLen, "h4") ||
                          tagNameEquals(name, nameLen, "h5") || tagNameEquals(name, nameLen, "h6"))) {
    blockBoundaryPending_ = true;
    activeType_ = NodeType::Heading;
    activeStyle_ = {.bold = true, .fontScalePermille = 1250};
  } else if (!closing && tagNameEquals(name, nameLen, "blockquote")) {
    blockBoundaryPending_ = true;
    activeType_ = NodeType::Quote;
    activeStyle_ = {.italic = true};
  } else if (!closing && tagNameEquals(name, nameLen, "pre")) {
    blockBoundaryPending_ = true;
    activeType_ = NodeType::CodeBlock;
    activeStyle_ = {};
  } else if (!closing && (tagNameEquals(name, nameLen, "em") || tagNameEquals(name, nameLen, "i"))) {
    activeStyle_.italic = true;
  } else if (!closing && (tagNameEquals(name, nameLen, "strong") || tagNameEquals(name, nameLen, "b"))) {
    activeStyle_.bold = true;
  } else if (closing && (tagNameEquals(name, nameLen, "em") || tagNameEquals(name, nameLen, "i"))) {
    activeStyle_.italic = false;
  } else if (closing && (tagNameEquals(name, nameLen, "strong") || tagNameEquals(name, nameLen, "b"))) {
    activeStyle_.bold = false;
  }
  if (closing && (tagNameEquals(name, nameLen, "p") || tagNameEquals(name, nameLen, "div") ||
                  tagNameEquals(name, nameLen, "li") || tagNameEquals(name, nameLen, "blockquote") ||
                  tagNameEquals(name, nameLen, "pre") || tagNameEquals(name, nameLen, "h1") ||
                  tagNameEquals(name, nameLen, "h2") || tagNameEquals(name, nameLen, "h3") ||
                  tagNameEquals(name, nameLen, "h4") || tagNameEquals(name, nameLen, "h5") ||
                  tagNameEquals(name, nameLen, "h6"))) {
    blockBoundaryPending_ = true;
  }
}

void XhtmlTextCursor::appendText(const char value) {
  if (isSpace(value)) {
    if (lastWasSpace_) return;
    lastWasSpace_ = true;
    if (textSize_ < text_.size()) text_[textSize_++] = ' ';
    return;
  }
  if (textSize_ == 0) {
    textType_ = activeType_;
    textStyle_ = activeStyle_;
  }
  lastWasSpace_ = false;
  if (textSize_ < text_.size()) text_[textSize_++] = value;
}

bool XhtmlTextCursor::emitText(DocumentNode& out) const {
  if (textSize_ == 0) return false;
  out = {.type = textType_, .style = textStyle_, .text = {text_.data(), static_cast<uint32_t>(textSize_)}};
  return true;
}

bool XhtmlTextCursor::next(DocumentNode& out) {
  if (imagePending_ && textSize_ == 0) {
    out = {.type = NodeType::Image, .resource = pendingImage_};
    imagePending_ = false;
    return true;
  }
  if (pageBreakPending_ && textSize_ == 0) {
    out = {.type = NodeType::PageBreak};
    pageBreakPending_ = false;
    return true;
  }

  textSize_ = 0;
  lastWasSpace_ = true;
  blockBoundaryPending_ = false;
  char value = 0;
  while (readChar(value)) {
    if (value == '<') {
      if (!readTag()) break;
      if (blockBoundaryPending_ && textSize_ != 0) return emitText(out);
      if (blockBoundaryPending_) {
        blockBoundaryPending_ = false;
        continue;
      }
      if ((imagePending_ || pageBreakPending_) && textSize_ != 0) return emitText(out);
      if (imagePending_) {
        out = {.type = NodeType::Image, .resource = pendingImage_};
        imagePending_ = false;
        return true;
      }
      if (pageBreakPending_) {
        out = {.type = NodeType::PageBreak};
        pageBreakPending_ = false;
        return true;
      }
      continue;
    }
    appendText(value);
    if (textSize_ == text_.size()) return emitText(out);
  }
  return emitText(out);
}

}  // namespace reader
