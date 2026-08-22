#!/usr/bin/env python3
from pathlib import Path

ROOT = Path.cwd().resolve()
GPIO = ROOT / "lib/hal/HalGPIO.cpp"
DISPLAY = ROOT / "lib/hal/HalDisplay.cpp"


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one patch target, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    GPIO,
    '''  const bool x3IsUc8279 = deviceIsX3() && detectX3DisplayIsUc8279();
  BoardConfig::selectDevice(!deviceIsX3() ? BoardConfig::Board::XteinkX4
                            : x3IsUc8279  ? BoardConfig::Board::XteinkX3Uc8279
                                          : BoardConfig::Board::XteinkX3);

  // Match CrossInk ordering exactly for X4: select the X4 profile first, let the
  // SDK resolve its controller while the EPD pins are still free, then attach SPI.
  if (deviceIsX4()) {
    freeink::applyXteinkDisplayController();
  }
''',
    '''  if (deviceIsX3()) {
    // Last known-working inkMOD X3 path: do not probe the EPD bus here.
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3);
  } else {
    // Keep current X4 live autodetection unchanged.
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
    freeink::applyXteinkDisplayController();
  }
''',
)

text = DISPLAY.read_text(encoding="utf-8")
if '#include "nvs.h"' not in text:
    text = text.replace('#include "HalSpiBus.h"\n', '#include "HalSpiBus.h"\n#include "nvs.h"\n', 1)

if 'bool applyLegacyX3FactoryController()' not in text:
    helper = '''namespace {\n\n// Restore the pre-live-probe X3 controller selection used by working inkMOD.
bool applyLegacyX3FactoryController() {
  nvs_handle_t h;
  if (nvs_open("hw_calib", NVS_READONLY, &h) != ESP_OK) return false;
  uint8_t screenType = 0;
  const esp_err_t err = nvs_get_u8(h, "screenType", &screenType);
  nvs_close(h);
  if (err != ESP_OK) return false;

  if (screenType == 2 || screenType == 0x0C) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
    return true;
  }

  BoardConfig::selectDevice(BoardConfig::Board::XteinkX3);
  return false;
}

'''
    if 'namespace {\n\n' not in text:
        raise SystemExit("HalDisplay.cpp: namespace anchor not found")
    text = text.replace('namespace {\n\n', helper, 1)

old = '''  if (isX3) {
    einkDisplay.setDisplayX3();
  } else {
'''
new = '''  if (isX3) {
    einkDisplay.setDisplayX3();
    applyLegacyX3FactoryController();
  } else {
'''
if new not in text:
    if text.count(old) != 1:
        raise SystemExit(f"HalDisplay.cpp: expected one X3 begin target, found {text.count(old)}")
    text = text.replace(old, new, 1)

DISPLAY.write_text(text, encoding="utf-8")
print("Restored pre-autodetect X3 display selection; X4 unchanged")
