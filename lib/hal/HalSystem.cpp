#include "HalSystem.h"

#include <string>

#include "AppVersion.h"
#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"

#define MAX_PANIC_STACK_DEPTH 32

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];

namespace {
constexpr uint32_t BREADCRUMB_MAGIC = 0x494E4B42;  // INKB
constexpr uint32_t DIAGNOSTIC_REBOOT_MAGIC = 0x494E4B44;  // INKD
constexpr size_t BREADCRUMB_COUNT = 12;
constexpr size_t BREADCRUMB_LEN = 48;

struct BreadcrumbStore {
  uint32_t magic;
  uint8_t head;
  uint8_t count;
  char entries[BREADCRUMB_COUNT][BREADCRUMB_LEN];
};

RTC_NOINIT_ATTR BreadcrumbStore breadcrumbStore;
RTC_NOINIT_ATTR uint32_t diagnosticRebootMagic;

void clearBreadcrumbStore() {
  breadcrumbStore.magic = BREADCRUMB_MAGIC;
  breadcrumbStore.head = 0;
  breadcrumbStore.count = 0;
  for (size_t i = 0; i < BREADCRUMB_COUNT; ++i) breadcrumbStore.entries[i][0] = '\0';
}

void sanitizeBreadcrumbStore() {
  if (breadcrumbStore.magic != BREADCRUMB_MAGIC || breadcrumbStore.head >= BREADCRUMB_COUNT ||
      breadcrumbStore.count > BREADCRUMB_COUNT) {
    clearBreadcrumbStore();
  }
}
}

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }

  __real_panic_print_backtrace(frame, core);
}
}

namespace HalSystem {

void begin() {
  sanitizeBreadcrumbStore();
  // This is mostly for the first boot, we need to initialize the panic info and logs to empty state
  // If we reboot from a panic state, we want to keep the panic info until we successfully dump it to the SD card, use
  // `clearPanic()` to clear it after dumping
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  const bool panicReset = isRebootFromPanic();
  const bool diagnosticReset = diagnosticRebootMagic == DIAGNOSTIC_REBOOT_MAGIC;
  if (!panicReset && !diagnosticReset) return;

  diagnosticRebootMagic = 0;
  std::string report;
  if (panicReset) {
    report = getPanicInfo(true);
  } else {
    report = "inkMOD version: " INKMOD_VERSION;
    report += "\ninkMOD variant: " INKMOD_FIRMWARE_VARIANT;
    report += "\n\nDiagnostic reboot (usually OOM or guarded restart).\n";
  }
  const std::string breadcrumbs = getBreadcrumbs();
  if (!breadcrumbs.empty()) report += "\nLast breadcrumbs:\n" + breadcrumbs;

  auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
  if (file) {
    file.write(report.c_str(), report.size());
    file.close();
    LOG_INF("SYS", "Dumped crash diagnostics to SD card");
  } else {
    LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
  }
  clearBreadcrumbStore();
}

void clearPanic() {
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  clearLastLogs();
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "inkMOD version: " INKMOD_VERSION;
    info += "\ninkMOD variant: " INKMOD_FIRMWARE_VARIANT;
    info += "\n\nPanic reason: " + std::string(panicMessage);
    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  return resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP;
}

void recordBreadcrumb(const char* message) {
  if (!message) return;
  sanitizeBreadcrumbStore();
  char* dst = breadcrumbStore.entries[breadcrumbStore.head];
  size_t i = 0;
  for (; i + 1 < BREADCRUMB_LEN && message[i]; ++i) dst[i] = message[i];
  dst[i] = '\0';
  breadcrumbStore.head = static_cast<uint8_t>((breadcrumbStore.head + 1) % BREADCRUMB_COUNT);
  if (breadcrumbStore.count < BREADCRUMB_COUNT) ++breadcrumbStore.count;
}

void markDiagnosticReboot() { diagnosticRebootMagic = DIAGNOSTIC_REBOOT_MAGIC; }

std::string getBreadcrumbs() {
  sanitizeBreadcrumbStore();
  if (breadcrumbStore.count == 0) return {};
  std::string out;
  out.reserve(breadcrumbStore.count * (BREADCRUMB_LEN + 2));
  const size_t start = (breadcrumbStore.head + BREADCRUMB_COUNT - breadcrumbStore.count) % BREADCRUMB_COUNT;
  for (size_t n = 0; n < breadcrumbStore.count; ++n) {
    const size_t idx = (start + n) % BREADCRUMB_COUNT;
    out += breadcrumbStore.entries[idx];
    out += '\n';
  }
  return out;
}

}  // namespace HalSystem
