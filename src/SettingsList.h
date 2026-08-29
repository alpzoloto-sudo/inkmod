#pragma once

#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>
#include <ctime>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "InkMODSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

inline std::string fontSizePointLabel(const uint8_t pointSize) { return std::to_string(pointSize) + " pt"; }

inline void appendBuiltinFontSizeOption(SettingInfo& setting, const InkMODSettings::FONT_SIZE size) {
  const uint8_t stored = InkMODSettings::getStoredReaderFontSize(size);
  if (stored == UINT8_MAX) return;

  setting.enumStringValues.push_back(fontSizePointLabel(InkMODSettings::getReaderFontPointSize(size)));
  setting.enumRawValues.push_back(stored);
}

inline SettingInfo buildBuiltinFontSizeSetting() {
  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.valuePtr = &InkMODSettings::fontSize;
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;
  s.enumStringValues.reserve(InkMODSettings::FONT_SIZE_COUNT);
  s.enumRawValues.reserve(InkMODSettings::FONT_SIZE_COUNT);

  appendBuiltinFontSizeOption(s, InkMODSettings::TEENSY);
  appendBuiltinFontSizeOption(s, InkMODSettings::ITTY_BITTY);
  appendBuiltinFontSizeOption(s, InkMODSettings::TINY);
  appendBuiltinFontSizeOption(s, InkMODSettings::SMALL);
  appendBuiltinFontSizeOption(s, InkMODSettings::MEDIUM);
  appendBuiltinFontSizeOption(s, InkMODSettings::LARGE);
  appendBuiltinFontSizeOption(s, InkMODSettings::EXTRA_LARGE);
  appendBuiltinFontSizeOption(s, InkMODSettings::HUGE_SIZE);

  return s;
}

inline SettingInfo buildSdFontSizeSetting(const SdCardFontFamilyInfo& family) {
  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.valuePtr = &InkMODSettings::fontSize;
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;

  const std::vector<uint8_t> sizes = family.availableSizes();
  s.enumStringValues.reserve(sizes.size());
  s.enumRawValues.reserve(sizes.size());
  for (size_t i = 0; i < sizes.size(); i++) {
    s.enumStringValues.push_back(fontSizePointLabel(sizes[i]));
    s.enumRawValues.push_back(static_cast<uint8_t>(i));
  }
  return s;
}

inline void insertEnumOptionAfter(SettingInfo& setting, const StrId after, const StrId option, const uint8_t rawValue) {
  const auto it = std::find(setting.enumValues.begin(), setting.enumValues.end(), after);
  if (it == setting.enumValues.end()) {
    setting.enumValues.push_back(option);
    if (!setting.enumRawValues.empty()) setting.enumRawValues.push_back(rawValue);
    return;
  }

  const auto insertIndex = static_cast<size_t>(std::distance(setting.enumValues.begin(), it) + 1);
  setting.enumValues.insert(it + 1, option);
  if (!setting.enumRawValues.empty()) {
    setting.enumRawValues.insert(setting.enumRawValues.begin() + insertIndex, rawValue);
  }
}

inline void removeEnumRawValue(SettingInfo& setting, const uint8_t rawValue) {
  const auto it = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), rawValue);
  if (it == setting.enumRawValues.end()) {
    return;
  }

  const size_t index = static_cast<size_t>(std::distance(setting.enumRawValues.begin(), it));
  setting.enumRawValues.erase(it);
  if (index < setting.enumValues.size()) {
    setting.enumValues.erase(setting.enumValues.begin() + index);
  }
}

inline SettingInfo buildFontSizeSetting(const SdCardFontRegistry* registry) {
  if (SETTINGS.sdFontFamilyName[0] == '\0' && SETTINGS.fontFamily == InkMODSettings::TEST_FONTS) {
    SettingInfo s;
    s.nameId = StrId::STR_FONT_SIZE;
    s.type = SettingType::ENUM;
    s.valuePtr = &InkMODSettings::fontSize;
    s.key = "fontSize";
    s.category = StrId::STR_CAT_READER;
    s.enumStringValues.push_back("12 pt");
    const uint8_t stored = InkMODSettings::getStoredReaderFontSize(InkMODSettings::SMALL);
    s.enumRawValues.push_back(stored == UINT8_MAX ? 0 : stored);
    return s;
  }

  if (registry && SETTINGS.sdFontFamilyName[0] != '\0') {
    const SdCardFontFamilyInfo* family = registry->findFamily(SETTINGS.sdFontFamilyName);
    if (family && !family->files.empty()) {
      SettingInfo sdSizeSetting = buildSdFontSizeSetting(*family);
      // Normally this always has entries (one per installed file), but if the
      // point size couldn't be parsed from any filename in this family, the
      // list stays empty. Rendering that as-is shows a blank line the user
      // can't act on, so fall back to the built-in size list instead — this
      // is also what previously made the row look "stuck" until the family
      // was re-picked in FontSelectionActivity (which rewrites
      // sdFontFamilyName from the registry and recomputes fontSize).
      if (!sdSizeSetting.enumStringValues.empty()) {
        return sdSizeSetting;
      }
    }
  }
  return buildBuiltinFontSizeSetting();
}

inline uint8_t closestPointSizeIndex(const std::vector<uint8_t>& sizes, const uint8_t targetPointSize) {
  if (sizes.empty()) return 0;

  uint8_t bestIndex = 0;
  uint8_t bestDiff = UINT8_MAX;
  for (size_t i = 0; i < sizes.size(); i++) {
    const uint8_t size = sizes[i];
    const uint8_t diff = size > targetPointSize ? size - targetPointSize : targetPointSize - size;
    if (diff < bestDiff || (diff == bestDiff && size < sizes[bestIndex])) {
      bestIndex = static_cast<uint8_t>(i);
      bestDiff = diff;
    }
  }
  return bestIndex;
}

inline uint8_t closestBuiltinFontSizeIndex(const uint8_t targetPointSize) {
  uint8_t bestStored = 0;
  uint8_t bestPointSize = 0;
  uint8_t bestDiff = UINT8_MAX;

  for (uint8_t i = 0; i < InkMODSettings::FONT_SIZE_COUNT; i++) {
    const auto size = static_cast<InkMODSettings::FONT_SIZE>(i);
    const uint8_t stored = InkMODSettings::getStoredReaderFontSize(size);
    if (stored == UINT8_MAX) continue;

    const uint8_t pointSize = InkMODSettings::getReaderFontPointSize(size);
    const uint8_t diff = pointSize > targetPointSize ? pointSize - targetPointSize : targetPointSize - pointSize;
    if (diff < bestDiff || (diff == bestDiff && pointSize < bestPointSize)) {
      bestStored = stored;
      bestPointSize = pointSize;
      bestDiff = diff;
    }
  }
  return bestStored;
}

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId)
  std::vector<StrId> enumValues;
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first InkMODSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  enumStringValues.push_back("DejaVu Sans");
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size() + 1);
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }


  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(enumStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  std::vector<std::vector<uint8_t>> sdFamilySizes;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    sdFamilySizes.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilySizes),
                   [](const SdCardFontFamilyInfo& f) { return f.availableSizes(); });
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    if (SETTINGS.sdFontFamilyName[0] == '\0' && SETTINGS.fontFamily == InkMODSettings::TEST_FONTS) return 0;
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) return static_cast<uint8_t>(i + 1);
      }
    }
    return 0;
  };

  s.valueSetter = [sdFamilyNames, sdFamilySizes](uint8_t v) {
    uint8_t targetPointSize =
        (SETTINGS.sdFontFamilyName[0] == '\0' && SETTINGS.fontFamily == InkMODSettings::TEST_FONTS)
            ? 12
            : InkMODSettings::getReaderFontPointSize(SETTINGS.getEffectiveReaderFontSize());
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (size_t i = 0; i < sdFamilyNames.size(); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName && SETTINGS.fontSize < sdFamilySizes[i].size()) {
          targetPointSize = sdFamilySizes[i][SETTINGS.fontSize];
          break;
        }
      }
    }

    if (v == 0) {
      SETTINGS.fontFamily = InkMODSettings::TEST_FONTS;
      const uint8_t stored = InkMODSettings::getStoredReaderFontSize(InkMODSettings::SMALL);
      SETTINGS.fontSize = stored == UINT8_MAX ? 0 : stored;
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }

    const size_t sdIndex = static_cast<size_t>(v - 1);
    if (sdIndex < sdFamilyNames.size()) {
      SETTINGS.fontSize = closestPointSizeIndex(sdFamilySizes[sdIndex], targetPointSize);
      strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIndex].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
    }
  };

  return s;
}

inline SettingInfo buildSleepScreenSetting() {
  SettingInfo s =
      SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &InkMODSettings::sleepScreen,
                        {StrId::STR_NONE_OPT, StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER,
                         StrId::STR_COVER_CUSTOM, StrId::STR_PAGE_OVERLAY, StrId::STR_READING_STATS,
                         StrId::STR_THEME_MINIMAL, StrId::STR_THEME_MINIMAL_STATS, StrId::STR_QUICK_RESUME,
                         StrId::STR_THEME_DASHBOARD, StrId::STR_THEME_CALENDAR, StrId::STR_THEME_CALENDAR_INVERTED,
                         StrId::STR_THEME_CALENDAR_LANDSCAPE, StrId::STR_THEME_CALENDAR_LANDSCAPE_INVERTED},
                        "sleepScreen", StrId::STR_CAT_DISPLAY);
  s.withEnumRawValues({
      static_cast<uint8_t>(InkMODSettings::BLANK),
      static_cast<uint8_t>(InkMODSettings::DARK),
      static_cast<uint8_t>(InkMODSettings::LIGHT),
      static_cast<uint8_t>(InkMODSettings::CUSTOM),
      static_cast<uint8_t>(InkMODSettings::COVER),
      static_cast<uint8_t>(InkMODSettings::COVER_CUSTOM),
      static_cast<uint8_t>(InkMODSettings::OVERLAY),
      static_cast<uint8_t>(InkMODSettings::READING_STATS_SLEEP),
      static_cast<uint8_t>(InkMODSettings::MINIMAL_SLEEP),
      static_cast<uint8_t>(InkMODSettings::MINIMAL_STATS_SLEEP),
      static_cast<uint8_t>(InkMODSettings::QUICK_RESUME),
      static_cast<uint8_t>(InkMODSettings::DASHBOARD_SLEEP),
      static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP),
      static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP_INVERTED),
      static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP_LANDSCAPE),
      static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP_LANDSCAPE_INVERTED),
  });
  return s;
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The static list is constructed exactly once (master's optimization, #1086 +
// #1636) so the per-entry SettingInfo cost is paid once. When an
// SdCardFontRegistry is supplied AND has SD card fonts installed, the
// font-family entry is replaced in a per-call copy with a registry-aware
// version. Callers without SD fonts pay only a vector copy.
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr) {
  static const std::vector<SettingInfo> baseList = [] {
    std::vector<SettingInfo> v;
    v.reserve(66);
    auto add = [&v](SettingInfo setting) { v.push_back(std::move(setting)); };

    // --- Display ---
    add(buildSleepScreenSetting());
    add(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &InkMODSettings::sleepScreenCoverMode,
                          {StrId::STR_FIT, StrId::STR_CROP, StrId::STR_BLACK_BACKGROUND}, "sleepScreenCoverMode",
                          StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &InkMODSettings::sleepScreenCoverFilter,
                          {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                          "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_TIMEOUT_SLEEP_SCREEN, &InkMODSettings::quickResumeSleepScreen,
                          {StrId::STR_TIMEOUT_SAME_AS_MAIN, StrId::STR_TIMEOUT_QUICK_RESUME,
                           StrId::STR_TIMEOUT_OVERLAY, StrId::STR_TIMEOUT_CUSTOM, StrId::STR_COVER},
                          "quickResumeSleepScreen", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &InkMODSettings::hideBatteryPercentage,
                          {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                          StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_HIDE_CLOCK, &InkMODSettings::hideClock,
                          {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideClock",
                          StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({InkMODSettings::HIDE_CLOCK_NEVER, InkMODSettings::HIDE_CLOCK_IN_READER,
                                InkMODSettings::HIDE_CLOCK_ALWAYS}));
    if (!gpio.deviceIsX3()) {
      // X4 has no battery-backed RTC, so it re-syncs time from WiFi/NTP on
      // every boot (see checkSilentBootTimeSyncCandidate() in main.cpp) and on
      // every WiFi connection. This lets X4 users opt out of the clock
      // entirely - no display, no boot-time WiFi join, no Calendar sleep
      // screen. X3 has a real RTC and doesn't need this, so it's not shown there.
      add(SettingInfo::Toggle(StrId::STR_CLOCK_DISABLED, &InkMODSettings::clockDisabled, "clockDisabled",
                              StrId::STR_CAT_DISPLAY, /*invertedToggleDisplay=*/true));
    }
    add(SettingInfo::Enum(
        StrId::STR_REFRESH_FREQ, &InkMODSettings::refreshFrequency,
        {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
        "refreshFrequency", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(
            StrId::STR_UI_THEME, &InkMODSettings::uiTheme,
            {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_MINIMAL, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
             StrId::STR_THEME_LYRA_CAROUSEL, StrId::STR_THEME_ROUNDEDRAFF, StrId::STR_THEME_DASHBOARD},
            "uiTheme", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({InkMODSettings::UI_THEME::CLASSIC, InkMODSettings::UI_THEME::MINIMAL,
                                InkMODSettings::UI_THEME::LYRA, InkMODSettings::UI_THEME::LYRA_3_COVERS,
                                InkMODSettings::UI_THEME::LYRA_CAROUSEL,
                                InkMODSettings::UI_THEME::ROUNDEDRAFF, InkMODSettings::UI_THEME::DASHBOARD}));
    // Accessibility: bumps SMALL/UI_10's font IDs up to the next built-in Inter
    // size (UI_12 is already the largest built-in size, so it doesn't change).
    // Applied live - see UiTextSize.h / SettingsActivity::toggleCurrentSetting().
    add(SettingInfo::Enum(StrId::STR_UI_TEXT_SIZE, &InkMODSettings::uiTextSize,
                          {StrId::STR_NORMAL, StrId::STR_LARGE}, "uiTextSize", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues(
                {InkMODSettings::UI_TEXT_SIZE_OFF, InkMODSettings::UI_TEXT_SIZE_LARGE}));
    add(SettingInfo::Enum(StrId::STR_RECENT_BOOKS_VIEW, &InkMODSettings::recentBooksView,
                          {StrId::STR_LIST_VIEW, StrId::STR_GRID_VIEW}, "recentBooksView", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_SHOW_HOME_SEARCH, &InkMODSettings::showHomeSearch, "showHomeSearch",
                            StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &InkMODSettings::fadingFix, "fadingFix",
                            StrId::STR_CAT_DISPLAY));

    // --- Reader ---
    // Built-in font-family entry. Replaced per-call with a registry-aware
    // version when SD fonts are installed.
    add(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &InkMODSettings::fontFamily, {}, "fontFamily",
                          StrId::STR_CAT_READER));
    add(buildBuiltinFontSizeSetting());
    add(SettingInfo::Value(StrId::STR_LINE_SPACING, &InkMODSettings::lineHeightPercent,
                           {InkMODSettings::MIN_LINE_HEIGHT_PERCENT, InkMODSettings::MAX_LINE_HEIGHT_PERCENT,
                            InkMODSettings::LINE_HEIGHT_PERCENT_STEP},
                           "lineHeightPercent", StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION, &InkMODSettings::orientation,
                          {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_LANDSCAPE_CCW, StrId::STR_INVERTED},
                          "orientation", StrId::STR_CAT_READER)
            .withEnumRawValues({InkMODSettings::PORTRAIT, InkMODSettings::LANDSCAPE_CW,
                                InkMODSettings::LANDSCAPE_CCW, InkMODSettings::INVERTED}));
    add(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &InkMODSettings::screenMargin, {5, 40, 5}, "screenMargin",
                           StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_PUBLISHER_PAGE_NUMBERS, &InkMODSettings::publisherPageNumbers,
                            "publisherPageNumbers", StrId::STR_CAT_READER));
    add(SettingInfo::Enum(
        StrId::STR_PARA_ALIGNMENT, &InkMODSettings::paragraphAlignment,
        {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE},
        "paragraphAlignment", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &InkMODSettings::embeddedStyle, "embeddedStyle",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_HYPHENATION, &InkMODSettings::hyphenationEnabled, "hyphenationEnabled",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_TEXT_AA, &InkMODSettings::textAntiAliasing, "textAntiAliasing",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_READER_DARK_MODE, &InkMODSettings::readerDarkMode, "readerDarkMode",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_IMAGES, &InkMODSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_EXTRA_SPACING, &InkMODSettings::extraParagraphSpacing,
                          {StrId::STR_NONE_OPT, StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE},
                          "extraParagraphSpacing", StrId::STR_CAT_READER)
            .withEnumRawValues({0, 1, 2, 3}));
    add(SettingInfo::Enum(StrId::STR_READER_CLOCK_POSITION, &InkMODSettings::readerClockAtBottom,
                          {StrId::STR_TOP, StrId::STR_BOTTOM}, "readerClockAtBottom", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_FORCE_PARAGRAPH_INDENTS, &InkMODSettings::forceParagraphIndents,
                            "forceParagraphIndents", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_BIONIC_READING, &InkMODSettings::bionicReadingEnabled,
                            "bionicReadingEnabled", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_GUIDE_READING, &InkMODSettings::guideReadingEnabled, "guideReadingEnabled",
                            StrId::STR_CAT_READER));

    // --- Controls ---
    add(SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &InkMODSettings::sideButtonLayout,
                          {StrId::STR_DISABLED, StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_NEXT_NEXT},
                          "sideButtonLayout", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({InkMODSettings::SIDE_BUTTONS_DISABLED, InkMODSettings::PREV_NEXT,
                                InkMODSettings::NEXT_PREV, InkMODSettings::NEXT_NEXT}));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &InkMODSettings::sideButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_YES}, "sideButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_SIDE_BTN_LONG_PRESS, &InkMODSettings::sideButtonLongPress,
                          {StrId::STR_IGNORE, StrId::STR_CHAPTER_SKIP_OPT, StrId::STR_PAGES_10,
                           StrId::STR_CHANGE_FONT_SIZE, StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION,
                           StrId::STR_CREATE_CLIPPING},
                          "sideButtonLongPress", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({InkMODSettings::SIDE_LONG_OFF, InkMODSettings::SIDE_LONG_CHAPTER_SKIP,
                                InkMODSettings::SIDE_LONG_PAGE_SKIP_10, InkMODSettings::SIDE_LONG_FONT_SIZE,
                                InkMODSettings::SIDE_LONG_ORIENTATION_CHANGE, InkMODSettings::SIDE_LONG_CREATE_CLIPPING}));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &InkMODSettings::frontButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_NAV_BUTTONS, StrId::STR_ALL_BUTTONS},
                          "frontButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &InkMODSettings::longPressButtonBehavior,
                          {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                           StrId::STR_PAGES_10, StrId::STR_CHANGE_FONT_SIZE,
                           StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION, StrId::STR_CREATE_CLIPPING},
                          "longPressButtonBehavior", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({InkMODSettings::OFF, InkMODSettings::CHAPTER_SKIP, InkMODSettings::PAGE_SKIP_10,
                                InkMODSettings::FONT_SIZE_CHANGE, InkMODSettings::ORIENTATION_CHANGE,
                                InkMODSettings::LONG_PRESS_CREATE_CLIPPING}));
    add(SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &InkMODSettings::shortPwrBtn,
                          {StrId::STR_IGNORE,
                           StrId::STR_SLEEP,
                           StrId::STR_QUICK_RESUME_TIMEOUT,
                           StrId::STR_PAGE_TURN,
                           StrId::STR_TOGGLE_BOOKMARK,
                           StrId::STR_READING_STATS,
                           StrId::STR_MARK_FINISHED,
                           StrId::STR_FORCE_REFRESH,
                           StrId::STR_CHANGE_FONT,
                           StrId::STR_TOGGLE_GUIDE_DOTS,
                           StrId::STR_TOGGLE_BIONIC_READING,
                           StrId::STR_CYCLE_PAGE_TURN,
                           StrId::STR_FILE_TRANSFER,
                           StrId::STR_CALIBRE_WIRELESS,
                           StrId::STR_JOIN_NETWORK,
                           StrId::STR_CREATE_HOTSPOT,
                           StrId::STR_SCREENSHOT_BUTTON,
                           StrId::STR_READER_DARK_MODE,
                           StrId::STR_FOOTNOTES,
                           StrId::STR_BROWSE_FILES,
                           StrId::STR_DICTIONARY,
                           StrId::STR_CREATE_CLIPPING},
                          "shortPwrBtn", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({InkMODSettings::IGNORE,
                                InkMODSettings::SLEEP,
                                InkMODSettings::QUICK_RESUME_SLEEP,
                                InkMODSettings::PAGE_TURN,
                                InkMODSettings::TOGGLE_BOOKMARK,
                                InkMODSettings::READING_STATS,
                                InkMODSettings::MARK_FINISHED,
                                InkMODSettings::FORCE_REFRESH,
                                InkMODSettings::TOGGLE_FONT,
                                InkMODSettings::TOGGLE_GUIDE_DOTS,
                                InkMODSettings::TOGGLE_BIONIC_READING,
                                InkMODSettings::CYCLE_PAGE_TURN,
                                InkMODSettings::FILE_TRANSFER,
                                InkMODSettings::CALIBRE_WIRELESS,
                                InkMODSettings::JOIN_NETWORK,
                                InkMODSettings::CREATE_HOTSPOT,
                                InkMODSettings::SCREENSHOT,
                                InkMODSettings::TOGGLE_DARK_MODE,
                                InkMODSettings::FOOTNOTES,
                                InkMODSettings::FILE_BROWSER,
                                InkMODSettings::DICTIONARY_LOOKUP,
                                InkMODSettings::CREATE_CLIPPING}));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_ACTION, &InkMODSettings::longPwrBtn,
                          {StrId::STR_IGNORE,
                           StrId::STR_SLEEP,
                           StrId::STR_QUICK_RESUME_TIMEOUT,
                           StrId::STR_PAGE_TURN,
                           StrId::STR_TOGGLE_BOOKMARK,
                           StrId::STR_READING_STATS,
                           StrId::STR_MARK_FINISHED,
                           StrId::STR_FORCE_REFRESH,
                           StrId::STR_CHANGE_FONT,
                           StrId::STR_TOGGLE_GUIDE_DOTS,
                           StrId::STR_TOGGLE_BIONIC_READING,
                           StrId::STR_CYCLE_PAGE_TURN,
                           StrId::STR_FILE_TRANSFER,
                           StrId::STR_CALIBRE_WIRELESS,
                           StrId::STR_JOIN_NETWORK,
                           StrId::STR_CREATE_HOTSPOT,
                           StrId::STR_SCREENSHOT_BUTTON,
                           StrId::STR_READER_DARK_MODE,
                           StrId::STR_FOOTNOTES,
                           StrId::STR_BROWSE_FILES,
                           StrId::STR_DICTIONARY,
                           StrId::STR_CREATE_CLIPPING},
                          "longPwrBtn", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({InkMODSettings::IGNORE,
                                InkMODSettings::SLEEP,
                                InkMODSettings::QUICK_RESUME_SLEEP,
                                InkMODSettings::PAGE_TURN,
                                InkMODSettings::TOGGLE_BOOKMARK,
                                InkMODSettings::READING_STATS,
                                InkMODSettings::MARK_FINISHED,
                                InkMODSettings::FORCE_REFRESH,
                                InkMODSettings::TOGGLE_FONT,
                                InkMODSettings::TOGGLE_GUIDE_DOTS,
                                InkMODSettings::TOGGLE_BIONIC_READING,
                                InkMODSettings::CYCLE_PAGE_TURN,
                                InkMODSettings::FILE_TRANSFER,
                                InkMODSettings::CALIBRE_WIRELESS,
                                InkMODSettings::JOIN_NETWORK,
                                InkMODSettings::CREATE_HOTSPOT,
                                InkMODSettings::SCREENSHOT,
                                InkMODSettings::TOGGLE_DARK_MODE,
                                InkMODSettings::FOOTNOTES,
                                InkMODSettings::FILE_BROWSER,
                                InkMODSettings::DICTIONARY_LOOKUP,
                                InkMODSettings::CREATE_CLIPPING}));
    add(SettingInfo::Enum(
            StrId::STR_LONG_PRESS_MENU_ACTION, &InkMODSettings::longPressMenuAction,
            {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_TOGGLE_BOOKMARK, StrId::STR_READING_STATS,
             StrId::STR_MARK_FINISHED, StrId::STR_FORCE_REFRESH, StrId::STR_CHANGE_FONT, StrId::STR_TOGGLE_GUIDE_DOTS,
             StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_CYCLE_PAGE_TURN,
             StrId::STR_FILE_TRANSFER, StrId::STR_CALIBRE_WIRELESS, StrId::STR_JOIN_NETWORK, StrId::STR_CREATE_HOTSPOT,
             StrId::STR_SCREENSHOT_BUTTON, StrId::STR_READER_DARK_MODE, StrId::STR_FOOTNOTES, StrId::STR_BROWSE_FILES,
             StrId::STR_SYNC_PROGRESS, StrId::STR_DICTIONARY, StrId::STR_CREATE_CLIPPING},
            "longPressMenuAction", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(
                {InkMODSettings::LONG_MENU_OFF, InkMODSettings::LONG_MENU_SLEEP,
                 InkMODSettings::LONG_MENU_TOGGLE_BOOKMARK, InkMODSettings::LONG_MENU_READING_STATS,
                 InkMODSettings::LONG_MENU_MARK_FINISHED, InkMODSettings::LONG_MENU_REFRESH_SCREEN,
                 InkMODSettings::LONG_MENU_CHANGE_FONT, InkMODSettings::LONG_MENU_TOGGLE_GUIDE_DOTS,
                 InkMODSettings::LONG_MENU_TOGGLE_BIONIC, InkMODSettings::LONG_MENU_CYCLE_PAGE_TURN,
                 InkMODSettings::LONG_MENU_FILE_TRANSFER,
                 InkMODSettings::LONG_MENU_CALIBRE_WIRELESS, InkMODSettings::LONG_MENU_JOIN_NETWORK,
                 InkMODSettings::LONG_MENU_CREATE_HOTSPOT, InkMODSettings::LONG_MENU_SCREENSHOT,
                 InkMODSettings::LONG_MENU_TOGGLE_DARK_MODE, InkMODSettings::LONG_MENU_FOOTNOTES,
                 InkMODSettings::LONG_MENU_FILE_BROWSER, InkMODSettings::LONG_MENU_SYNC_PROGRESS,
                 InkMODSettings::LONG_MENU_DICTIONARY_LOOKUP, InkMODSettings::LONG_MENU_CREATE_CLIPPING}));
    add(SettingInfo::Enum(
            StrId::STR_LONG_PRESS_BACK_ACTION, &InkMODSettings::longPressBackAction,
            {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_TOGGLE_BOOKMARK, StrId::STR_READING_STATS,
             StrId::STR_MARK_FINISHED, StrId::STR_FORCE_REFRESH, StrId::STR_CHANGE_FONT, StrId::STR_TOGGLE_GUIDE_DOTS,
             StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_CYCLE_PAGE_TURN,
             StrId::STR_FILE_TRANSFER, StrId::STR_CALIBRE_WIRELESS, StrId::STR_JOIN_NETWORK, StrId::STR_CREATE_HOTSPOT,
             StrId::STR_SCREENSHOT_BUTTON, StrId::STR_READER_DARK_MODE, StrId::STR_FOOTNOTES, StrId::STR_BROWSE_FILES,
             StrId::STR_SYNC_PROGRESS, StrId::STR_DICTIONARY, StrId::STR_CREATE_CLIPPING},
            "longPressBackAction", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(
                {InkMODSettings::LONG_MENU_OFF, InkMODSettings::LONG_MENU_SLEEP,
                 InkMODSettings::LONG_MENU_TOGGLE_BOOKMARK, InkMODSettings::LONG_MENU_READING_STATS,
                 InkMODSettings::LONG_MENU_MARK_FINISHED, InkMODSettings::LONG_MENU_REFRESH_SCREEN,
                 InkMODSettings::LONG_MENU_CHANGE_FONT, InkMODSettings::LONG_MENU_TOGGLE_GUIDE_DOTS,
                 InkMODSettings::LONG_MENU_TOGGLE_BIONIC, InkMODSettings::LONG_MENU_CYCLE_PAGE_TURN,
                 InkMODSettings::LONG_MENU_FILE_TRANSFER,
                 InkMODSettings::LONG_MENU_CALIBRE_WIRELESS, InkMODSettings::LONG_MENU_JOIN_NETWORK,
                 InkMODSettings::LONG_MENU_CREATE_HOTSPOT, InkMODSettings::LONG_MENU_SCREENSHOT,
                 InkMODSettings::LONG_MENU_TOGGLE_DARK_MODE, InkMODSettings::LONG_MENU_FOOTNOTES,
                 InkMODSettings::LONG_MENU_FILE_BROWSER, InkMODSettings::LONG_MENU_SYNC_PROGRESS,
                 InkMODSettings::LONG_MENU_DICTIONARY_LOOKUP, InkMODSettings::LONG_MENU_CREATE_CLIPPING}));
    add(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &InkMODSettings::pwrBtnFootnoteBack,
                            "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS));

    // --- System ---
    add(SettingInfo::String(StrId::STR_DEVICE_NAME, SETTINGS.deviceName, sizeof(SETTINGS.deviceName), "deviceName",
                            StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Value(
        StrId::STR_TIME_TO_SLEEP, &InkMODSettings::sleepTimeoutMinutes,
        {InkMODSettings::MIN_SLEEP_TIMEOUT_MINUTES, InkMODSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
        "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &InkMODSettings::showHiddenFiles, "showHiddenFiles",
                            StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_HIDE_FILE_EXTENSION, &InkMODSettings::hideFileExtension, "hideFileExtension",
                            StrId::STR_CAT_SYSTEM, /*invertedToggleDisplay=*/true));
    add(SettingInfo::Enum(StrId::STR_FILE_BROWSER_DISPLAY, &InkMODSettings::fileBrowserDisplay,
                          {StrId::STR_FILE_BROWSER_DISPLAY_1_LINE, StrId::STR_FILE_BROWSER_DISPLAY_2_LINES},
                          "fileBrowserDisplay", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &InkMODSettings::removeReadBooksFromRecents,
                            "removeReadBooksFromRecents", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &InkMODSettings::moveFinishedToReadFolder,
                            "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_AUTO_BACKUP_STATS, &InkMODSettings::autoBackupStats, "autoBackupStats",
                            StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Value(StrId::STR_IDLE_TIME_THRESHOLD, &InkMODSettings::readingIdleTimeThresholdUnits,
                           {InkMODSettings::MIN_READING_IDLE_TIME_THRESHOLD_UNITS,
                            InkMODSettings::MAX_READING_IDLE_TIME_THRESHOLD_UNITS, 1},
                           "readingIdleTimeThresholdUnits", StrId::STR_CAT_SYSTEM));
#ifdef INKMOD_ENABLE_READING_STATS_TOGGLE
    add(SettingInfo::Toggle(StrId::STR_TRACK_READING_STATS, &InkMODSettings::trackReadingStats, "trackReadingStats",
                            StrId::STR_CAT_SYSTEM));
#endif

    // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
    add(SettingInfo::DynamicString(
        StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
        [](const std::string& v) {
          KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
        },
        "koUsername", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicString(
        StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
        [](const std::string& v) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
          KOREADER_STORE.saveToFile();
        },
        "koPassword", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicString(
        StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
        [](const std::string& v) {
          KOREADER_STORE.setServerUrl(v);
          KOREADER_STORE.saveToFile();
        },
        "koServerUrl", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicEnum(
        StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
        [](uint8_t v) {
          KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
          KOREADER_STORE.saveToFile();
        },
        "koMatchMethod", StrId::STR_KOREADER_SYNC));

    // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
    // Three values: hidden, chapter-relative, and whole-book.  This must be
    // an enum (not a binary toggle), otherwise JSON loading clamps "By book"
    // (raw value 2) back to "By chapter" after a deep-sleep reboot.
    add(SettingInfo::Enum(StrId::STR_CHAPTER_PAGE_COUNT, &InkMODSettings::statusBarChapterPageCount,
                          {StrId::STR_HIDE, StrId::STR_PAGE_COUNT_MODE_CHAPTER, StrId::STR_PAGE_COUNT_MODE_BOOK},
                          "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &InkMODSettings::statusBarBookProgressPercentage,
                            "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &InkMODSettings::statusBarProgressBar,
                          {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER}, "statusBarProgressBar",
                          StrId::STR_CUSTOMISE_STATUS_BAR)
            .withEnumRawValues({InkMODSettings::HIDE_PROGRESS, InkMODSettings::BOOK_PROGRESS,
                                InkMODSettings::CHAPTER_PROGRESS}));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &InkMODSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_TITLE, &InkMODSettings::statusBarTitle,
                          {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER}, "statusBarTitle",
                          StrId::STR_CUSTOMISE_STATUS_BAR)
            .withEnumRawValues(
                {InkMODSettings::HIDE_TITLE, InkMODSettings::BOOK_TITLE, InkMODSettings::CHAPTER_TITLE}));
    add(SettingInfo::Enum(StrId::STR_TIME_LEFT, &InkMODSettings::statusBarTimeLeft,
                          {StrId::STR_HIDE, StrId::STR_CHAPTER, StrId::STR_BOOK}, "statusBarTimeLeft",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Toggle(StrId::STR_BATTERY, &InkMODSettings::statusBarBattery, "statusBarBattery",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &InkMODSettings::xtcStatusBarMode,
                          {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    // Clock detail entries live under System > Device in the device UI.
    // Range 0..104 = quarter-hour steps from UTC-12:00 to UTC+14:00, biased by 48.
    add(SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &InkMODSettings::clockUtcOffsetQ, {0, 104, 1},
                           "clockUtcOffsetQ", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &InkMODSettings::clockFormat,
                          {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                          StrId::STR_CAT_SYSTEM));
    // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
    // on next WiFi connect, which is useful when crossing time zones.
    add(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &InkMODSettings::clockHasBeenSynced, "clockHasBeenSynced",
                            StrId::STR_CAT_SYSTEM));
    // Only show tilt page turn setting when the QMI8658 IMU is present (X3).
    if (halTiltSensor.isAvailable()) {
      for (auto& setting : v) {
        if (setting.nameId == StrId::STR_SHORT_PWR_BTN || setting.nameId == StrId::STR_LONG_PRESS_ACTION ||
            setting.nameId == StrId::STR_LONG_PRESS_MENU_ACTION ||
            setting.nameId == StrId::STR_LONG_PRESS_BACK_ACTION) {
          const uint8_t rawValue =
              setting.nameId == StrId::STR_LONG_PRESS_MENU_ACTION || setting.nameId == StrId::STR_LONG_PRESS_BACK_ACTION
                  ? static_cast<uint8_t>(InkMODSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN)
                  : static_cast<uint8_t>(InkMODSettings::TOGGLE_TILT_PAGE_TURN);
          insertEnumOptionAfter(setting, StrId::STR_CYCLE_PAGE_TURN, StrId::STR_TILT_PAGE_TURN, rawValue);
        }
      }
      auto shortPowerButtonIt = std::find_if(
          v.begin(), v.end(), [](const SettingInfo& setting) { return setting.nameId == StrId::STR_SHORT_PWR_BTN; });
      if (shortPowerButtonIt != v.end()) {
        auto insertPos = v.insert(shortPowerButtonIt + 1,
                                  SettingInfo::Toggle(StrId::STR_TILT_PAGE_TURN, &InkMODSettings::tiltPageTurn,
                                                      "tiltPageTurn", StrId::STR_CAT_CONTROLS));
        v.insert(
            insertPos + 1,
            SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN_DIRECTION, &InkMODSettings::tiltPageTurnDirection,
                              {StrId::STR_TILT_DIRECTION_LEFT_RIGHT, StrId::STR_TILT_DIRECTION_LEFT_RIGHT_INVERTED,
                               StrId::STR_TILT_DIRECTION_FORWARD_BACK, StrId::STR_TILT_DIRECTION_FORWARD_BACK_INVERTED},
                              "tiltPageTurnDirection", StrId::STR_CAT_CONTROLS));
      }
    }
    return v;
  }();

  std::vector<SettingInfo> v = baseList;
  if (registry && registry->getFamilyCount() > 0) {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_FAMILY; });
    if (it != v.end()) {
      *it = buildFontFamilySetting(registry);
    }
    auto fontSizeIt =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_SIZE; });
    if (fontSizeIt != v.end()) {
      *fontSizeIt = buildFontSizeSetting(registry);
    }
  }
  if (!gpio.deviceIsX3()) {
    auto sleepScreenIt =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_SLEEP_SCREEN; });
    if (sleepScreenIt != v.end()) {
      removeEnumRawValue(*sleepScreenIt, static_cast<uint8_t>(InkMODSettings::MINIMAL_STATS_SLEEP));
    }
  }
  if (SETTINGS.clockDisabled) {
    // Calendar needs a real clock to be meaningful - hide it from the picker
    // once the user has turned the clock off (X4-only setting).
    auto sleepScreenIt =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_SLEEP_SCREEN; });
    if (sleepScreenIt != v.end()) {
      removeEnumRawValue(*sleepScreenIt, static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP));
      removeEnumRawValue(*sleepScreenIt, static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP_INVERTED));
      removeEnumRawValue(*sleepScreenIt, static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP_LANDSCAPE));
      removeEnumRawValue(*sleepScreenIt, static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP_LANDSCAPE_INVERTED));
    }
  }
  return v;
}

inline std::vector<SettingInfo> buildGroupedReaderSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> readerSettings;
  readerSettings.reserve(22);

  auto addReaderSetting = [&](StrId nameId) {
    const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                                 [nameId](const auto& setting) { return setting.nameId == nameId; });
    if (it != allSettings.end()) {
      readerSettings.push_back(*it);
    }
  };

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_FONT_OPTIONS));
  addReaderSetting(StrId::STR_FONT_FAMILY);
  addReaderSetting(StrId::STR_FONT_SIZE);

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_PAGE_LAYOUT));
  addReaderSetting(StrId::STR_LINE_SPACING);
  addReaderSetting(StrId::STR_SCREEN_MARGIN);
  addReaderSetting(StrId::STR_PARA_ALIGNMENT);
  addReaderSetting(StrId::STR_EXTRA_SPACING);
  addReaderSetting(StrId::STR_FORCE_PARAGRAPH_INDENTS);

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_BOOK_STYLING));
  addReaderSetting(StrId::STR_EMBEDDED_STYLE);
  addReaderSetting(StrId::STR_HYPHENATION);
  addReaderSetting(StrId::STR_TEXT_AA);
  addReaderSetting(StrId::STR_IMAGES);

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_READING_AIDS));
  addReaderSetting(StrId::STR_BIONIC_READING);
  addReaderSetting(StrId::STR_GUIDE_READING);

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_UI));
  addReaderSetting(StrId::STR_ORIENTATION);
  addReaderSetting(StrId::STR_PUBLISHER_PAGE_NUMBERS);
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  return readerSettings;
}

inline void addSettingByName(std::vector<SettingInfo>& target, const std::vector<SettingInfo>& allSettings,
                             StrId nameId) {
  const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                               [nameId](const auto& setting) { return setting.nameId == nameId; });
  if (it != allSettings.end()) {
    target.push_back(*it);
  }
}

inline std::vector<SettingInfo> buildReaderSettingsParentList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> readerSettings;
  readerSettings.reserve(9);
  readerSettings.push_back(SettingInfo::Submenu(StrId::STR_READER_FONT_OPTIONS, SettingAction::ReaderFontOptions));
  readerSettings.push_back(SettingInfo::Submenu(StrId::STR_READER_PAGE_LAYOUT, SettingAction::ReaderPageLayout));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  addSettingByName(readerSettings, allSettings, StrId::STR_PUBLISHER_PAGE_NUMBERS);
  addSettingByName(readerSettings, allSettings, StrId::STR_READER_DARK_MODE);
  addSettingByName(readerSettings, allSettings, StrId::STR_EMBEDDED_STYLE);
  addSettingByName(readerSettings, allSettings, StrId::STR_IMAGES);
  addSettingByName(readerSettings, allSettings, StrId::STR_BIONIC_READING);
  addSettingByName(readerSettings, allSettings, StrId::STR_GUIDE_READING);
  if (!SETTINGS.clockDisabled && halClock.isAvailable()) {
    addSettingByName(readerSettings, allSettings, StrId::STR_READER_CLOCK_POSITION);
  }
  return readerSettings;
}

inline std::vector<SettingInfo> buildReaderFontSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(5);
  addSettingByName(settings, allSettings, StrId::STR_FONT_FAMILY);
  addSettingByName(settings, allSettings, StrId::STR_FONT_SIZE);
  addSettingByName(settings, allSettings, StrId::STR_TEXT_AA);
  return settings;
}

inline std::vector<SettingInfo> buildReaderPageLayoutSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(6);
  addSettingByName(settings, allSettings, StrId::STR_ORIENTATION);
  addSettingByName(settings, allSettings, StrId::STR_LINE_SPACING);
  addSettingByName(settings, allSettings, StrId::STR_SCREEN_MARGIN);
  addSettingByName(settings, allSettings, StrId::STR_PARA_ALIGNMENT);
  addSettingByName(settings, allSettings, StrId::STR_HYPHENATION);
  addSettingByName(settings, allSettings, StrId::STR_EXTRA_SPACING);
  addSettingByName(settings, allSettings, StrId::STR_FORCE_PARAGRAPH_INDENTS);
  return settings;
}

inline void addSettingByKey(std::vector<SettingInfo>& target, const std::vector<SettingInfo>& allSettings,
                            const char* key) {
  const auto it = std::find_if(allSettings.begin(), allSettings.end(), [key](const auto& setting) {
    return setting.key && std::strcmp(setting.key, key) == 0;
  });
  if (it != allSettings.end()) {
    target.push_back(*it);
  }
}

inline bool hasSettingByName(const std::vector<SettingInfo>& allSettings, StrId nameId) {
  return std::any_of(allSettings.begin(), allSettings.end(),
                     [nameId](const auto& setting) { return setting.nameId == nameId; });
}

inline std::vector<SettingInfo> buildControlsSettingsParentList(const std::vector<SettingInfo>& allSettings) {
  const bool hasTiltPageTurnSetting = hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN);
  const bool hasTiltPageTurnDirectionSetting = hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN_DIRECTION);

  std::vector<SettingInfo> settings;
  settings.reserve(3 + (hasTiltPageTurnSetting ? 1u : 0u) + (hasTiltPageTurnDirectionSetting ? 1u : 0u));
  settings.push_back(SettingInfo::Submenu(StrId::STR_POWER_BUTTON, SettingAction::ControlsPowerButton));
  settings.push_back(SettingInfo::Submenu(StrId::STR_FRONT_BUTTONS, SettingAction::ControlsFrontButtons));
  settings.push_back(SettingInfo::Submenu(StrId::STR_SIDE_BUTTONS, SettingAction::ControlsSideButtons));
  if (hasTiltPageTurnSetting) addSettingByName(settings, allSettings, StrId::STR_TILT_PAGE_TURN);
  if (hasTiltPageTurnDirectionSetting) addSettingByName(settings, allSettings, StrId::STR_TILT_PAGE_TURN_DIRECTION);
  return settings;
}

inline std::vector<SettingInfo> buildControlsPowerSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(3);
  addSettingByName(settings, allSettings, StrId::STR_SHORT_PWR_BTN);
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_ACTION);
  if (SETTINGS.shortPwrBtn == InkMODSettings::SHORT_PWRBTN::FOOTNOTES ||
      SETTINGS.longPwrBtn == InkMODSettings::SHORT_PWRBTN::FOOTNOTES ||
      SETTINGS.longPressMenuAction == InkMODSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FOOTNOTES ||
      SETTINGS.longPressBackAction == InkMODSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FOOTNOTES) {
    addSettingByName(settings, allSettings, StrId::STR_PWR_BTN_FOOTNOTE_BACK);
  }
  return settings;
}

inline std::vector<SettingInfo> buildControlsFrontButtonSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(6);
  settings.push_back(SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  settings.push_back(
      SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS_READER, SettingAction::RemapFrontButtonsReader));
  addSettingByKey(settings, allSettings, "frontButtonOrientationAware");
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_BEHAVIOR);
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_BACK_ACTION);
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_MENU_ACTION);
  return settings;
}

inline std::vector<SettingInfo> buildControlsSideButtonSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(3);
  addSettingByName(settings, allSettings, StrId::STR_SIDE_BTN_LAYOUT);
  addSettingByKey(settings, allSettings, "sideButtonOrientationAware");
  addSettingByName(settings, allSettings, StrId::STR_SIDE_BTN_LONG_PRESS);
  return settings;
}

inline std::vector<SettingInfo> buildGroupedDisplaySettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> displaySettings;
  displaySettings.reserve(8);

  auto addDisplaySetting = [&](StrId nameId) {
    const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                                 [nameId](const auto& setting) { return setting.nameId == nameId; });
    if (it != allSettings.end()) {
      displaySettings.push_back(*it);
    }
  };

  displaySettings.push_back(SettingInfo::Submenu(StrId::STR_DISPLAY_SLEEP_SCREEN, SettingAction::DisplaySleepScreen));
  addDisplaySetting(StrId::STR_HIDE_BATTERY);

  // Keep the master X4 clock switch visible so the clock can be enabled again,
  // but hide all subordinate clock-display options while it is disabled.
  if (halClock.isAvailable() && !SETTINGS.clockDisabled) {
    addDisplaySetting(StrId::STR_HIDE_CLOCK);
  }
  if (!gpio.deviceIsX3()) {
    addDisplaySetting(StrId::STR_CLOCK_DISABLED);
  }

  addDisplaySetting(StrId::STR_REFRESH_FREQ);
  addDisplaySetting(StrId::STR_UI_THEME);
  addDisplaySetting(StrId::STR_UI_TEXT_SIZE);
  addDisplaySetting(StrId::STR_RECENT_BOOKS_VIEW);
  addDisplaySetting(StrId::STR_SHOW_HOME_SEARCH);
  addDisplaySetting(StrId::STR_SUNLIGHT_FADING_FIX);

  return displaySettings;
}

inline std::vector<SettingInfo> buildDisplaySleepSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> sleepSettings;
  sleepSettings.reserve(4);

  auto addSleepSetting = [&](StrId nameId, StrId displayNameId) {
    const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                                 [nameId](const auto& setting) { return setting.nameId == nameId; });
    if (it != allSettings.end()) {
      sleepSettings.push_back(*it);
      sleepSettings.back().nameId = displayNameId;
    }
  };

  addSleepSetting(StrId::STR_SLEEP_SCREEN, StrId::STR_SLEEP_SCREEN_WALLPAPER);
  addSleepSetting(StrId::STR_SLEEP_COVER_MODE, StrId::STR_SLEEP_COVER_MODE_SHORT);
  addSleepSetting(StrId::STR_SLEEP_COVER_FILTER, StrId::STR_SLEEP_COVER_FILTER_SHORT);
  addSleepSetting(StrId::STR_TIMEOUT_SLEEP_SCREEN, StrId::STR_TIMEOUT_SLEEP_SCREEN);

  return sleepSettings;
}

inline std::vector<SettingInfo> buildSystemSettingsParentList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> systemSettings;
  systemSettings.reserve(8);
  systemSettings.push_back(SettingInfo::Submenu(StrId::STR_SYSTEM_DEVICE, SettingAction::SystemDevice));
  systemSettings.push_back(SettingInfo::Submenu(StrId::STR_SYSTEM_FILES_CACHE, SettingAction::SystemFilesCache));
  systemSettings.push_back(SettingInfo::Submenu(StrId::STR_READING_STATS, SettingAction::SystemReadingStats));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_BOOK_MENU_SETTINGS, SettingAction::BookMenuSettings));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SUPPORT_INKMOD, SettingAction::SupportInkMOD));
  return systemSettings;
}

namespace StorageUsageCalc {
// inline functions defining function-local statics, included from both
// SettingsList.h (the display side) and SettingsActivity.cpp (the "user
// pressed Select" side) - the one-definition rule merges these across
// translation units, so both sides see the same started/ready/cachedUsed
// state without needing a dedicated .cpp file just for three variables.
inline bool& started() { static bool v = false; return v; }
inline bool& ready() { static bool v = false; return v; }
inline uint64_t& cachedUsedBytes() { static uint64_t v = 0; return v; }

// Called when the user selects the "Внутренняя память" row. Idempotent -
// later presses while already running/done do nothing.
inline void start() {
  // cppcheck-suppress knownConditionTrueFalse
  if (started()) return;
  started() = true;
  xTaskCreate(
      [](void*) {
        // The scan needs HalStorage's lock for correctness (SD and the
        // e-ink display share one SPI bus - see HalSpiBus), and there's no
        // way to make that shorter without touching the scan itself. A
        // short delay first lets whatever redraw the button press itself
        // triggers (e.g. the row's own highlight/"Загрузка..." text) go
        // out before this grabs the bus for the ~12s the real scan takes.
        vTaskDelay(pdMS_TO_TICKS(500));
        cachedUsedBytes() = Storage.getCardUsedBytes();
        ready() = true;
        vTaskDelete(nullptr);
      },
      "SdUsageScan", 4096, nullptr, 1, nullptr);
}

inline std::string display() {
  if (!started()) return std::string(tr(STR_TAP_TO_CALCULATE));
  if (!ready()) return tr(STR_LOADING_POPUP) + std::string("...");

  const uint64_t used = cachedUsedBytes();
  const uint64_t total = Storage.getCardTotalBytes();
  if (total == 0) {
    char usedOnly[32];
    if (used < 1073741824ULL) {
      snprintf(usedOnly, sizeof(usedOnly), "%.0f %s", used / 1048576.0, tr(STR_UNIT_MB_SHORT));
    } else {
      snprintf(usedOnly, sizeof(usedOnly), "%.1f %s", used / 1073741824.0, tr(STR_UNIT_GB_SHORT));
    }
    return std::string(usedOnly);
  }
  char buf[32];
  if (used < 1073741824ULL) {
    // Under 1GB used on a card this size is worth saying in MB - "0 ГБ"
    // (the whole-GB rounding this used before) reads as "couldn't tell",
    // not "barely anything's on here yet".
    snprintf(buf, sizeof(buf), "%.0f %s / %.0f %s", used / 1048576.0, tr(STR_UNIT_MB_SHORT), total / 1073741824.0,
             tr(STR_UNIT_GB_SHORT));
  } else {
    // "%.0f" rather than an integer divide: rounds 1.98GB up to "2 ГБ"
    // instead of truncating to "1 ГБ".
    snprintf(buf, sizeof(buf), "%.0f / %.0f %s", used / 1073741824.0, total / 1073741824.0, tr(STR_UNIT_GB_SHORT));
  }
  return std::string(buf);
}
}  // namespace StorageUsageCalc

inline std::vector<SettingInfo> buildSystemDeviceSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(9);
  addSettingByName(settings, allSettings, StrId::STR_DEVICE_NAME);
  addSettingByName(settings, allSettings, StrId::STR_TIME_TO_SLEEP);
  settings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  if (!SETTINGS.clockDisabled && halClock.isAvailable()) {
    addSettingByName(settings, allSettings, StrId::STR_CLOCK_FORMAT);
    addSettingByName(settings, allSettings, StrId::STR_CLOCK_UTC_OFFSET);
    settings.push_back(SettingInfo::Action(StrId::STR_CLOCK_SYNC_NOW, SettingAction::ClockSync));
  }

  settings.push_back(SettingInfo::Action(StrId::STR_DIAGNOSTICS, SettingAction::SystemDiagnostics));
  return settings;
}

inline std::vector<SettingInfo> buildSystemFilesCacheSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(6);
  addSettingByName(settings, allSettings, StrId::STR_SHOW_HIDDEN_FILES);
  addSettingByName(settings, allSettings, StrId::STR_HIDE_FILE_EXTENSION);
  addSettingByName(settings, allSettings, StrId::STR_FILE_BROWSER_DISPLAY);
  addSettingByName(settings, allSettings, StrId::STR_REMOVE_READ_FROM_RECENTS);
  addSettingByName(settings, allSettings, StrId::STR_MOVE_FINISHED_TO_READ);
  settings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  return settings;
}

inline std::vector<SettingInfo> buildSystemReadingStatsSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(3);
  addSettingByName(settings, allSettings, StrId::STR_TRACK_READING_STATS);
  settings.push_back(SettingInfo::Submenu(StrId::STR_ALL_TIME_STATS, SettingAction::SystemGlobalStats));
  addSettingByName(settings, allSettings, StrId::STR_IDLE_TIME_THRESHOLD);
  return settings;
}

inline std::vector<SettingInfo> buildSystemGlobalStatsSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(4);
  if (halClock.isAvailable()) {
    addSettingByName(settings, allSettings, StrId::STR_AUTO_BACKUP_STATS);
  }
  settings.push_back(SettingInfo::Action(StrId::STR_BACKUP_NOW, SettingAction::BackupStats));
  settings.push_back(SettingInfo::Action(StrId::STR_RESTORE_STATS, SettingAction::RestoreStats));
  settings.push_back(SettingInfo::Action(StrId::STR_RESET_ALL_TIME_STATS, SettingAction::ResetGlobalStats));
  return settings;
}
