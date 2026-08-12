// Non-owning reader for small, bounded data. It never owns or allocates RAM.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "IByteReader.h"

class MemoryByteReader final : public IByteReader {
 public:
  MemoryByteReader(const void* data, size_t size) : data_(static_cast<const uint8_t*>(data)), size_(size) {}

  size_t read(void* dst, size_t len) override {
    if (!dst || !data_ || position_ >= size_) return 0;
    const size_t count = std::min(len, size_ - position_);
    std::memcpy(dst, data_ + position_, count);
    position_ += count;
    return count;
  }
  bool seek(uint64_t pos) override {
    if (pos > size_) return false;
    position_ = static_cast<size_t>(pos);
    return true;
  }
  uint64_t tell() const override { return position_; }
  uint64_t size() const override { return size_; }

 private:
  const uint8_t* data_ = nullptr;  // Source must outlive this reader.
  size_t size_ = 0;
  size_t position_ = 0;
};

