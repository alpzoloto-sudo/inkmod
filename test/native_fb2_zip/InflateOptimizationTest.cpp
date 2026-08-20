#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Inflate.h"
#include "src/reader/core/io/IByteReader.h"

namespace {

class InflateMemoryReader final : public IByteReader {
 public:
  explicit InflateMemoryReader(std::vector<uint8_t> data) : data_(std::move(data)) {}

  size_t read(void* dst, size_t len) override {
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

 private:
  std::vector<uint8_t> data_;
  size_t position_ = 0;
};

TEST(NativeFb2Inflate, ExpandsLongDistanceOneRun) {
  // Raw DEFLATE generated from 1000 'A' bytes. The stream contains a long
  // repeated match and exercises the distance==1 optimized copy path.
  const std::vector<uint8_t> compressed = {
      0x73, 0x74, 0x1c, 0x05, 0xa3, 0x60, 0x14, 0x0c, 0x77, 0x00, 0x00};
  InflateMemoryReader reader(compressed);

  std::string output;
  ASSERT_TRUE(inflateRaw(reader, static_cast<uint32_t>(compressed.size()),
                         [&](const uint8_t* data, size_t len) {
                           output.append(reinterpret_cast<const char*>(data), len);
                         }));
  ASSERT_EQ(output.size(), 1000U);
  EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](char c) { return c == 'A'; }));
}

}  // namespace
