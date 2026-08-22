#!/usr/bin/env python3
from pathlib import Path

if "__file__" in globals():
    ROOT = Path(__file__).resolve().parents[1]
else:
    ROOT = Path.cwd().resolve()

MAIN = ROOT / "src/main.cpp"
text = MAIN.read_text(encoding="utf-8")

# Only instrument the first loop pass on real X3 hardware. showX3BootStage()
# is supplied by patch_x3_boot_stage_diag.py, which runs before this script.
replacements = [
    (
        "void loop() {\n  static unsigned long maxLoopDuration = 0;\n",
        "void loop() {\n"
        "  static bool x3FirstLoopDiag = true;\n"
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) showX3BootStage(\"X3-L1 LOOP ENTER\");\n"
        "  static unsigned long maxLoopDuration = 0;\n",
    ),
    (
        "  gpio.update();\n  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.tiltPageTurnDirection, SETTINGS.orientation,\n                       activityManager.isReaderActivity());\n\n  renderer.setFadingFix(SETTINGS.fadingFix);\n",
        "  gpio.update();\n"
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) showX3BootStage(\"X3-L2 GPIO OK\");\n"
        "  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.tiltPageTurnDirection, SETTINGS.orientation,\n"
        "                       activityManager.isReaderActivity());\n"
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) showX3BootStage(\"X3-L3 TILT OK\");\n\n"
        "  renderer.setFadingFix(SETTINGS.fadingFix);\n",
    ),
    (
        "  // Handle incoming serial commands,\n",
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) showX3BootStage(\"X3-L4 POWER OK\");\n\n"
        "  // Handle incoming serial commands,\n",
    ),
    (
        "  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();\n",
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) showX3BootStage(\"X3-L5 INPUT OK\");\n\n"
        "  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();\n",
    ),
    (
        "  // Refresh the battery icon when USB is plugged or unplugged.\n",
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) showX3BootStage(\"X3-L6 SLEEP GUARDS OK\");\n\n"
        "  // Refresh the battery icon when USB is plugged or unplugged.\n",
    ),
    (
        "  const unsigned long activityStartTime = millis();\n  activityManager.loop();\n  const unsigned long activityDuration = millis() - activityStartTime;\n",
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) showX3BootStage(\"X3-L7 BEFORE ACTIVITY\");\n"
        "  const unsigned long activityStartTime = millis();\n"
        "  activityManager.loop();\n"
        "  const unsigned long activityDuration = millis() - activityStartTime;\n"
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) showX3BootStage(\"X3-L8 ACTIVITY OK\");\n",
    ),
    (
        "  if (activityManager.skipLoopDelay()) {\n",
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) showX3BootStage(\"X3-L9 BEFORE IDLE\");\n"
        "  if (activityManager.skipLoopDelay()) {\n",
    ),
    (
        "      delay(10);\n    }\n  }\n}\n",
        "      delay(10);\n"
        "    }\n"
        "  }\n"
        "  if (x3FirstLoopDiag && gpio.deviceIsX3()) {\n"
        "    x3FirstLoopDiag = false;\n"
        "    showX3BootStage(\"X3-L10 LOOP DONE\");\n"
        "  }\n"
        "}\n",
    ),
]

for old, new in replacements:
    if new in text:
        continue
    if old not in text:
        raise SystemExit("main.cpp: loop diagnostic anchor not found")
    text = text.replace(old, new, 1)

MAIN.write_text(text, encoding="utf-8")
print("X3 first-loop diagnostics applied")
