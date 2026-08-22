Import("env")
from pathlib import Path


def patch_file(path, replacements):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    original = text
    for old, new in replacements:
        if old not in text:
            print(f"[x3-trace] pattern not found in {path}: {old[:80]!r}")
            env.Exit(1)
        text = text.replace(old, new, 1)
    if text != original:
        p.write_text(text, encoding="utf-8")
        print(f"[x3-trace] patched {path}")


patch_file("src/activities/ActivityManager.cpp", [
    ('#include "ActivityManager.h"\n', '#include "ActivityManager.h"\n\n#include <Arduino.h>\n'),
    ('    if (currentActivity) {\n      HalPowerManager::Lock powerLock;  // Ensure we don\'t go into low-power mode while rendering\n      currentActivity->render(std::move(lock));\n    }',
     '    if (currentActivity) {\n      Serial.printf("[X3TRACE] render ENTER activity=%s heap=%u max=%u\\n", currentActivity->name.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());\n      Serial.flush();\n      HalPowerManager::Lock powerLock;  // Ensure we don\'t go into low-power mode while rendering\n      currentActivity->render(std::move(lock));\n      Serial.printf("[X3TRACE] render EXIT activity=%s heap=%u max=%u\\n", currentActivity ? currentActivity->name.c_str() : "<null>", ESP.getFreeHeap(), ESP.getMaxAllocHeap());\n      Serial.flush();\n    }'),
    ('      lock.unlock();  // onEnter may acquire its own lock\n      currentActivity->onEnter();',
     '      lock.unlock();  // onEnter may acquire its own lock\n      Serial.printf("[X3TRACE] onEnter ENTER activity=%s\\n", currentActivity ? currentActivity->name.c_str() : "<null>");\n      Serial.flush();\n      currentActivity->onEnter();\n      Serial.printf("[X3TRACE] onEnter EXIT activity=%s\\n", currentActivity ? currentActivity->name.c_str() : "<null>");\n      Serial.flush();'),
    ('void ActivityManager::goHome(HomeMenuItem initialMenuItem) {',
     'void ActivityManager::goHome(HomeMenuItem initialMenuItem) {\n  Serial.printf("[X3TRACE] goHome ENTER initial=%d current=%s\\n", static_cast<int>(initialMenuItem), currentActivity ? currentActivity->name.c_str() : "<null>");\n  Serial.flush();'),
    ('  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));\n}',
     '  Serial.printf("[X3TRACE] goHome before replace initial=%d\\n", static_cast<int>(initialMenuItem));\n  Serial.flush();\n  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));\n  Serial.printf("[X3TRACE] goHome EXIT current=%s\\n", currentActivity ? currentActivity->name.c_str() : "<null>");\n  Serial.flush();\n}')
])

patch_file("lib/GfxRenderer/GfxRenderer.cpp", [
    ('void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode, const bool turnOffScreen) const {\n  auto elapsed = millis() - start_ms;\n  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);\n  display.displayBuffer(refreshMode, fadingFix || turnOffScreen);\n}',
     'void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode, const bool turnOffScreen) const {\n  auto elapsed = millis() - start_ms;\n  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);\n  Serial.printf("[X3TRACE] GFX display ENTER mode=%d off=%d elapsed=%lu\\n", static_cast<int>(refreshMode), static_cast<int>(fadingFix || turnOffScreen), elapsed);\n  Serial.flush();\n  display.displayBuffer(refreshMode, fadingFix || turnOffScreen);\n  Serial.printf("[X3TRACE] GFX display EXIT mode=%d\\n", static_cast<int>(refreshMode));\n  Serial.flush();\n}')
])

patch_file("freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp", [
    ('void FreeInkDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {',
     'void FreeInkDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {\n  Serial.printf("[X3TRACE] FREEINK display ENTER panel=%d mode=%d off=%d driver=%p\\n", static_cast<int>(_panelSel), static_cast<int>(mode), static_cast<int>(turnOffScreen), static_cast<void*>(_driver));\n  Serial.flush();')
])
