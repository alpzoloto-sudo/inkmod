#include "XhtmlTextCursor.h"

#include <cstring>

namespace reader {

namespace {

bool asciiEqualIgnoreCase(const char left, const char right) {
  const char normalized = left >= 'A' && left <= 'Z' ? static_cast<char>(left + ('a' - 'A')) : left;
  return normalized == right;
}

bool beginsTag(const char* const tag, const size_t len, const char* const expected, const size_t expectedLen) {
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

enum class TagKind : uint8_t {
  Other,
  Img,
  Br,
  P,
  Div,
  Li,
  H1,
  H2,
  H3,
  H4,
  H5,
  H6,
  Blockquote,
  Pre,
  Em,
  I,
  Strong,
  B,
};

TagKind classifyTag(const char* const tag, const size_t len) {
  if (!tag || len == 0) return TagKind::Other;

  // XHTML tag names are ASCII. Dispatch by first letter and pass compile-time
  // literal lengths so this hot path does not call strlen() for every tag.
  char first = tag[0];
  if (first >= 'A' && first <= 'Z') first = static_cast<char>(first + ('a' - 'A'));

  switch (first) {
    case 'b':
      if (beginsTag(tag, len, "br", 2)) return TagKind::Br;
      if (beginsTag(tag, len, "blockquote", 10)) return TagKind::Blockquote;
      if (beginsTag(tag, len, "b", 1)) return TagKind::B;
      break;
    case 'd':
      if (beginsTag(tag, len, "div", 3)) return TagKind::Div;
      break;
    case 'e':
      if (beginsTag(tag, len, "em", 2)) return TagKind::Em;
      break;
    case 'h':
      if (beginsTag(tag, len, "h1", 2)) return TagKind::H1;
      if (beginsTag(tag, len, "h2", 2)) return TagKind::H2;
      if (beginsTag(tag, len, "h3", 2)) return TagKind::H3;
      if (beginsTag(tag, len, "h4", 2)) return TagKind::H4;
      if (beginsTag(tag, len, "h5", 2)) return TagKind::H5;
      if (beginsTag(tag, len, "h6", 2)) return TagKind::H6;
      break;
    case 'i':
      if (beginsTag(tag, len, "img", 3)) return TagKind::Img;
      if (beginsTag(tag, len, "i", 1)) return TagKind::I;
      break;
    case 'l':
      if (beginsTag(tag, len, "li", 2)) return TagKind::Li;
      break;
    case 'p':
      if (beginsTag(tag, len, "pre", 3)) return TagKind::Pre;
      if (beginsTag(tag, len, "p", 1)) return TagKind::P;
      break;
    case 's':
      if (beginsTag(tag, len, "strong", 6)) return TagKind::Strong;
      break;
    default:
      break;
  }
  return TagKind::Other;
}

bool isHeading(const TagKind kind) {
  return kind >= TagKind::H1 && kind <= TagKind::H6;
}

bool isBlockTag(const TagKind kind) {
  return kind == TagKind::P || kind == TagKind::Div || kind == TagKind::Li || kind == TagKind::Blockquote ||
         kind == TagKind::Pre || isHeading(kind);
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
  return beginsTag(tag, len, expected, std::strlen(expected));
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
  const bool closing = start < tagSize_ && tag_[start] == '/';
  if (closing) ++start;
  while (start < tagSize_ && isSpace(tag_[start])) ++start;
  const char* const name = tag_.data() + start;
  const size_t nameLen = tagSize_ - start;
  const TagKind kind = classifyTag(name, nameLen);

  if (!closing && kind == TagKind::Img) {
    const uint32_t resource = imageResourceId(name, nameLen);
    if (resource != 0) {
      pendingImage_ = {.value = resource};
      imagePending_ = true;
    }
    return;
  }
  if (!closing && kind == TagKind::Br) {
    pageBreakPending_ = true;
    return;
  }

  if (!closing) {
    if (kind == TagKind::P || kind == TagKind::Div || kind == TagKind::Li) {
      blockBoundaryPending_ = true;
      activeType_ = NodeType::Paragraph;
      activeStyle_ = {};
    } else if (isHeading(kind)) {
      blockBoundaryPending_ = true;
      activeType_ = NodeType::Heading;
      activeStyle_ = {.bold = true, .fontScalePermille = 1250};
    } else if (kind == TagKind::Blockquote) {
      blockBoundaryPending_ = true;
      activeType_ = NodeType::Quote;
      activeStyle_ = {.italic = true};
    } else if (kind == TagKind::Pre) {
      blockBoundaryPending_ = true;
      activeType_ = NodeType::CodeBlock;
      activeStyle_ = {};
    } else if (kind == TagKind::Em || kind == TagKind::I) {
      activeStyle_.italic = true;
    } else if (kind == TagKind::Strong || kind == TagKind::B) {
      activeStyle_.bold = true;
    }
  } else {
    if (kind == TagKind::Em || kind == TagKind::I) {
      activeStyle_.italic = false;
    } else if (kind == TagKind::Strong || kind == TagKind::B) {
      activeStyle_.bold = false;
    }
    if (isBlockTag(kind)) blockBoundaryPending_ = true;
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
