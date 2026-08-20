#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "src/reader/core/io/BufferedByteReader.h"

namespace {

class CountingVectorReader final : public IByteReader {
 public:
  explicit CountingVectorReader(size_t size) : data_(size) {
    for (size_t i = 0; i < data_.size(); ++i) data_[i] = static_cast<uint8_t>(i & 0xffU);
  }

  size_t read(void* dst, size_t len) override {
    ++readCalls;
    const size_t available = position_ < data_.size() ? data_.size() - position_ : 0;
    const size_t count = std::min(len, available);
    if (count) std::memcpy(dst, data_.data() + position_, count);
    position_ += count;
    return count;
  }

  bool seek(uint64_t pos) override {
    ++seekCalls;
    if (pos > data_.size()) return false;
    position_ = static_cast<size_t>(pos);
    return true;
  }

  uint64_t tell() const override { return position_; }
  uint64_t size() const override { return data_.size(); }

  uint32_t readCalls = 0;
  uint32_t seekCalls = 0;

 private:
  std::vector<uint8_t> data_;
  size_t position_ = 0;
};

TEST(BufferedByteReader, CoalescesSmallSequentialReads) {
  CountingVectorReader source(10000);
  BufferedByteReader buffered(source);
  std::array<uint8_t, 256> chunk{};

  for (int i = 0; i < 16; ++i) {
    ASSERT_EQ(buffered.read(chunk.data(), chunk.size()), chunk.size());
    EXPECT_EQ(chunk[0], static_cast<uint8_t>((i * chunk.size()) & 0xffU));
  }
  EXPECT_EQ(buffered.tell(), 4096U);
  EXPECT_EQ(source.readCalls, 1U);

  ASSERT_EQ(buffered.read(chunk.data(), chunk.size()), chunk.size());
  EXPECT_EQ(source.readCalls, 2U);
}

TEST(BufferedByteReader, ReusesWindowForSeekInsideReadAhead) {
  CountingVectorReader source(10000);
  BufferedByteReader buffered(source);
  std::array<uint8_t, 64> chunk{};

  ASSERT_EQ(buffered.read(chunk.data(), chunk.size()), chunk.size());
  ASSERT_EQ(source.readCalls, 1U);
  ASSERT_TRUE(buffered.seek(1024));
  ASSERT_EQ(buffered.read(chunk.data(), chunk.size()), chunk.size());
  EXPECT_EQ(source.readCalls, 1U);
  EXPECT_EQ(source.seekCalls, 0U);
  EXPECT_EQ(chunk[0], 0U);  // byte 1024 modulo 256
}

TEST(BufferedByteReader, LargeScanReadsBypassReadAhead) {
  CountingVectorReader source(10000);
  BufferedByteReader buffered(source);
  std::array<uint8_t, 4096> chunk{};

  ASSERT_EQ(buffered.read(chunk.data(), chunk.size()), chunk.size());
  EXPECT_EQ(source.readCalls, 1U);
  EXPECT_EQ(source.seekCalls, 0U);
  EXPECT_EQ(buffered.tell(), chunk.size());
}

TEST(BufferedByteReader, ReportsLogicalPositionDespiteReadAhead) {
  CountingVectorReader source(10000);
  BufferedByteReader buffered(source);
  std::array<uint8_t, 10> chunk{};

  ASSERT_EQ(buffered.read(chunk.data(), chunk.size()), chunk.size());
  EXPECT_EQ(source.tell(), BufferedByteReader::kReadAheadSize);
  EXPECT_EQ(buffered.tell(), 10U);
  ASSERT_TRUE(buffered.seek(5));
  EXPECT_EQ(buffered.tell(), 5U);
  ASSERT_EQ(buffered.read(chunk.data(), 1), 1U);
  EXPECT_EQ(chunk[0], 5U);
}

}  // namespace
