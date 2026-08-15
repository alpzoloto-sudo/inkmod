#pragma once

#include <Arduino.h>
#include <cstdint>
#include <string>

class DiagnosticManager {
 public:
  enum Flag : uint8_t {
    Developer = 1 << 0,
    Extended = 1 << 1,
    Reader = 1 << 2,
    Filesystem = 1 << 3,
    Memory = 1 << 4,
    Network = 1 << 5,
    Display = 1 << 6,
  };

  static DiagnosticManager& instance();
  void begin();
  bool enabled(Flag f) const { return (flags_ & static_cast<uint8_t>(f)) != 0; }
  bool developerMode() const { return enabled(Developer); }
  uint8_t flags() const { return flags_; }
  void setFlag(Flag f, bool on);
  void toggle(Flag f) { setFlag(f, !enabled(f)); }
  void log(const char* category, const char* message);
  void logf(const char* category, const char* format, ...);
  bool snapshot(const char* activity = nullptr, const char* book = nullptr);
  bool createReport();
  void clearFiles();

  static constexpr const char* LOG_PATH = "/diagnostics/inkmod.log";
  static constexpr const char* OLD_LOG_PATH = "/diagnostics/inkmod.log.old";
  static constexpr const char* REPORT_PATH = "/diagnostics/inkmod-diagnostic.txt";

 private:
  uint8_t flags_ = 0;
  bool loaded_ = false;
  static constexpr const char* CFG_PATH = "/.inkmod/diagnostics.cfg";
  static constexpr size_t MAX_LOG_BYTES = 24 * 1024;
  void save();
  void rotateIfNeeded();
  void append(const String& line);
};

#define DIAGNOSTICS DiagnosticManager::instance()
