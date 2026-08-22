#pragma once

// Minimal, dependency-light boot-time logger that appends one line per call
// directly to the SD card and closes the file immediately after every write.
//
// Compiled out by default (no SD writes, calls become no-ops) - see
// ENABLE_BOOT_LOG_SD below. Re-enable by adding -DENABLE_BOOT_LOG_SD=1 to
// build_flags in platformio.ini (or to a specific [env:...] section) the
// next time boot needs to be diagnosed without a UART connection.
//
// Why this exists (and why it's separate from the existing RTC breadcrumb
// trail in HalSystem.cpp): the breadcrumb trail lives in RTC_NOINIT memory
// and is only ever dumped to /crash_report.txt after a *panic/reset*
// (see HalSystem::checkPanic()). A true hang - the firmware sitting in an
// unbounded or very-long loop with no crash and no reset - never triggers
// that dump, and RTC memory itself is not guaranteed to survive a hard
// power-cycle (unplug/battery-pull), which is often the only way to recover
// a frozen board. This logger instead commits each line to the SD card the
// moment it happens, so it survives both a hang AND a subsequent hard power
// cycle, and can be read on a PC afterwards without any UART connection.
//
// Usage:
//   BootLog::begin();               // once, right after Storage.begin() succeeds
//   BootLog::step("MAIN", "gpio.begin() done, device=X3");
//
// Every call opens the file in append mode, writes, calls sync(), and
// closes it again - slower than buffered logging, but that cost is what
// guarantees the line is actually on the card before the *next* line of
// firmware code runs (which might be the one that never returns).

namespace BootLog {

// Starts a new boot session: appends a separator + firmware version to
// /boot_log.txt. Safe to call multiple times (e.g. after a silent reboot);
// each call just adds another session marker to the same file so the full
// boot history accumulates across resets instead of being overwritten.
void begin();

// Appends "[<millis>ms] <tag>: <message>" to /boot_log.txt and flushes it to
// the card immediately. No-op if begin() hasn't been called yet or the SD
// card isn't mounted.
void step(const char* tag, const char* message);

// Same as step(), but with printf-style formatting. Keep messages short -
// the internal buffer is bounded (see BootLog.cpp).
void stepf(const char* tag, const char* fmt, ...);

}  // namespace BootLog
