#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include "src/reader/layout/LayoutEngine.h"

namespace {

class SingleNodeCursor final : public reader::DocumentCursor {
 public:
  explicit SingleNodeCursor(const reader::DocumentNode& node) : node_(node) {}

  bool next(reader::DocumentNode& out) override {
    if (done_) return false;
    out = node_;
    done_ = true;
    return true;
  }
  bool seek(uint64_t byteOffset) override {
    if (byteOffset != 0) return false;
    done_ = false;
    return true;
  }
  uint64_t byteOffset() const override { return done_ ? 100 : 0; }

 private:
  reader::DocumentNode node_;
  bool done_ = false;
};

class CountingMeasurer final : public reader::TextMeasurer {
 public:
  uint16_t measure(reader::StringRef text, const reader::TextStyle&) const override {
    ++calls;
    return static_cast<uint16_t>(text.size * 8U);
  }
  uint16_t lineHeight(const reader::TextStyle&) const override { return 10; }

  mutable uint32_t calls = 0;
};

TEST(LayoutPerformance, KeepsExistingWhitespaceBreak) {
  constexpr char text[] = "one two three four";
  const reader::DocumentNode node{.type = reader::NodeType::Paragraph, .text = {text, sizeof(text) - 1}};
  SingleNodeCursor cursor(node);
  CountingMeasurer measurer;
  reader::LayoutEngine layout;
  reader::RenderPage page;
  reader::PageAnchor next;
  layout.beginChapter(0);

  ASSERT_EQ(layout.layoutNext(cursor, {.width = 64, .height = 10}, measurer, page, next),
            reader::LayoutResult::PageReady);
  ASSERT_EQ(page.count(), 1U);
  EXPECT_EQ(page[0].text.size, 7U);  // exactly the previous "one two" break
}

TEST(LayoutPerformance, UsesLogarithmicPrefixMeasurements) {
  std::string text;
  text.reserve(600);
  for (int i = 0; i < 100; ++i) text += "word ";

  const reader::DocumentNode node{.type = reader::NodeType::Paragraph,
                                  .text = {text.data(), static_cast<uint32_t>(text.size())}};
  SingleNodeCursor cursor(node);
  CountingMeasurer measurer;
  reader::LayoutEngine layout;
  reader::RenderPage page;
  reader::PageAnchor next;
  layout.beginChapter(0);

  ASSERT_EQ(layout.layoutNext(cursor, {.width = 96, .height = 10}, measurer, page, next),
            reader::LayoutResult::PageReady);
  ASSERT_EQ(page.count(), 1U);

  // One full-width probe plus ceil(log2(500)) binary-search probes. Give a
  // little headroom so the test describes the complexity rather than one
  // exact implementation detail. The previous linear scan needed ~13 calls
  // for this narrow width and hundreds for wider long lines.
  EXPECT_LE(measurer.calls, 12U);
}

}  // namespace
