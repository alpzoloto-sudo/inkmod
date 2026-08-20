#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "src/reader/core/io/IByteReader.h"
#include "src/reader/document/XhtmlTextCursor.h"

namespace {

class CountingStringReader final : public IByteReader {
 public:
  explicit CountingStringReader(const std::string& data) : data_(data) {}

  size_t read(void* dst, size_t len) override {
    ++readCalls;
    const size_t available = position_ < data_.size() ? data_.size() - position_ : 0;
    const size_t count = std::min(len, available);
    if (count != 0) std::memcpy(dst, data_.data() + position_, count);
    position_ += count;
    return count;
  }

  bool seek(uint64_t pos) override {
    if (pos > data_.size()) return false;
    position_ = static_cast<size_t>(pos);
    return true;
  }

  uint64_t tell() const override { return position_; }
  uint64_t size() const override { return data_.size(); }

  uint32_t readCalls = 0;

 private:
  const std::string& data_;
  size_t position_ = 0;
};

TEST(XhtmlPerformance, ParsesCachedChapterWithBoundedReadAhead) {
  std::string html = "<p>";
  html.append(2000, 'a');
  html += "</p>";

  CountingStringReader source(html);
  reader::XhtmlTextCursor cursor(source);
  reader::DocumentNode node;
  size_t textBytes = 0;
  while (cursor.next(node)) {
    if (node.text.data) textBytes += node.text.size;
  }

  EXPECT_EQ(textBytes, 2000U);
  // 2007 bytes fit in one 2 KiB source fill; allow the final EOF probe.
  EXPECT_LE(source.readCalls, 2U);
}

TEST(XhtmlPerformance, SeekInvalidatesReadAheadWithoutChangingOffsets) {
  const std::string html = "<p>first</p><p>second</p>";
  CountingStringReader source(html);
  reader::XhtmlTextCursor cursor(source);
  reader::DocumentNode node;

  ASSERT_TRUE(cursor.next(node));
  ASSERT_TRUE(node.text.data);
  EXPECT_EQ(std::string(node.text.data, node.text.size), "first");

  ASSERT_TRUE(cursor.seek(0));
  ASSERT_TRUE(cursor.next(node));
  EXPECT_EQ(std::string(node.text.data, node.text.size), "first");
}

TEST(XhtmlPerformance, OptimizedTagDispatchPreservesBlockAndInlineSemantics) {
  // XhtmlTextCursor intentionally does not split a text node merely because
  // an inline style toggles in the middle of one paragraph. Put each styled
  // sample in its own block so the test validates the cursor's established
  // emission semantics rather than expecting a new behaviour from this perf
  // optimization.
  const std::string html =
      "<H2>Heading</H2><p>plain</p><p><STRONG>bold</STRONG></p><p><em>italic</em></p>"
      "<blockquote>quote</blockquote>";
  CountingStringReader source(html);
  reader::XhtmlTextCursor cursor(source);
  reader::DocumentNode node;

  struct SeenText {
    reader::NodeType type;
    bool bold;
    bool italic;
    std::string text;
  };
  std::vector<SeenText> seen;
  while (cursor.next(node)) {
    if (!node.text.data) continue;
    seen.push_back({node.type, node.style.bold, node.style.italic, std::string(node.text.data, node.text.size)});
  }

  ASSERT_GE(seen.size(), 5U);
  EXPECT_EQ(seen[0].type, reader::NodeType::Heading);
  EXPECT_TRUE(seen[0].bold);
  EXPECT_EQ(seen[0].text, "Heading");

  bool sawPlain = false;
  bool sawBold = false;
  bool sawItalic = false;
  bool sawQuote = false;
  for (const auto& item : seen) {
    if (item.text == "plain") sawPlain = item.type == reader::NodeType::Paragraph && !item.bold && !item.italic;
    if (item.text == "bold") sawBold = item.bold;
    if (item.text == "italic") sawItalic = item.italic;
    if (item.text == "quote") sawQuote = item.type == reader::NodeType::Quote && item.italic;
  }
  EXPECT_TRUE(sawPlain);
  EXPECT_TRUE(sawBold);
  EXPECT_TRUE(sawItalic);
  EXPECT_TRUE(sawQuote);
}

TEST(XhtmlPerformance, OptimizedTagDispatchStillRecognizesUppercaseImage) {
  const std::string html = "<P>before</P><IMG SRC=\"images/cover.jpg\"/><P>after</P>";
  CountingStringReader source(html);
  reader::XhtmlTextCursor cursor(source);
  reader::DocumentNode node;

  bool sawImage = false;
  bool sawBefore = false;
  bool sawAfter = false;
  while (cursor.next(node)) {
    if (node.type == reader::NodeType::Image) sawImage = node.resource.value != 0;
    if (node.text.data) {
      const std::string text(node.text.data, node.text.size);
      if (text == "before") sawBefore = true;
      if (text == "after") sawAfter = true;
    }
  }

  EXPECT_TRUE(sawBefore);
  EXPECT_TRUE(sawImage);
  EXPECT_TRUE(sawAfter);
}

}  // namespace
