#pragma once
#include <I18n.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "InkMODSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING, SECTION_HEADER, SUBMENU, INFO };

enum class SettingAction {
  None,
  RemapFrontButtons,
  RemapFrontButtonsReader,
  CustomiseStatusBar,
  KOReaderSync,
  OPDSBrowser,
  DisplaySleepScreen,
  ReaderFontOptions,
  ReaderPageLayout,
  ControlsPowerButton,
  ControlsFrontButtons,
  ControlsSideButtons,
  SystemDevice,
  SystemFilesCache,
  SystemReadingStats,
  SystemGlobalStats,
  Network,
  BackupStats,
  ResetGlobalStats,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  SwitchOtaSlot,
  Language,
  DownloadFonts,
  ClockSync,
  CalculateStorageUsage,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t InkMODSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  std::vector<uint8_t> enumRawValues;
  std::vector<std::string> enumStringValues;  // runtime alternative to StrId enumValues (for SD card fonts etc.)
  SettingAction action = SettingAction::None;
  // TOGGLE only: the stored field's own sense is the opposite of what the
  // row should say - e.g. InkMODSettings::clockDisabled is true when the
  // clock is OFF, so showing "ВКЛ"/"ВЫКЛ" straight off that value reads
  // backwards. Flips which label the ON/OFF text uses without touching
  // the field's actual stored meaning (renaming/inverting the field
  // itself would also flip its JSON key, silently changing what an
  // existing saved settings file means).
  bool invertedToggleDisplay = false;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)

  // Direct char[] string fields (for settings stored in InkMODSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside InkMODSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t InkMODSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT, bool invertedToggleDisplay = false) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    s.invertedToggleDisplay = invertedToggleDisplay;
    return s;
  }

  // Non-interactive row: shows nameId's localized label on the left and
  // getter()'s current value on the right, same visual row style as any
  // other setting, but never enters an edit/toggle/enum interaction and is
  // skipped over by up/down navigation - same treatment as SECTION_HEADER
  // (see the loops in SettingsActivity.cpp that check for that type).
  // For things like storage usage or "time since last charge" that are
  // computed live from hardware/state rather than stored in InkMODSettings.
  static SettingInfo Info(StrId nameId, std::function<std::string()> getter,
                          StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::INFO;
    s.stringGetter = std::move(getter);
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t InkMODSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  SettingInfo& withEnumRawValues(std::vector<uint8_t> values) {
    enumRawValues = std::move(values);
    return *this;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Submenu(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::SUBMENU;
    s.action = action;
    return s;
  }

  static SettingInfo SectionHeader(StrId nameId) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::SECTION_HEADER;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t InkMODSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

inline size_t settingEnumOptionCount(const SettingInfo& setting) {
  return setting.enumStringValues.empty() ? setting.enumValues.size() : setting.enumStringValues.size();
}

inline std::string settingEnumOptionLabel(const SettingInfo& setting, const uint8_t displayIndex) {
  if (!setting.enumStringValues.empty()) {
    return displayIndex < setting.enumStringValues.size() ? setting.enumStringValues[displayIndex] : std::string();
  }
  return displayIndex < setting.enumValues.size() ? std::string(I18N.get(setting.enumValues[displayIndex]))
                                                  : std::string();
}

inline uint8_t settingEnumDisplayIndexForRawValue(const SettingInfo& setting, uint8_t rawValue) {
  if (setting.enumRawValues.empty()) {
    return rawValue;
  }

  auto it = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), rawValue);
  if (it == setting.enumRawValues.end()) {
    return 0;
  }
  return static_cast<uint8_t>(std::distance(setting.enumRawValues.begin(), it));
}

inline uint8_t settingEnumRawValueForDisplayIndex(const SettingInfo& setting, uint8_t displayIndex) {
  if (setting.enumRawValues.empty()) {
    return displayIndex;
  }
  if (displayIndex >= setting.enumRawValues.size()) {
    return setting.enumRawValues.front();
  }
  return setting.enumRawValues[displayIndex];
}

inline bool settingShowsNavigationCaret(const SettingInfo& setting) {
  return setting.type == SettingType::SUBMENU || setting.action == SettingAction::CustomiseStatusBar;
}

class SettingsActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedCategoryIndex = 0;  // Currently selected category
  int selectedSettingIndex = 0;
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> displaySleepSettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> readerFontSettings;
  std::vector<SettingInfo> readerPageLayoutSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> controlsPowerSettings;
  std::vector<SettingInfo> controlsFrontButtonSettings;
  std::vector<SettingInfo> controlsSideButtonSettings;
  std::vector<SettingInfo> systemSettings;
  std::vector<SettingInfo> systemDeviceSettings;
  std::vector<SettingInfo> systemFilesCacheSettings;
  std::vector<SettingInfo> systemReadingStatsSettings;
  std::vector<SettingInfo> systemGlobalStatsSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;
  SettingAction activeSubmenu = SettingAction::None;
  SettingAction parentSubmenu = SettingAction::None;
  // Which row was selected in the parent list right before openSubmenu()
  // switched to the child list - closeSubmenu() restores this instead of
  // always landing back on row 1, so backing out of a submenu leaves the
  // cursor where it was rather than resetting to the top of the parent
  // list every time.
  int parentSelectedIndex = 0;

  static constexpr int categoryCount = 4;
  static const StrId categoryNames[categoryCount];

  void enterCategory(int categoryIndex);
  void setCurrentSettingsForCategory();
  StrId activeSubmenuTitleId() const;
  void openSubmenu(SettingAction action);
  void closeSubmenu();
  bool currentSettingUsesOptionMenu(const SettingInfo& setting) const;
  void openEnumOptionPicker(const SettingInfo& setting);
  void openScreenMarginPicker(const SettingInfo& setting);
  void openIdleTimeThresholdPicker();
  void toggleCurrentSetting();
  void openSleepTimeoutPicker();
  void openLineHeightPicker();
  void openStringEditor(const SettingInfo& setting);
  void rebuildSettingsLists();
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Settings", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
