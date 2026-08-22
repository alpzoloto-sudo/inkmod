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
    ('#include "ActivityManager.h"\n', '#include "ActivityManager.h"\n\n#include <Logging.h>\n'),
    ('    if (currentActivity) {\n      HalPowerManager::Lock powerLock;  // Ensure we don\'t go into low-power mode while rendering\n      currentActivity->render(std::move(lock));\n    }',
     '    if (currentActivity) {\n      LOG_INF("X3TRACE", "render ENTER activity=%s heap=%u max=%u", currentActivity->name.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());\n      HalPowerManager::Lock powerLock;  // Ensure we don\'t go into low-power mode while rendering\n      currentActivity->render(std::move(lock));\n      LOG_INF("X3TRACE", "render EXIT activity=%s heap=%u max=%u", currentActivity ? currentActivity->name.c_str() : "<null>", ESP.getFreeHeap(), ESP.getMaxAllocHeap());\n    }'),
    ('      lock.unlock();  // onEnter may acquire its own lock\n      currentActivity->onEnter();',
     '      lock.unlock();  // onEnter may acquire its own lock\n      LOG_INF("X3TRACE", "onEnter ENTER activity=%s", currentActivity ? currentActivity->name.c_str() : "<null>");\n      currentActivity->onEnter();\n      LOG_INF("X3TRACE", "onEnter EXIT activity=%s", currentActivity ? currentActivity->name.c_str() : "<null>");'),
    ('void ActivityManager::goHome(HomeMenuItem initialMenuItem) {',
     'void ActivityManager::goHome(HomeMenuItem initialMenuItem) {\n  LOG_INF("X3TRACE", "goHome ENTER initial=%d current=%s", static_cast<int>(initialMenuItem), currentActivity ? currentActivity->name.c_str() : "<null>");'),
    ('  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));\n}',
     '  LOG_INF("X3TRACE", "goHome before replace initial=%d", static_cast<int>(initialMenuItem));\n  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));\n  LOG_INF("X3TRACE", "goHome EXIT current=%s", currentActivity ? currentActivity->name.c_str() : "<null>");\n}')
])

patch_file("lib/GfxRenderer/GfxRenderer.cpp", [
    ('void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode, const bool turnOffScreen) const {\n  auto elapsed = millis() - start_ms;\n  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);\n  display.displayBuffer(refreshMode, fadingFix || turnOffScreen);\n}',
     'void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode, const bool turnOffScreen) const {\n  auto elapsed = millis() - start_ms;\n  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);\n  LOG_INF("X3TRACE", "GFX display ENTER mode=%d off=%d elapsed=%lu", static_cast<int>(refreshMode), static_cast<int>(fadingFix || turnOffScreen), elapsed);\n  display.displayBuffer(refreshMode, fadingFix || turnOffScreen);\n  LOG_INF("X3TRACE", "GFX display EXIT mode=%d", static_cast<int>(refreshMode));\n}')
])

patch_file("freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp", [
    ('#include <BoardConfig.h>\n', '#include <BoardConfig.h>\n#include <Logging.h>\n'),
    ('void FreeInkDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {',
     'void FreeInkDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {\n  LOG_INF("X3TRACE", "FREEINK display ENTER panel=%d mode=%d off=%d driver=%p", static_cast<int>(_panelSel), static_cast<int>(mode), static_cast<int>(turnOffScreen), static_cast<void*>(_driver));')
])
