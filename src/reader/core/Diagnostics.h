// Small, allocation-free diagnostics for the new reader path.
#pragma once

#include <Arduino.h>
#include <Logging.h>

#include <cstdint>

namespace reader {

struct MemoryStats {
  uint32_t freeHeap = 0;
  uint32_t minFreeHeap = 0;
  uint32_t maxAlloc = 0;

  static MemoryStats capture() {
    return {.freeHeap = ESP.getFreeHeap(), .minFreeHeap = ESP.getMinFreeHeap(), .maxAlloc = ESP.getMaxAllocHeap()};
  }

  // Both checks matter: a fragmented heap can have enough total free RAM but
  // no contiguous block for the requested operation.
  bool canReserve(const uint32_t bytes, const uint32_t reserveAfter = 0) const {
    return maxAlloc >= bytes && freeHeap >= bytes + reserveAfter;
  }

  void log(const char* phase) const {
    LOG_DBG("RCORE", "[MEM] %s free=%u min=%u maxAlloc=%u", phase ? phase : "unknown", freeHeap, minFreeHeap,
            maxAlloc);
  }
};

class ScopedPerfTimer final {
 public:
  explicit ScopedPerfTimer(const char* operation) : operation_(operation), startedAt_(millis()) {}
  ~ScopedPerfTimer() {
    LOG_DBG("RCORE", "[PERF] %s = %lu ms", operation_ ? operation_ : "unknown", millis() - startedAt_);
  }

  ScopedPerfTimer(const ScopedPerfTimer&) = delete;
  ScopedPerfTimer& operator=(const ScopedPerfTimer&) = delete;

 private:
  const char* operation_ = nullptr;  // Caller-provided static label.
  unsigned long startedAt_ = 0;
};

}  // namespace reader

