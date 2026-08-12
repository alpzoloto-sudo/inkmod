#include "ReaderJobQueue.h"

namespace reader {

bool ReaderJobQueue::enqueue(const ReaderJob& job) {
  if (size_ == kCapacity) return false;
  const uint8_t tail = static_cast<uint8_t>((head_ + size_) % kCapacity);
  entries_[tail] = job;
  ++size_;
  return true;
}

bool ReaderJobQueue::dequeue(ReaderJob& out) {
  if (size_ == 0) return false;
  out = entries_[head_];
  head_ = static_cast<uint8_t>((head_ + 1) % kCapacity);
  --size_;
  return true;
}

void ReaderJobQueue::clear() {
  head_ = 0;
  size_ = 0;
}

}  // namespace reader
