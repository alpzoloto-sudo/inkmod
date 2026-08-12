// Format-neutral, lazy document nodes. Text points into the active parser chunk.
#pragma once

#include <cstdint>

namespace reader {

enum class NodeType : uint8_t { Paragraph, Heading, Image, Link, Footnote, Quote, CodeBlock, PageBreak };
enum class TextAlign : uint8_t { Default, Left, Right, Center, Justify };

struct StringRef {
  const char* data = nullptr;
  uint32_t size = 0;
  constexpr bool empty() const { return size == 0; }
};

struct ResourceId {
  uint32_t value = 0;
  constexpr bool valid() const { return value != 0; }
};

struct TextStyle {
  bool bold = false;
  bool italic = false;
  bool underline = false;
  uint16_t fontScalePermille = 1000;
  TextAlign align = TextAlign::Default;
};

struct DocumentNode {
  NodeType type = NodeType::Paragraph;
  TextStyle style = {};
  StringRef text = {};
  ResourceId resource = {};
  StringRef target = {};
};

}  // namespace reader

