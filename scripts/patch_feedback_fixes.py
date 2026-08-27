#!/usr/bin/env python3
from pathlib import Path

if "__file__" in globals():
    ROOT = Path(__file__).resolve().parents[1]
else:
    ROOT = Path.cwd().resolve()

CLEAR_CACHE_CPP = ROOT / "src/activities/settings/ClearCacheActivity.cpp"
HOME_CPP = ROOT / "src/activities/home/HomeActivity.cpp"
SLEEP_CPP = ROOT / "src/activities/boot_sleep/SleepActivity.cpp"
LYRA_CAROUSEL_CPP = ROOT / "src/components/themes/lyra/LyraCarouselTheme.cpp"
MAIN_CPP = ROOT / "src/main.cpp"
CHAPTER_HTML_CPP = ROOT / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp"


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count == 1:
        path.write_text(text.replace(old, new, 1), encoding="utf-8")
        return
    if count == 0 and new in text:
        return
    raise SystemExit(f"{path}: expected exactly one patch target, found {count}")


def has_marker(path: Path, marker: str) -> bool:
    return marker in path.read_text(encoding="utf-8")


# "Clear reading cache" must only remove derived/cache state. User
# configuration must survive cache clearing. Older trees may have only the
# original wifi/settings pair; newer trees may already preserve KOReader and
# OPDS credentials. Treat either fully-patched form as success so this script
# is safe to run repeatedly (PlatformIO builds multiple environments in the
# same checkout).
clear_cache_text = CLEAR_CACHE_CPP.read_text(encoding="utf-8")
required_cache_preserve_markers = (
    'std::strcmp(name, "wifi.json") == 0',
    'std::strcmp(name, "inkmod-settings.json") == 0',
    'std::strcmp(name, "koreader.json") == 0',
    'std::strcmp(name, "koreader.bin") == 0',
    'std::strcmp(name, "koreader.bin.bak") == 0',
    'std::strcmp(name, "opds.json") == 0',
)
if not all(marker in clear_cache_text for marker in required_cache_preserve_markers):
    old_preserve = '    const bool preserve = std::strcmp(name, "wifi.json") == 0 || std::strcmp(name, "inkmod-settings.json") == 0;\n'
    new_preserve = '''    const bool preserve = std::strcmp(name, "wifi.json") == 0 ||
                          std::strcmp(name, "inkmod-settings.json") == 0 ||
                          std::strcmp(name, "koreader.json") == 0 ||
                          std::strcmp(name, "koreader.bin") == 0 ||
                          std::strcmp(name, "koreader.bin.bak") == 0 ||
                          std::strcmp(name, "opds.json") == 0;
'''
    if old_preserve not in clear_cache_text:
        raise SystemExit(
            f"{CLEAR_CACHE_CPP}: cache-preserve block is neither legacy nor fully patched; refusing ambiguous patch"
        )
    CLEAR_CACHE_CPP.write_text(clear_cache_text.replace(old_preserve, new_preserve, 1), encoding="utf-8")

# Minimal has its own home-menu builder. Keep Search visibility consistent
# with every other theme and with the existing showHomeSearch setting.
replace_once(
    HOME_CPP,
    '''HomeMenuEntries buildMinimalMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks) {\n  HomeMenuEntries items;\n  items.push({tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks});\n\n  if (hasOpdsServers) {\n''',
    '''HomeMenuEntries buildMinimalMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks) {\n  HomeMenuEntries items;\n  items.push({tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks});\n  if (SETTINGS.showHomeSearch) {\n    items.push({tr(STR_SEARCH_FILES), Search, HomeMenuAction::SearchFiles});\n  }\n\n  if (hasOpdsServers) {\n''',
)

# COVER_CUSTOM means "use the current book cover when one is available,
# otherwise fall back to the user's custom sleep image". renderCoverSleepScreen
# already implements that fallback. Relying on lastSleepFromReader made the
# combined mode show Custom even when a valid current-book cover existed.
replace_once(
    SLEEP_CPP,
    '''    case (InkMODSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):\n      if (APP_STATE.lastSleepFromReader) {\n        return renderCoverSleepScreen();\n      } else {\n        return renderCustomSleepScreen();\n      }\n''',
    '''    case (InkMODSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):\n      return renderCoverSleepScreen();\n''',
)

# Lyra Carousel draws the reading percentage at the exact right edge of its
# footer progress bar. Leave a small E-Ink safe inset so the '%' glyph cannot
# overhang/clamp at the right edge. Guard the compound patch because CI builds
# developer and release sequentially in the same checkout.
if not has_marker(LYRA_CAROUSEL_CPP, "kFooterPercentRightInset"):
    replace_once(
        LYRA_CAROUSEL_CPP,
        '''constexpr int kFooterPercentTopGap = 2;\n''',
        '''constexpr int kFooterPercentTopGap = 2;\nconstexpr int kFooterPercentRightInset = 6;\n''',
    )
    replace_once(
        LYRA_CAROUSEL_CPP,
        '''      const int progressLabelW = renderer.getTextWidth(footerLabelFontId, progressLabel, EpdFontFamily::REGULAR);\n      const int progressLabelY = progressBarY + kFooterProgressBarHeight + kFooterPercentTopGap;\n      renderer.drawText(footerLabelFontId, footerX + footerWidth - progressLabelW, progressLabelY, progressLabel, true,\n                        EpdFontFamily::REGULAR);\n''',
        '''      const int progressLabelW = renderer.getTextWidth(footerLabelFontId, progressLabel, EpdFontFamily::REGULAR);\n      const int progressLabelY = progressBarY + kFooterProgressBarHeight + kFooterPercentTopGap;\n      const int progressLabelX =\n          std::max(footerX, footerX + footerWidth - progressLabelW - kFooterPercentRightInset);\n      renderer.drawText(footerLabelFontId, progressLabelX, progressLabelY, progressLabel, true,\n                        EpdFontFamily::REGULAR);\n''',
    )

# Display-controller autodetection now lives directly in lib/hal/HalDisplay.cpp.
# Do not mutate display selection from a build-time feedback patch: the source
# tree itself must show the same live-probe logic that ships in the firmware.

# Emergency recovery intentionally reuses the normal SD firmware flasher so it
# gets the exact same ESP image validation, partition selection and write path as
# the interactive updater. It runs immediately after Storage.begin(), before any
# display/UI setup, so a device with a non-working panel can still be recovered.
if not has_marker(MAIN_CPP, '"network/FirmwareFlasher.h"'):
    replace_once(
        MAIN_CPP,
        '''#include "activities/settings/SdFirmwareUpdateActivity.h"\n#include "components/UITheme.h"\n''',
        '''#include "activities/settings/SdFirmwareUpdateActivity.h"\n#ifndef SIMULATOR\n#include "network/FirmwareFlasher.h"\n#endif\n#include "components/UITheme.h"\n''',
    )

if not has_marker(MAIN_CPP, "EMERGENCY_FIRMWARE_PATH"):
    replace_once(
        MAIN_CPP,
        '''void waitForPowerRelease() {\n''',
        '''static constexpr char EMERGENCY_FIRMWARE_PATH[] = "/inkmod-recovery.bin";\nstatic constexpr char EMERGENCY_FIRMWARE_APPLIED_PATH[] = "/inkmod-recovery.applied.bin";\nstatic constexpr char EMERGENCY_FIRMWARE_FAILED_PATH[] = "/inkmod-recovery.failed.bin";\n\nstatic void tryEmergencyFirmwareUpdate() {\n#ifndef SIMULATOR\n  if (!Storage.exists(EMERGENCY_FIRMWARE_PATH)) {\n    return;\n  }\n\n  LOG_INF("FW", "Emergency SD firmware detected: %s", EMERGENCY_FIRMWARE_PATH);\n  const auto result = firmware_flash::flashFromSdPath(EMERGENCY_FIRMWARE_PATH, nullptr, nullptr);\n  if (result != firmware_flash::Result::OK) {\n    LOG_ERR("FW", "Emergency SD firmware failed: %s", firmware_flash::resultName(result));\n    Storage.remove(EMERGENCY_FIRMWARE_FAILED_PATH);\n    if (!Storage.rename(EMERGENCY_FIRMWARE_PATH, EMERGENCY_FIRMWARE_FAILED_PATH)) {\n      Storage.remove(EMERGENCY_FIRMWARE_PATH);\n    }\n    return;\n  }\n\n  Storage.remove(EMERGENCY_FIRMWARE_APPLIED_PATH);\n  bool disarmed = Storage.rename(EMERGENCY_FIRMWARE_PATH, EMERGENCY_FIRMWARE_APPLIED_PATH);\n  if (!disarmed) {\n    disarmed = Storage.remove(EMERGENCY_FIRMWARE_PATH);\n  }\n  if (!disarmed) {\n    LOG_ERR("FW", "Emergency firmware applied but trigger could not be disarmed; refusing auto-restart");\n    return;\n  }\n\n  LOG_INF("FW", "Emergency SD firmware applied; restarting");\n  delay(500);\n  ESP.restart();\n#endif\n}\n\nvoid waitForPowerRelease() {\n''',
    )

if not has_marker(MAIN_CPP, "Headless emergency recovery"):
    replace_once(
        MAIN_CPP,
        '''  if (!Storage.begin()) {\n    LOG_ERR("MAIN", "SD card initialization failed");\n    setupDisplayAndFonts(isSilentReboot);\n    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);\n    return;\n  }\n''',
        '''  if (!Storage.begin()) {\n    LOG_ERR("MAIN", "SD card initialization failed");\n    setupDisplayAndFonts(isSilentReboot);\n    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);\n    return;\n  }\n\n  // Headless emergency recovery: root-of-SD /inkmod-recovery.bin is validated\n  // and flashed before display/UI initialization.\n  tryEmergencyFirmwareUpdate();\n''',
    )

# Apply the universal inkMOD Classic Book profile after both embedded stylesheet
# rules and inline style="...". This gives the profile CSS-!important semantics
# without disabling author typography or increasing the runtime CSS rule table.
if not has_marker(CHAPTER_HTML_CPP, '"Epub/css/ClassicBookProfile.h"'):
    replace_once(
        CHAPTER_HTML_CPP,
        '''#include "Epub/htmlEntities.h"\n''',
        '''#include "Epub/htmlEntities.h"\n#include "Epub/css/ClassicBookProfile.h"\n''',
    )

if not has_marker(CHAPTER_HTML_CPP, "applyInkmodClassicBookProfile(name"):
    replace_once(
        CHAPTER_HTML_CPP,
        '''    if (!styleAttr.empty()) {\n      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);\n      cssStyle.applyOver(inlineStyle);\n    }\n    if (self->shouldAbortForLowMemory("CSS style resolution")) {\n''',
        '''    if (!styleAttr.empty()) {\n      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);\n      cssStyle.applyOver(inlineStyle);\n    }\n    applyInkmodClassicBookProfile(name, classAttr, self->ancestorStack_, cssStyle);\n    if (self->shouldAbortForLowMemory("CSS style resolution")) {\n''',
    )

print("Feedback fixes applied")
