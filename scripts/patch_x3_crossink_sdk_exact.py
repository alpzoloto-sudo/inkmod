#!/usr/bin/env python3
from pathlib import Path

if "__file__" in globals():
    ROOT = Path(__file__).resolve().parents[1]
else:
    ROOT = Path.cwd().resolve()

XTEINK = ROOT / "freeink-sdk/libs/hardware/XteinkDetect/src/XteinkDetect.cpp"
UC8253 = ROOT / "freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8253X3Driver.cpp"
HAL_GPIO = ROOT / "lib/hal/HalGPIO.cpp"


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count == 1:
        path.write_text(text.replace(old, new, 1), encoding="utf-8")
        return
    if count == 0 and new in text:
        return
    raise SystemExit(f"{path}: expected exactly one target, found {count}")


# CrossInk pins FreeInk SDK commit 1ff020263cd2202ea79ce3eb811f5ac8489b8cde.
# Restore the X3-relevant behavior from that exact SDK commit without changing
# inkMOD's X4-specific SSD1677/QY work.
text = XTEINK.read_text(encoding="utf-8")
text = text.replace("#include <driver/gpio.h>\n", "")
text = text.replace(
    "    // Deep-sleep GPIO hold can survive wake and keep EPD RESET latched.\n"
    "    // Release it before probing the physical display controller.\n"
    "    gpio_hold_dis(static_cast<gpio_num_t>(p.rst));\n\n",
    "",
)
XTEINK.write_text(text, encoding="utf-8")

replace_once(
    UC8253,
    "  _initialFullSyncsRemaining = 2;\n",
    "  // CrossInk/FreeInk SDK 1ff020: only one forced clean after begin().\n"
    "  _initialFullSyncsRemaining = 1;\n",
)

# Invalidate only inkMOD's old X3 panel-controller cache. Earlier experimental
# builds could have stored a wrong UC8253/UC8279 result in cphw/epd_det; CrossInk
# trusts a valid cache and would therefore never execute the corrected live probe.
# A new key forces one fresh hardware probe, then caches that result normally.
replace_once(
    HAL_GPIO,
    'constexpr char NVS_KEY_EPD_CACHED[] = "epd_det";    // 0=unknown, 1=uc8253, 2=uc8279\n',
    'constexpr char NVS_KEY_EPD_CACHED[] = "epd_det2";   // v2: fresh CrossInk probe cache\n',
)

print("CrossInk exact X3 SDK behavior applied with fresh EPD cache")
