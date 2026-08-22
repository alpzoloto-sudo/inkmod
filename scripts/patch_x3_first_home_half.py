#!/usr/bin/env python3
from pathlib import Path

ROOT = Path.cwd().resolve()
HOME = ROOT / "src/activities/home/HomeActivity.cpp"

text = HOME.read_text(encoding="utf-8")

if '#include <HalGPIO.h>\n' not in text:
    text = text.replace('#include <HalStorage.h>\n', '#include <HalStorage.h>\n#include <HalGPIO.h>\n', 1)

# Rewrite the four Home paint calls first, before inserting the helper, so the
# helper's own normal FAST call cannot be rewritten recursively.
if 'displayHomeFrame(renderer, firstRenderDone);' not in text:
    count = text.count('renderer.displayBuffer();')
    if count != 4:
        raise SystemExit(f'Expected exactly 4 Home displayBuffer() calls, found {count}')
    text = text.replace('renderer.displayBuffer();', 'displayHomeFrame(renderer, firstRenderDone);')

marker = 'namespace {\nconstexpr uint32_t CAROUSEL_CACHE_MAGIC'
helper = '''namespace {\nvoid displayHomeFrame(const GfxRenderer& renderer, bool firstRenderDone) {\n  // Diagnostic only: X3 gets one strong first Home paint after the splash.\n  // Every later Home paint, and every X4 paint, remains FAST as before.\n  if (gpio.deviceIsX3() && !firstRenderDone) {\n    renderer.displayBuffer(HalDisplay::HALF_REFRESH);\n  } else {\n    renderer.displayBuffer();\n  }\n}\n\nconstexpr uint32_t CAROUSEL_CACHE_MAGIC'''
if 'void displayHomeFrame(' not in text:
    if marker not in text:
        raise SystemExit('HomeActivity namespace marker not found')
    text = text.replace(marker, helper, 1)

HOME.write_text(text, encoding='utf-8')
print('X3 first Home HALF diagnostic applied')
