// Small deterministic job queue for reader preparation work. It deliberately
// contains descriptors only: parsers, images and file handles stay owned by
// their activity/session and are never transferred through a heap queue.
#pragma once

#include <array>
#include <cstdint>

namespace reader {

enum class ReaderJobKind : uint8_t { InspectBook, ExtractSource, BuildIndex, Paginate, DecodeImage, CleanupCache };

struct ReaderJob {
  ReaderJobKind kind = ReaderJobKind::InspectBook;
  uint32_t bookKey = 0;
  uint32_t argument = 0;
};

class ReaderJobQueue {
 public:
  static constexpr uint8_t kCapacity = 8;

  bool enqueue(const ReaderJob& job);
  bool dequeue(ReaderJob& out);
  void clear();
  uint8_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

 private:
  std::array<ReaderJob, kCapacity> entries_ = {};
  uint8_t head_ = 0;
  uint8_t size_ = 0;
};

}  // namespace reader
