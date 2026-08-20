#!/usr/bin/env python3
from pathlib import Path
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
HAL_DISPLAY_CPP = ROOT / "lib/hal/HalDisplay.cpp"

# Pin the Xteink panel-driver set to the known-good FreeInk SDK snapshot supplied
# for the 1.1.5 hotfix. Do not mix our locally modified drivers with this set.
FREEINK_COMMIT = "ffeaaa271231d865590f8c54ea45ec02b1342d4e"
RAW_BASE = f"https://raw.githubusercontent.com/Free-Ink/freeink-sdk/{FREEINK_COMMIT}/libs/display/FreeInkDisplay/src/driver"
DRIVER_DIR = ROOT / "freeink-sdk/libs/display/FreeInkDisplay/src/driver"
PINNED_FILES = (
    "PanelDriver.h",
    "Ssd1677Driver.cpp",
    "Ssd1677Driver.h",
    "Uc8179Driver.cpp",
    "Uc8179Driver.h",
    "Uc8253X3Driver.cpp",
    "Uc8253X3Driver.h",
    "Uc8279Driver.cpp",
    "Uc8279Driver.h",
    "Uc8279X4Driver.cpp",
    "Uc8279X4Driver.h",
)


def fetch_text(url: str) -> str:
    req = Request(url, headers={"User-Agent": "inkMOD-build/1.1.5"})
    with urlopen(req, timeout=30) as response:
        data = response.read()
    text = data.decode("utf-8")
    if len(text) < 128:
        raise SystemExit(f"Pinned FreeInk driver download looks truncated: {url}")
    return text


for filename in PINNED_FILES:
    target = DRIVER_DIR / filename
    target.write_text(fetch_text(f"{RAW_BASE}/{filename}"), encoding="utf-8")
print(f"Pinned Xteink display drivers to FreeInk SDK {FREEINK_COMMIT}")

# The 1.1.5 release temporarily injected a live UC81xx bus probe from
# patch_feedback_fixes.py. Field reports show devices can fail to come back
# after an SD update, leaving the retained E-Ink SUCCESS frame visible.
# Keep X3/X4 family detection, but restore the conservative factory NVS
# screenType selection for the panel controller. The pinned SDK supplies the
# actual controller drivers; inkMOD no longer tries to outsmart them at boot.
text = HAL_DISPLAY_CPP.read_text(encoding="utf-8")
text = text.replace("#include <XteinkDetect.h>\n", "")

live = '''  // Live display-bus fingerprint is the ground truth for panel revision.\n  // It promotes stock X4 SSD1677 -> UC8179/UC8279 or X3 UC8253 -> UC8279\n  // only after two-pass confirmation; otherwise the stock profile remains.\n  // Do not let OEM NVS override this decision: full-chip images can carry\n  // calibration from another unit, while the live controller is authoritative.\n  freeink::applyXteinkDisplayController();\n\n  einkDisplay.begin();\n'''

safe = '''  // Safe revision selection from factory calibration only. Unknown/missing data\n  // leaves the stock controller untouched and boot continues normally.\n  applyOemNvsDisplayController(isX3);\n\n  einkDisplay.begin();\n'''

if live in text:
    text = text.replace(live, safe, 1)
elif safe not in text:
    raise SystemExit("HalDisplay.cpp: expected live-probe or safe NVS block not found")

HAL_DISPLAY_CPP.write_text(text, encoding="utf-8")
print("Live display probe disabled; factory NVS panel selection restored")
