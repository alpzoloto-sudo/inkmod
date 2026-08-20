#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HAL_DISPLAY_CPP = ROOT / "lib/hal/HalDisplay.cpp"

text = HAL_DISPLAY_CPP.read_text(encoding="utf-8")

# The 1.1.5 release temporarily injected the live UC81xx bus probe from
# patch_feedback_fixes.py. Field reports show devices can fail to come back
# after an SD update, leaving the retained E-Ink SUCCESS frame visible.
# Keep the proven X3/X4 family detection, but restore the conservative factory
# NVS screenType selection for the panel controller until the live probe has
# been validated across production revisions.
text = text.replace("#include <XteinkDetect.h>\n", "")

live = '''  // Live display-bus fingerprint is the ground truth for panel revision.\n  // It promotes stock X4 SSD1677 -> UC8179/UC8279 or X3 UC8253 -> UC8279\n  // only after two-pass confirmation; otherwise the stock profile remains.\n  // Do not let OEM NVS override this decision: full-chip images can carry\n  // calibration from another unit, while the live controller is authoritative.\n  freeink::applyXteinkDisplayController();\n\n  einkDisplay.begin();\n'''

safe = '''  // Safe revision selection from factory calibration only. Unknown/missing data\n  // leaves the stock controller untouched and boot continues normally.\n  applyOemNvsDisplayController(isX3);\n\n  einkDisplay.begin();\n'''

if live in text:
    text = text.replace(live, safe, 1)
elif safe not in text:
    raise SystemExit("HalDisplay.cpp: expected live-probe or safe NVS block not found")

HAL_DISPLAY_CPP.write_text(text, encoding="utf-8")
print("Live display probe disabled; factory NVS panel selection restored")
