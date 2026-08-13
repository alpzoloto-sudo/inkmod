// Bounded reader-work policy shared by the active EPUB/FB2 reader pipeline.
// It deliberately owns no task, file or heap buffer: the ESP32-C3 performs
// one synchronous operation at a time and callers only pass this tiny token
// down to streaming loops that can stop at safe chunk boundaries.
#pragma once

#include <atomic>
#include <cstdint>

namespace reader {

enum class ReaderMemoryMode : uint8_t { Normal, Safe, Survival, Unavailable };

struct ReaderMemoryPolicy {
  ReaderMemoryMode mode = ReaderMemoryMode::Unavailable;
  bool allowSdFont = false;
  bool allowEmbeddedStyle = false;
  bool allowImages = false;
  bool allowHyphenation = false;
  bool allowReadingEffects = false;
};

// Keep an explicit reserve above ChapterHtmlSlimParser's hard 44 KiB/32 KiB
// layout floor. Both values matter: total free heap can look healthy while
// fragmentation leaves no contiguous block for Expat, a font or a decoder.
inline ReaderMemoryPolicy selectReaderMemoryPolicy(const uint32_t freeHeap, const uint32_t maxAllocHeap) {
  if (freeHeap >= 96U * 1024U && maxAllocHeap >= 56U * 1024U) {
    return {ReaderMemoryMode::Normal, true, true, true, true, true};
  }
  if (freeHeap >= 72U * 1024U && maxAllocHeap >= 44U * 1024U) {
    return {ReaderMemoryMode::Safe, false, false, false, true, false};
  }
  if (freeHeap >= 52U * 1024U && maxAllocHeap >= 34U * 1024U) {
    return {ReaderMemoryMode::Survival, false, false, false, false, false};
  }
  return {};
}

class ReaderCancellationToken final {
 public:
  ReaderCancellationToken() = default;

  bool isCancellationRequested() const {
    return generation_ != nullptr && generation_->load(std::memory_order_relaxed) != expectedGeneration_;
  }

 private:
  friend class ReaderWorkController;
  ReaderCancellationToken(const std::atomic<uint32_t>* generation, const uint32_t expectedGeneration)
      : generation_(generation), expectedGeneration_(expectedGeneration) {}

  const std::atomic<uint32_t>* generation_ = nullptr;
  uint32_t expectedGeneration_ = 0;
};

class ReaderWorkController final {
 public:
  ReaderCancellationToken begin() {
    const uint32_t generation = generation_.load(std::memory_order_relaxed);
    active_.store(true, std::memory_order_release);
    return ReaderCancellationToken(&generation_, generation);
  }

  void cancel() { generation_.fetch_add(1, std::memory_order_relaxed); }
  void finish() { active_.store(false, std::memory_order_release); }
  bool active() const { return active_.load(std::memory_order_acquire); }

 private:
  std::atomic<uint32_t> generation_{1};
  std::atomic<bool> active_{false};
};

class ScopedReaderWork final {
 public:
  explicit ScopedReaderWork(ReaderWorkController& controller) : controller_(controller), token_(controller.begin()) {}
  ~ScopedReaderWork() { controller_.finish(); }

  ScopedReaderWork(const ScopedReaderWork&) = delete;
  ScopedReaderWork& operator=(const ScopedReaderWork&) = delete;

  const ReaderCancellationToken& token() const { return token_; }

 private:
  ReaderWorkController& controller_;
  ReaderCancellationToken token_;
};

}  // namespace reader
