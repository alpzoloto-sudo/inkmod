#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "src/reader/core/io/IByteReader.h"
#include "src/reader/core/io/MemoryByteReader.h"
#include "src/reader/core/ReaderJobQueue.h"
#include "src/reader/core/DocumentProgress.h"
#include "src/reader/core/BookIdentity.h"
#include "src/reader/core/io/ZipEntryByteReader.h"
#include "src/reader/document/XhtmlTextCursor.h"
#include "src/reader/layout/LayoutEngine.h"
#include "src/reader/layout/PageAnchor.h"
#include "src/reader/render/RenderPageRenderer.h"
#include "src/reader/session/ReaderSession.h"
#include "src/reader/session/ReaderState.h"

namespace {

// Test-only fixed source. It has no heap allocation, mirroring the intended
// Fs/cache adapter lifetime on ESP32-C3.
class FixedByteReader final : public IByteReader {
 public:
  explicit FixedByteReader(const std::array<uint8_t, 4>& bytes) : bytes_(bytes) {}

  size_t read(void* destination, size_t len) override {
    auto* out = static_cast<uint8_t*>(destination);
    const size_t remaining = bytes_.size() - position_;
    const size_t count = len < remaining ? len : remaining;
    for (size_t i = 0; i < count; ++i) out[i] = bytes_[position_ + i];
    position_ += count;
    return count;
  }

  bool seek(uint64_t pos) override {
    if (pos > bytes_.size()) return false;
    position_ = static_cast<size_t>(pos);
    return true;
  }

  uint64_t tell() const override { return position_; }
  uint64_t size() const override { return bytes_.size(); }

 private:
  const std::array<uint8_t, 4>& bytes_;
  size_t position_ = 0;
};

class FixedCursor final : public reader::DocumentCursor {
 public:
  explicit FixedCursor(const std::array<reader::DocumentNode, 2>& nodes) : nodes_(nodes) {}

  bool next(reader::DocumentNode& out) override {
    if (index_ >= nodes_.size()) return false;
    out = nodes_[index_++];
    return true;
  }
  bool seek(uint64_t byteOffset) override {
    if (byteOffset % 100 != 0) return false;
    const size_t index = static_cast<size_t>(byteOffset / 100);
    if (index > nodes_.size()) return false;
    index_ = index;
    return true;
  }
  uint64_t byteOffset() const override { return index_ * 100; }

 private:
  const std::array<reader::DocumentNode, 2>& nodes_;
  size_t index_ = 0;
};

class FixedMeasurer final : public reader::TextMeasurer {
 public:
  uint16_t measure(reader::StringRef text, const reader::TextStyle&) const override {
    return static_cast<uint16_t>(text.size * 8);
  }
  uint16_t lineHeight(const reader::TextStyle&) const override { return 10; }
};

class FixedDocument final : public reader::DocumentModel {
 public:
  explicit FixedDocument(const std::array<reader::DocumentNode, 2>&) {}

  reader::BookMetadata metadata() const override { return {}; }
  uint32_t chapterCount() const override { return 1; }
  bool chapterInfo(uint32_t chapter, reader::ChapterInfo& out) const override {
    if (chapter != 0) return false;
    out = {.id = 0};
    return true;
  }
  bool openChapter(uint32_t chapter, reader::DocumentCursor&) override { return chapter == 0; }
};

class ProgressCatalog final : public reader::DocumentCatalog {
 public:
  reader::BookMetadata metadata() const override { return {}; }
  uint32_t chapterCount() const override { return 2; }
  bool chapterInfo(uint32_t chapter, reader::ChapterInfo& out) const override {
    if (chapter >= chapterCount()) return false;
    out = {.id = chapter, .approximateTextBytes = chapter == 0 ? 100U : 300U};
    return true;
  }
};

class CountingRenderExecutor final : public reader::RenderExecutor {
 public:
  void drawText(const reader::RenderRect&, reader::StringRef, const reader::TextStyle&) override { ++textCount; }
  void drawImage(const reader::RenderRect&, reader::ResourceId) override { ++imageCount; }
  void drawLine(const reader::RenderRect&) override { ++lineCount; }
  void fillRect(const reader::RenderRect&) override { ++fillCount; }

  uint8_t textCount = 0;
  uint8_t imageCount = 0;
  uint8_t lineCount = 0;
  uint8_t fillCount = 0;
};

TEST(IByteReader, ReadsSeeksAndDetectsEof) {
  constexpr std::array<uint8_t, 4> source = {1, 2, 3, 4};
  FixedByteReader reader(source);
  std::array<uint8_t, 2> out = {};

  EXPECT_FALSE(reader.eof());
  EXPECT_EQ(reader.read(out.data(), out.size()), 2U);
  EXPECT_EQ(out[0], 1U);
  EXPECT_EQ(out[1], 2U);
  EXPECT_TRUE(reader.seek(4));
  EXPECT_TRUE(reader.eof());
  EXPECT_EQ(reader.read(out.data(), out.size()), 0U);
  EXPECT_FALSE(reader.seek(5));
}

TEST(IByteReader, BoundsMemoryAndStoredZipEntryWithoutAllocating) {
  constexpr std::array<uint8_t, 6> source = {9, 8, 1, 2, 3, 7};
  MemoryByteReader sourceReader(source.data(), source.size());
  ZipEntryByteReader entry(sourceReader, 2, 3);
  std::array<uint8_t, 4> out = {};

  EXPECT_EQ(entry.read(out.data(), out.size()), 3U);
  EXPECT_EQ(out[0], 1U);
  EXPECT_EQ(out[2], 3U);
  EXPECT_TRUE(entry.eof());
  EXPECT_TRUE(entry.seek(0));
  EXPECT_EQ(entry.read(out.data(), 1), 1U);
  EXPECT_EQ(out[0], 1U);
}

TEST(XhtmlTextCursor, StreamsTextHeadingsAndImagesWithoutDom) {
  constexpr char html[] = "<h1>Title</h1><p>Hello <em>world</em>.</p><img src=\"image/a.png\"><p>Next</p>";
  MemoryByteReader source(html, sizeof(html) - 1);
  reader::XhtmlTextCursor cursor(source);
  reader::DocumentNode node;

  ASSERT_TRUE(cursor.next(node));
  EXPECT_EQ(node.type, reader::NodeType::Heading);
  EXPECT_TRUE(node.style.bold);
  EXPECT_EQ(node.text.size, 5U);

  ASSERT_TRUE(cursor.next(node));
  EXPECT_EQ(node.type, reader::NodeType::Paragraph);
  EXPECT_EQ(node.text.size, 12U);

  ASSERT_TRUE(cursor.next(node));
  EXPECT_EQ(node.type, reader::NodeType::Image);
  EXPECT_TRUE(node.resource.valid());

  ASSERT_TRUE(cursor.next(node));
  EXPECT_EQ(node.type, reader::NodeType::Paragraph);
  EXPECT_EQ(node.text.size, 4U);
}

TEST(ReaderJobQueue, KeepsPreparationWorkOrderedAndBounded) {
  reader::ReaderJobQueue jobs;
  ASSERT_TRUE(jobs.enqueue({.kind = reader::ReaderJobKind::ExtractSource, .bookKey = 7}));
  ASSERT_TRUE(jobs.enqueue({.kind = reader::ReaderJobKind::BuildIndex, .bookKey = 7, .argument = 42}));

  reader::ReaderJob job;
  ASSERT_TRUE(jobs.dequeue(job));
  EXPECT_EQ(job.kind, reader::ReaderJobKind::ExtractSource);
  ASSERT_TRUE(jobs.dequeue(job));
  EXPECT_EQ(job.kind, reader::ReaderJobKind::BuildIndex);
  EXPECT_EQ(job.argument, 42U);
  EXPECT_TRUE(jobs.empty());

  for (uint8_t index = 0; index < reader::ReaderJobQueue::kCapacity; ++index) {
    EXPECT_TRUE(jobs.enqueue({.bookKey = index}));
  }
  EXPECT_FALSE(jobs.enqueue({}));
}

TEST(DocumentProgress, UsesFormatNeutralChapterSizes) {
  ProgressCatalog catalog;
  reader::DocumentProgress progress;
  ASSERT_TRUE(reader::DocumentProgressEstimator::estimate(catalog, 1, 150, progress));
  EXPECT_EQ(progress.completedBytes, 250U);
  EXPECT_EQ(progress.totalBytes, 400U);
  EXPECT_EQ(progress.permille, 625U);
}

TEST(BookIdentity, ChangesWhenSourceSignatureChangesAndFormatsCacheName) {
  constexpr char path[] = "/books/example.epub";
  const reader::BookId first = reader::BookIdentity::make({path, sizeof(path) - 1}, {.size = 100, .modifiedTime = 1});
  const reader::BookId second = reader::BookIdentity::make({path, sizeof(path) - 1}, {.size = 101, .modifiedTime = 1});
  EXPECT_TRUE(first.valid());
  EXPECT_NE(first.value, second.value);

  std::array<char, reader::BookIdentity::kCacheDirectoryNameCapacity> name = {};
  ASSERT_TRUE(reader::BookIdentity::cacheDirectoryName(first, name.data(), name.size()));
  EXPECT_EQ(name[0], 'b');
  EXPECT_EQ(name[4], '_');
  EXPECT_EQ(name.back(), '\0');
}

TEST(RenderPageRenderer, DispatchesBoundedCommandsWithoutFormatDependencies) {
  reader::RenderPage page;
  constexpr char text[] = "text";
  ASSERT_TRUE(page.append({.type = reader::RenderCommandType::DrawText, .text = {text, sizeof(text) - 1}}));
  ASSERT_TRUE(page.append({.type = reader::RenderCommandType::DrawImage, .resource = {.value = 7}}));
  ASSERT_TRUE(page.append({.type = reader::RenderCommandType::DrawLine}));
  ASSERT_TRUE(page.append({.type = reader::RenderCommandType::FillRect}));
  CountingRenderExecutor executor;

  reader::RenderPageRenderer::render(page, executor);
  EXPECT_EQ(executor.textCount, 1U);
  EXPECT_EQ(executor.imageCount, 1U);
  EXPECT_EQ(executor.lineCount, 1U);
  EXPECT_EQ(executor.fillCount, 1U);
}

TEST(PageAnchor, IsLogicalAndPersistable) {
  reader::PageAnchor anchor{.chapter = 2, .byteOffset = 65536, .nodeOffset = 12};
  EXPECT_EQ(anchor.chapter, 2U);
  EXPECT_EQ(anchor.byteOffset, 65536U);
  EXPECT_EQ(anchor.nodeOffset, 12U);
}

TEST(LayoutEngine, PaginatesLazyParagraphNodesIntoBoundedCommands) {
  constexpr char first[] = "one two three four";
  constexpr char second[] = "five six";
  const std::array<reader::DocumentNode, 2> nodes = {{
      {.type = reader::NodeType::Paragraph, .text = {first, sizeof(first) - 1}},
      {.type = reader::NodeType::Paragraph, .text = {second, sizeof(second) - 1}},
  }};
  FixedCursor cursor(nodes);
  FixedMeasurer measurer;
  reader::LayoutEngine layout;
  reader::RenderPage page;
  reader::PageAnchor next;
  layout.beginChapter(3);

  const reader::LayoutConstraints constraints{.width = 64, .height = 20};
  EXPECT_EQ(layout.layoutNext(cursor, constraints, measurer, page, next), reader::LayoutResult::PageReady);
  ASSERT_EQ(page.count(), 2U);
  EXPECT_EQ(page[0].text.size, 7U);  // "one two"
  EXPECT_EQ(page[1].text.size, 5U);  // "three"
  EXPECT_EQ(next.chapter, 3U);
  EXPECT_GT(next.nodeOffset, 0U);
}

TEST(ReaderSession, OpensAFormatNeutralChapterAndKeepsBoundedHistory) {
  constexpr char first[] = "one two three";
  constexpr char second[] = "four five";
  const std::array<reader::DocumentNode, 2> nodes = {{
      {.type = reader::NodeType::Paragraph, .text = {first, sizeof(first) - 1}},
      {.type = reader::NodeType::Paragraph, .text = {second, sizeof(second) - 1}},
  }};
  FixedDocument document(nodes);
  FixedCursor cursor(nodes);
  FixedMeasurer measurer;
  reader::ReaderSession session(document);
  reader::RenderPage page;

  ASSERT_TRUE(session.openChapter(0, cursor));
  EXPECT_EQ(session.currentChapter(), 0U);
  EXPECT_EQ(session.nextPage(cursor, {.width = 64, .height = 20}, measurer, page), reader::LayoutResult::PageReady);
  ASSERT_EQ(session.historyCount(), 1U);
  reader::PageAnchor anchor;
  ASSERT_TRUE(session.historyAnchor(0, anchor));
  EXPECT_EQ(anchor.chapter, 0U);
}

TEST(ReaderSession, RestoresLogicalAnchorWithoutWholeBookState) {
  constexpr char first[] = "one two three";
  constexpr char second[] = "four five";
  const std::array<reader::DocumentNode, 2> nodes = {{
      {.type = reader::NodeType::Paragraph, .text = {first, sizeof(first) - 1}},
      {.type = reader::NodeType::Paragraph, .text = {second, sizeof(second) - 1}},
  }};
  FixedDocument document(nodes);
  FixedCursor cursor(nodes);
  FixedMeasurer measurer;
  reader::ReaderSession session(document);
  reader::RenderPage page;

  ASSERT_TRUE(session.restoreAnchor({.chapter = 0, .byteOffset = 100, .nodeOffset = 0}, cursor));
  EXPECT_EQ(session.nextPage(cursor, {.width = 80, .height = 20}, measurer, page), reader::LayoutResult::PageReady);
  ASSERT_GT(page.count(), 0U);
  EXPECT_EQ(page[0].text.data[0], 'f');
}

TEST(ReaderState, RoundTripsCounterModeAndRejectsInterruptedData) {
  reader::ReaderState source{.anchor = {.chapter = 7, .byteOffset = 123456, .nodeOffset = 12},
                             .pageNumber = 91,
                             .layoutRevision = 3,
                             .counterMode = reader::PageCounterMode::ByBook,
                             .orientation = 1,
                             .fontScale = 130,
                             .flags = 5};
  std::array<uint8_t, reader::ReaderStateCodec::kSerializedSize> bytes = {};
  ASSERT_TRUE(reader::ReaderStateCodec::encode(source, bytes));

  reader::ReaderState restored;
  ASSERT_TRUE(reader::ReaderStateCodec::decode(bytes.data(), bytes.size(), restored));
  EXPECT_EQ(restored.anchor.chapter, 7U);
  EXPECT_EQ(restored.anchor.byteOffset, 123456U);
  EXPECT_EQ(restored.pageNumber, 91U);
  EXPECT_EQ(restored.counterMode, reader::PageCounterMode::ByBook);

  bytes[20] ^= 0x01U;
  EXPECT_FALSE(reader::ReaderStateCodec::decode(bytes.data(), bytes.size(), restored));
}

}  // namespace
