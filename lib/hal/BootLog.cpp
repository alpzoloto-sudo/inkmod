#include "BootLog.h"

#ifdef ENABLE_BOOT_LOG_SD

#include <Arduino.h>
#include <cstdarg>
#include <cstring>

#include "AppVersion.h"
#include "HalStorage.h"

namespace BootLog {

namespace {
constexpr char PATH[] = "/boot_log.txt";
bool started = false;

// Opens, appends `text` (already newline-terminated), syncs and closes.
// Deliberately does the full open/write/sync/close dance on every call
// instead of keeping the file handle open: if the very next line of boot
// code hangs forever, a held-open handle's buffered data could still be
// lost, but a closed-and-synced write is already committed to the card.
void appendLine(const char* text) {
  auto file = Storage.open(PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return;
  file.write(text, strlen(text));
  file.sync();
  file.close();
}
}  // namespace

void begin() {
  char header[112];
  snprintf(header, sizeof(header), "\n=== boot @ %lums | inkMOD " INKMOD_VERSION " (" INKMOD_FIRMWARE_VARIANT ") ===\n",
            static_cast<unsigned long>(millis()));
  appendLine(header);
  started = true;
}

void step(const char* tag, const char* message) {
  if (!started) return;
  char line[192];
  snprintf(line, sizeof(line), "[%8lums] %-6s %s\n", static_cast<unsigned long>(millis()), tag, message);
  appendLine(line);
}

void stepf(const char* tag, const char* fmt, ...) {
  if (!started) return;
  char message[144];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  step(tag, message);
}

}  // namespace BootLog

#else  // !ENABLE_BOOT_LOG_SD

// Disabled: every call is a no-op that the compiler inlines away, so there is
// no SD I/O and no measurable cost in normal (non-diagnostic) builds.
namespace BootLog {
void begin() {}
void step(const char*, const char*) {}
void stepf(const char*, const char*, ...) {}
}  // namespace BootLog

#endif  // ENABLE_BOOT_LOG_SD
