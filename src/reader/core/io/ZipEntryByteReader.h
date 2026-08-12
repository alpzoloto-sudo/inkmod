// Bounded reader for an already-uncompressed ZIP_STORED entry.
// Deflated entries must be extracted to SD cache; never buffer them in RAM.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "IByteReader.h"

class ZipEntryByteReader final : public IByteReader {
 public:
  ZipEntryByteReader(IByteReader& source, uint64_t sourceOffset, uint64_t entrySize)
      : source_(source), sourceOffset_(sourceOffset), size_(entrySize) {}

  size_t read(void* dst, size_t len) override {
    if (!dst || position_ >= size_ || !source_.seek(sourceOffset_ + position_)) return 0;
    const uint64_t remaining = size_ - position_;
    const size_t count = static_cast<size_t>(std::min<uint64_t>(len, remaining));
    const size_t got = source_.read(dst, count);
    position_ += got;
    return got;
  }
  bool seek(uint64_t pos) override {
    if (pos > size_) return false;
    position_ = pos;
    return true;
  }
  uint64_t tell() const override { return position_; }
  uint64_t size() const override { return size_; }

 private:
  IByteReader& source_;
  uint64_t sourceOffset_ = 0;
  uint64_t size_ = 0;
  uint64_t position_ = 0;
};

