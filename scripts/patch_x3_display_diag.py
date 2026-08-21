from pathlib import Path

Import("env")

if env.get("PIOENV") != "x3diag":
    Return()

root = Path(env.subst("$PROJECT_DIR"))
main = root / "src" / "main.cpp"
text = main.read_text(encoding="utf-8")

marker = "// INKMOD_X3_DISPLAY_DIAG_PATCH"
if marker in text:
    Return()

include_anchor = "#include <Arduino.h>\n"
include_block = r'''#include <Arduino.h>
#ifdef X3_DISPLAY_DIAG
#include <XteinkDetect.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#endif
'''
if include_anchor not in text:
    raise RuntimeError("X3 diag: Arduino include anchor not found")
text = text.replace(include_anchor, include_block, 1)

setup_anchor = "void setup() {\n"
diag_code = r'''// INKMOD_X3_DISPLAY_DIAG_PATCH
#ifdef X3_DISPLAY_DIAG
static bool x3DisplayDiagHalt = false;

static void diagAppend(std::string& out, const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n > 0) out.append(buf, static_cast<size_t>(std::min(n, static_cast<int>(sizeof(buf) - 1))));
}

static bool writeX3DiagReport(const std::string& report) {
  HalFile file;
  if (!Storage.openFileForWrite("X3D", "/x3-display-report.txt", file)) return false;
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(report.data()), report.size());
  file.close();
  return written == report.size();
}

static const char* boardVerdictName(freeink::XteinkVerdict v) {
  switch (v) {
    case freeink::XteinkVerdict::X3Confirmed: return "X3Confirmed";
    case freeink::XteinkVerdict::X4Confirmed: return "X4Confirmed";
    default: return "Inconclusive";
  }
}

static const char* displayVerdictName(freeink::DisplayControllerVerdict v) {
  switch (v) {
    case freeink::DisplayControllerVerdict::Uc81xxConfirmed: return "Uc81xxConfirmed";
    case freeink::DisplayControllerVerdict::PrimaryAssumed: return "PrimaryAssumed";
    default: return "Inconclusive";
  }
}

static void runX3DisplayDiagnostic() {
  x3DisplayDiagHalt = true;
  std::string report;
  report.reserve(1800);
  diagAppend(report, "inkMOD X3 Display Diagnostic\n");
  diagAppend(report, "============================\n");
  diagAppend(report, "Diagnostic build: x3diag-v1\n");
  diagAppend(report, "Firmware base: 1.1.5\n");
  diagAppend(report, "Boot millis: %lu\n\n", static_cast<unsigned long>(millis()));

  uint8_t score1 = 0, score2 = 0;
  const auto boardVerdict = freeink::detectXteinkVerdict(&score1, &score2);
  diagAppend(report, "Board detection\n---------------\n");
  diagAppend(report, "gpio.deviceIsX3: %s\n", gpio.deviceIsX3() ? "YES" : "NO");
  diagAppend(report, "fingerprint verdict: %s (%u)\n", boardVerdictName(boardVerdict), static_cast<unsigned>(boardVerdict));
  diagAppend(report, "fingerprint pass scores: %u / %u (0..3)\n\n", score1, score2);

  diagAppend(report, "Factory hw_calib\n----------------\n");
  nvs_handle_t h = 0;
  esp_err_t openErr = nvs_open("hw_calib", NVS_READONLY, &h);
  diagAppend(report, "nvs_open(hw_calib): %s (0x%X)\n", esp_err_to_name(openErr), static_cast<unsigned>(openErr));
  if (openErr == ESP_OK) {
    uint8_t st8 = 0;
    uint32_t st32 = 0;
    esp_err_t e8 = nvs_get_u8(h, "screenType", &st8);
    esp_err_t e32 = nvs_get_u32(h, "screenType", &st32);
    diagAppend(report, "screenType as u8: status=%s value=%u (0x%02X)\n", esp_err_to_name(e8), st8, st8);
    diagAppend(report, "screenType as u32: status=%s value=%lu (0x%08lX)\n", esp_err_to_name(e32),
               static_cast<unsigned long>(st32), static_cast<unsigned long>(st32));
    nvs_close(h);
  }

  diagAppend(report, "\nDisplay probe\n-------------\n");
  diagAppend(report, "stage: PRE_PROBE (report safely written before touching panel bus)\n");
  writeX3DiagReport(report);

  if (!gpio.deviceIsX3()) {
    diagAppend(report, "ABORTED: board is not detected as X3; panel probe was NOT run.\n");
    diagAppend(report, "stage: FINISHED\n");
    writeX3DiagReport(report);
    return;
  }

  uint8_t ver[5] = {0, 0, 0, 0, 0};
  uint8_t flg = 0;
  const auto displayVerdict = freeink::detectXteinkDisplayController(ver, &flg);
  const auto& d = freeink::getXteinkDisplayProbeDiag();

  diagAppend(report, "stage: POST_PROBE\n");
  diagAppend(report, "verdict: %s (%u)\n", displayVerdictName(displayVerdict), static_cast<unsigned>(displayVerdict));
  diagAppend(report, "VER raw: %02X %02X %02X %02X %02X\n", ver[0], ver[1], ver[2], ver[3], ver[4]);
  diagAppend(report, "FLG raw: %02X\n", flg);
  diagAppend(report, "diag.valid: %s\n", d.valid ? "YES" : "NO");
  diagAppend(report, "diag.promoted: %s\n", d.promoted ? "YES" : "NO");
  diagAppend(report, "diag.verdict(raw): %u\n", static_cast<unsigned>(d.verdict));
  diagAppend(report, "diag.VER: %02X %02X %02X %02X %02X\n", d.ver[0], d.ver[1], d.ver[2], d.ver[3], d.ver[4]);
  diagAppend(report, "diag.FLG: %02X\n", d.flg);
  diagAppend(report, "MTP valid: %s\n", d.mtpValid ? "YES" : "NO");
  if (d.mtpValid) {
    diagAppend(report, "MTP[0..47]:\n");
    for (unsigned i = 0; i < 48; i += 16) {
      diagAppend(report, "%02X: ", i);
      for (unsigned j = 0; j < 16; ++j) diagAppend(report, "%02X%s", d.mtp[i + j], j == 15 ? "" : " ");
      diagAppend(report, "\n");
    }
  }
  diagAppend(report, "stage: FINISHED\n");
  diagAppend(report, "\nNOTE: This diagnostic intentionally does NOT initialize the normal display driver.\n");
  writeX3DiagReport(report);
}
#endif

void setup() {
'''
if setup_anchor not in text:
    raise RuntimeError("X3 diag: setup anchor not found")
text = text.replace(setup_anchor, diag_code, 1)

storage_anchor = '''  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }
'''
storage_repl = storage_anchor + r'''
#ifdef X3_DISPLAY_DIAG
  runX3DisplayDiagnostic();
  return;
#endif
'''
if storage_anchor not in text:
    raise RuntimeError("X3 diag: Storage.begin anchor not found")
text = text.replace(storage_anchor, storage_repl, 1)

loop_anchor = "void loop() {\n"
loop_repl = r'''void loop() {
#ifdef X3_DISPLAY_DIAG
  if (x3DisplayDiagHalt) {
    delay(1000);
    return;
  }
#endif
'''
if loop_anchor not in text:
    raise RuntimeError("X3 diag: loop anchor not found")
text = text.replace(loop_anchor, loop_repl, 1)

main.write_text(text, encoding="utf-8")
print("Applied X3 SD display diagnostic patch")
