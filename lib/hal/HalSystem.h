#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();

// Allocation-free RTC breadcrumb trail for release diagnostics.
void recordBreadcrumb(const char* message);
void markDiagnosticReboot();
std::string getBreadcrumbs();
}  // namespace HalSystem
