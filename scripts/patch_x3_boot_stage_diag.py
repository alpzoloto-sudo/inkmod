#!/usr/bin/env python3
from pathlib import Path

if "__file__" in globals():
    ROOT = Path(__file__).resolve().parents[1]
else:
    ROOT = Path.cwd().resolve()

MAIN = ROOT / "src/main.cpp"
text = MAIN.read_text(encoding="utf-8")

marker = "void showX3BootStage(const char* stage)"
if marker not in text:
    anchor = "// Devices without a battery-backed RTC (X4) lose track of time on every real power\n"
    helper = r'''// Locked-X3 hardware diagnostic: paint a persistent boot-stage breadcrumb on
// the E-Ink panel. If the next operation hangs, this is the last stage visible.
// X4 is deliberately untouched.
void showX3BootStage(const char* stage) {
#ifndef SIMULATOR
  if (!gpio.deviceIsX3()) return;
  renderer.clearScreen();
  renderer.drawCenteredText(UI_14_FONT_ID, renderer.getScreenHeight() / 2 - 16, stage, true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 22, "X3 BOOT DIAG", true,
                            EpdFontFamily::REGULAR);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  delay(120);
#else
  (void)stage;
#endif
}

'''
    if anchor not in text:
        raise SystemExit("main.cpp: helper insertion anchor not found")
    text = text.replace(anchor, helper + anchor, 1)

replacements = [
    (
        "  // Placed after the splash/quick-resume screen is already painted, so a cold boot's\n",
        "  showX3BootStage(\"X3-D1 AFTER SPLASH\");\n\n"
        "  // Placed after the splash/quick-resume screen is already painted, so a cold boot's\n",
    ),
    (
        "  attemptSilentBootTimeSync(bootTimeSyncCandidate);\n\n  if (recoveryFirmwareMode) {\n",
        "  attemptSilentBootTimeSync(bootTimeSyncCandidate);\n"
        "  showX3BootStage(\"X3-D2 AFTER CLOCK\");\n\n"
        "  showX3BootStage(\"X3-D3 BEFORE ROUTE\");\n"
        "  if (recoveryFirmwareMode) {\n",
    ),
    (
        "  if (resume == BootResume::Silent) {\n    // Block until the first paint physically completes. refreshDisplay()\n",
        "  showX3BootStage(\"X3-D4 AFTER ROUTE\");\n\n"
        "  if (resume == BootResume::Silent) {\n"
        "    // Block until the first paint physically completes. refreshDisplay()\n",
    ),
    (
        "  // Ensure we're not still holding the power button before leaving setup\n  waitForPowerRelease();\n  allowSleepAt = millis() + 2000;\n",
        "  showX3BootStage(\"X3-D5 BEFORE PWR RELEASE\");\n"
        "  // Ensure we're not still holding the power button before leaving setup\n"
        "  waitForPowerRelease();\n"
        "  showX3BootStage(\"X3-D6 SETUP DONE\");\n"
        "  allowSleepAt = millis() + 2000;\n",
    ),
]

for old, new in replacements:
    if new in text:
        continue
    if old not in text:
        raise SystemExit("main.cpp: diagnostic patch anchor not found")
    text = text.replace(old, new, 1)

MAIN.write_text(text, encoding="utf-8")
print("X3 on-screen boot diagnostics applied")
