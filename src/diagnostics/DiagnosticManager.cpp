#include "DiagnosticManager.h"

#include <HalPowerManager.h>
#include <HalStorage.h>
#include <MemoryBudget.h>
#include <esp_system.h>
#include <stdarg.h>

#include "AppVersion.h"

DiagnosticManager& DiagnosticManager::instance() {
  static DiagnosticManager mgr;
  return mgr;
}

void DiagnosticManager::begin() {
  if (loaded_) return;
  loaded_ = true;
  if (!Storage.ready()) return;
  char buf[16] = {};
  if (Storage.readFileToBuffer(CFG_PATH, buf, sizeof(buf)) > 0) {
    flags_ = static_cast<uint8_t>(strtoul(buf, nullptr, 10));
  }
}

void DiagnosticManager::save() {
  if (!Storage.ready()) return;
  Storage.ensureDirectoryExists("/.inkmod");
  Storage.writeFile(CFG_PATH, String(flags_));
}

void DiagnosticManager::setFlag(Flag f, bool on) {
  begin();
  const uint8_t bit = static_cast<uint8_t>(f);
  if (f != Developer && on) flags_ |= static_cast<uint8_t>(Developer);
  if (on) flags_ |= bit; else flags_ &= static_cast<uint8_t>(~bit);
  if (f == Developer && !on) flags_ = 0;
  save();
}

void DiagnosticManager::rotateIfNeeded() {
  if (!Storage.exists(LOG_PATH)) return;
  HalFile f;
  if (!Storage.openFileForRead("DIAG", LOG_PATH, f)) return;
  const uint64_t size = f.fileSize64();
  f.close();
  if (size < MAX_LOG_BYTES) return;
  Storage.remove(OLD_LOG_PATH);
  Storage.rename(LOG_PATH, OLD_LOG_PATH);
}

void DiagnosticManager::append(const String& line) {
  if (!developerMode() || !Storage.ready()) return;
  Storage.ensureDirectoryExists("/diagnostics");
  rotateIfNeeded();
  String existing;
  if (Storage.exists(LOG_PATH)) existing = Storage.readFile(LOG_PATH);
  existing += line;
  existing += '\n';
  Storage.writeFile(LOG_PATH, existing);
}

void DiagnosticManager::log(const char* category, const char* message) {
  begin();
  if (!developerMode()) return;
  char prefix[64];
  snprintf(prefix, sizeof(prefix), "[%lu] [%s] ", millis(), category ? category : "DIAG");
  append(String(prefix) + (message ? message : ""));
}

void DiagnosticManager::logf(const char* category, const char* format, ...) {
  begin();
  if (!developerMode()) return;
  char msg[224];
  va_list ap;
  va_start(ap, format);
  vsnprintf(msg, sizeof(msg), format, ap);
  va_end(ap);
  log(category, msg);
}

bool DiagnosticManager::snapshot(const char* activity, const char* book) {
  begin();
  if (!developerMode()) return false;
  const auto mem = MemoryBudget::snapshot();
  logf("SNAP", "fw=%s activity=%s heap=%u largest=%u battery=%u%% reset=%d book=%s",
       INKMOD_VERSION, activity ? activity : "-", static_cast<unsigned>(mem.freeHeap),
       static_cast<unsigned>(mem.maxAllocHeap), static_cast<unsigned>(powerManager.getBatteryPercentage()),
       static_cast<int>(esp_reset_reason()), book ? book : "-");
  return true;
}

bool DiagnosticManager::createReport() {
  begin();
  if (!Storage.ready()) return false;
  const auto mem = MemoryBudget::snapshot();
  String report;
  report.reserve(1400);
  report += "inkMOD diagnostic report\n";
  report += "Firmware: "; report += INKMOD_VERSION; report += "\n";
  report += "Variant: "; report += INKMOD_FIRMWARE_VARIANT; report += "\n";
  report += "Uptime ms: "; report += String(millis()); report += "\n";
  report += "Reset reason: "; report += String(static_cast<int>(esp_reset_reason())); report += "\n";
  report += "Battery: "; report += String(powerManager.getBatteryPercentage()); report += "%\n";
  report += "Free heap: "; report += String(mem.freeHeap); report += "\n";
  report += "Largest free block: "; report += String(mem.maxAllocHeap); report += "\n";
  report += "SD total: "; report += String(static_cast<unsigned long long>(Storage.getCardTotalBytes())); report += "\n";
  report += "Developer flags: "; report += String(flags_); report += "\n";
  report += "Crash report: "; report += Storage.exists("/crash_report.txt") ? "yes\n" : "no\n";
  if (Storage.exists("/crash_report.txt")) {
    report += "\n--- crash_report.txt ---\n";
    report += Storage.readFile("/crash_report.txt");
  }
  Storage.ensureDirectoryExists("/diagnostics");
  return Storage.writeFile(REPORT_PATH, report);
}

void DiagnosticManager::clearFiles() {
  Storage.remove(LOG_PATH);
  Storage.remove(OLD_LOG_PATH);
  Storage.remove(REPORT_PATH);
  Storage.remove("/crash_report.txt");
}
