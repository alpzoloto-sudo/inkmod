// Bounded read-ahead adapter for seekable byte sources.
//
// Intended for slow storage where consumers issue many small sequential reads
// (FB2 binary decode and section rendering are typical examples). Large reads
// already amortize storage latency and therefore bypass the buffer entirely.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

#include "IByteReader.h"

class BufferedByteReader final : public IByteReader {
 public:
  static constexpr size_t kReadAheadSize = 4096;
  static constexpr size_t kDirectReadThreshold = 2048;

  explicit BufferedByteReader(IByteReader& source)
      : source_(source), size_(source.size()), position_(source.tell()), sourcePosition_(position_) {}

  size_t read(void* dst, size_t len) override {
    if (!dst || len == 0 || position_ >= size_) return 0;
    const uint64_t remaining = size_ - position_;
    if (len > remaining) len = static_cast<size_t>(remaining);

    // Large consumers such as the 8 KiB FB2 scanner already amortize SD
    // latency. Avoid an extra memcpy and, importantly, avoid allocating a
    // read-ahead buffer that such a path would never benefit from.
    if (len >= kDirectReadThreshold) {
      bufferLength_ = 0;
      if (!positionSource(position_)) return 0;
      const size_t got = source_.read(dst, len);
      sourcePosition_ += got;
      position_ += got;
      return got;
    }

    if (!buffer_ && !allocateBuffer()) {
      if (!positionSource(position_)) return 0;
      const size_t got = source_.read(dst, len);
      sourcePosition_ += got;
      position_ += got;
      return got;
    }

    auto* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < len) {
      if (!bufferContains(position_) && !refill()) break;
      const size_t offset = static_cast<size_t>(position_ - bufferStart_);
      const size_t available = bufferLength_ - offset;
      const size_t take = std::min(available, len - done);
      std::memcpy(out + done, buffer_.get() + offset, take);
      done += take;
      position_ += take;
    }
    return done;
  }

  bool seek(uint64_t pos) override {
    if (pos > size_) return false;
    position_ = pos;
    // Keep a valid window: short backward/forward seeks inside it become pure
    // RAM operations. A seek outside the window is deferred until refill/read.
    return true;
  }

  uint64_t tell() const override { return position_; }
  uint64_t size() const override { return size_; }

 private:
  bool allocateBuffer() {
    buffer_.reset(new (std::nothrow) uint8_t[kReadAheadSize]);
    return static_cast<bool>(buffer_);
  }

  bool bufferContains(uint64_t pos) const {
    return buffer_ && bufferLength_ > 0 && pos >= bufferStart_ && pos < bufferStart_ + bufferLength_;
  }

  bool positionSource(uint64_t pos) {
    if (sourcePosition_ == pos) return true;
    if (!source_.seek(pos)) return false;
    sourcePosition_ = pos;
    return true;
  }

  bool refill() {
    if (!buffer_ || position_ >= size_) return false;
    if (!positionSource(position_)) return false;

    const uint64_t remaining = size_ - position_;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, kReadAheadSize));
    const size_t got = source_.read(buffer_.get(), want);
    if (got == 0) {
      bufferLength_ = 0;
      return false;
    }
    bufferStart_ = position_;
    bufferLength_ = got;
    sourcePosition_ += got;
    return true;
  }

  IByteReader& source_;
  uint64_t size_ = 0;
  uint64_t position_ = 0;
  uint64_t sourcePosition_ = 0;
  std::unique_ptr<uint8_t[]> buffer_;
  uint64_t bufferStart_ = 0;
  size_t bufferLength_ = 0;
};
