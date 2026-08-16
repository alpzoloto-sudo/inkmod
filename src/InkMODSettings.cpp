#include "InkMODSettings.h"

#include <HalGPIO.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>

#include "I18nKeys.h"
#include "fontIds.h"

// Initialize the static instance
InkMODSettings InkMODSettings::instance;

void readAndValidate(FsFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 1;
constexpr char SETTINGS_FILE_BIN[] = "/.inkmod/settings.bin";
constexpr char SETTINGS_FILE_JSON[] = "/.inkmod/inkmod-settings.json";
constexpr char LEGACY_SETTINGS_FILE_JSON[] = "/.inkmod/settings.json";
constexpr char SETTINGS_FILE_BAK[] = "/.inkmod/settings.bin.bak";
constexpr char LANG_FILE_BIN[] = "/.inkmod/language.bin";
constexpr char LANG_FILE_BAK[] = "/.inkmod/language.bin.bak";
constexpr uint8_t INVALID_READER_FONT_SIZE = 0xFF;
constexpr uint8_t SLEEP_SCREEN_STORAGE_ORDER[] = {
    static_cast<uint8_t>(InkMODSettings::DARK),
    static_cast<uint8_t>(InkMODSettings::LIGHT),
    static_cast<uint8_t>(InkMODSettings::CUSTOM),
    static_cast<uint8_t>(InkMODSettings::COVER),
    static_cast<uint8_t>(InkMODSettings::BLANK),
    static_cast<uint8_t>(InkMODSettings::COVER_CUSTOM),
    static_cast<uint8_t>(InkMODSettings::OVERLAY),
    static_cast<uint8_t>(InkMODSettings::READING_STATS_SLEEP),
    static_cast<uint8_t>(InkMODSettings::MINIMAL_SLEEP),
    static_cast<uint8_t>(InkMODSettings::QUICK_RESUME),
    static_cast<uint8_t>(InkMODSettings::MINIMAL_STATS_SLEEP),
    static_cast<uint8_t>(InkMODSettings::DASHBOARD_SLEEP),
    static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP),
    static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP_INVERTED),
    static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP_LANDSCAPE),
    static_cast<uint8_t>(InkMODSettings::CALENDAR_SLEEP_LANDSCAPE_INVERTED),
};
constexpr uint8_t SLEEP_SCREEN_STORAGE_ORDER_COUNT =
    sizeof(SLEEP_SCREEN_STORAGE_ORDER) / sizeof(SLEEP_SCREEN_STORAGE_ORDER[0]);
static_assert(SLEEP_SCREEN_STORAGE_ORDER_COUNT == InkMODSettings::SLEEP_SCREEN_MODE_COUNT,
              "Update sleep screen persisted-value mapping when adding modes");
constexpr InkMODSettings::FONT_SIZE READER_FONT_SIZE_STORAGE_ORDER[] = {
    InkMODSettings::TINY,      InkMODSettings::SMALL,       InkMODSettings::MEDIUM,
    InkMODSettings::LARGE,     InkMODSettings::EXTRA_LARGE, InkMODSettings::TEENSY,
    InkMODSettings::HUGE_SIZE, InkMODSettings::ITTY_BITTY};
constexpr InkMODSettings::FONT_SIZE READER_FONT_SIZE_CYCLE_ORDER[] = {
    InkMODSettings::TEENSY,      InkMODSettings::ITTY_BITTY, InkMODSettings::TINY,
    InkMODSettings::SMALL,       InkMODSettings::MEDIUM,     InkMODSettings::LARGE,
    InkMODSettings::EXTRA_LARGE, InkMODSettings::HUGE_SIZE};
constexpr uint8_t SD_FONT_RANGE_POINT_SIZES[InkMODSettings::SD_FONT_SIZE_RANGE_COUNT]
                                           [InkMODSettings::SD_FONT_MAX_SIZE_STEPS] = {
                                               {8, 9, 10, 12},
                                               {10, 12, 14, 16},
                                               {16, 18, 20},
                                               {10, 12, 14, 16, 18},
                                               {8, 9, 10, 12, 14, 16, 18, 20},
};
constexpr uint8_t SD_FONT_RANGE_STEP_COUNTS[InkMODSettings::SD_FONT_SIZE_RANGE_COUNT] = {4, 4, 3, 5, 8};

bool isValidDeviceName(const char* name) {
  if (!name) return false;
  const size_t len = std::strlen(name);
  return len >= InkMODSettings::MIN_DEVICE_NAME_LENGTH && len <= InkMODSettings::MAX_DEVICE_NAME_LENGTH;
}

uint8_t normalizedSdFontRange(uint8_t range) {
  return range < InkMODSettings::SD_FONT_SIZE_RANGE_COUNT ? range : InkMODSettings::SD_FONT_RANGE_TINY;
}

bool isReaderFontSizeAvailable(const InkMODSettings::FONT_SIZE size) {
  switch (size) {
    case InkMODSettings::TEENSY:
#ifdef OMIT_TEENSY_FONT
      return false;
#else
      return true;
#endif
    case InkMODSettings::ITTY_BITTY:
#ifdef OMIT_ITTY_BITTY_FONT
      return false;
#else
      return true;
#endif
    case InkMODSettings::TINY:
#ifdef OMIT_TINY_FONT
      return false;
#else
      return true;
#endif
    case InkMODSettings::SMALL:
#ifdef OMIT_SMALL_FONT
      return false;
#else
      return true;
#endif
    case InkMODSettings::MEDIUM:
#ifdef OMIT_MEDIUM_FONT
      return false;
#else
      return true;
#endif
    case InkMODSettings::EXTRA_LARGE:
#ifdef OMIT_XLARGE_FONT
      return false;
#else
      return true;
#endif
    case InkMODSettings::LARGE:
#ifdef OMIT_LARGE_FONT
      return false;
#else
      return true;
#endif
    case InkMODSettings::HUGE_SIZE:
#ifdef OMIT_HUGE_FONT
      return false;
#else
      return true;
#endif
    default:
      return true;
  }
}

InkMODSettings::FONT_SIZE firstAvailableReaderFontSize() {
  const auto it =
      std::find_if(std::begin(READER_FONT_SIZE_STORAGE_ORDER), std::end(READER_FONT_SIZE_STORAGE_ORDER),
                   [](const InkMODSettings::FONT_SIZE size) { return isReaderFontSizeAvailable(size); });
  return (it != std::end(READER_FONT_SIZE_STORAGE_ORDER)) ? *it : InkMODSettings::LARGE;
}

int getFallbackReaderFontIdForFamily(const InkMODSettings::FONT_FAMILY /*family*/) {
  // No built-in reading fonts ship with this firmware (Bitter/ChareInk/
  // LexendDeca were removed). This is the fallback used whenever no SD card
  // font is selected/found, so book text always has something to render.
  return UI_12_FONT_ID;
}

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(InkMODSettings& settings) {
  switch (static_cast<InkMODSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case InkMODSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = InkMODSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = InkMODSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = InkMODSettings::FRONT_HW_BACK;
      settings.frontButtonRight = InkMODSettings::FRONT_HW_CONFIRM;
      break;
    case InkMODSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = InkMODSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = InkMODSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = InkMODSettings::FRONT_HW_BACK;
      settings.frontButtonRight = InkMODSettings::FRONT_HW_RIGHT;
      break;
    case InkMODSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = InkMODSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = InkMODSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = InkMODSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = InkMODSettings::FRONT_HW_LEFT;
      break;
    case InkMODSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = InkMODSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = InkMODSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = InkMODSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = InkMODSettings::FRONT_HW_RIGHT;
      break;
  }
}

}  // namespace

const char* InkMODSettings::getDefaultDeviceName() {
  if (gpio.deviceIsX3()) return "inkMOD X3";
  if (gpio.deviceIsX4()) return "inkMOD X4";
  return "inkMOD";
}

const char* InkMODSettings::getEffectiveDeviceName() const {
  return isValidDeviceName(deviceName) ? deviceName : getDefaultDeviceName();
}

void InkMODSettings::validateFrontButtonMapping(InkMODSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

void InkMODSettings::validateReaderFrontButtonMapping(InkMODSettings& settings) {
  const uint8_t mapping[] = {settings.readerFrontButtonBack, settings.readerFrontButtonConfirm,
                             settings.readerFrontButtonLeft, settings.readerFrontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.readerFrontButtonBack = FRONT_HW_BACK;
        settings.readerFrontButtonConfirm = FRONT_HW_CONFIRM;
        settings.readerFrontButtonLeft = FRONT_HW_LEFT;
        settings.readerFrontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

uint8_t InkMODSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

uint8_t InkMODSettings::sleepScreenStorageToMode(const uint8_t storedValue) {
  if (storedValue < SLEEP_SCREEN_STORAGE_ORDER_COUNT) {
    return SLEEP_SCREEN_STORAGE_ORDER[storedValue];
  }
  return DARK;
}

uint8_t InkMODSettings::sleepScreenModeToStorage(const uint8_t mode) {
  for (uint8_t storedValue = 0; storedValue < SLEEP_SCREEN_STORAGE_ORDER_COUNT; storedValue++) {
    if (SLEEP_SCREEN_STORAGE_ORDER[storedValue] == mode) {
      return storedValue;
    }
  }
  return 0;
}

uint8_t InkMODSettings::legacyLineSpacingToPercent(const uint8_t legacyValue, const uint8_t fontFamily,
                                                       const bool sdFontSelected) {
  if (sdFontSelected) {
    switch (legacyValue) {
      case TIGHT:
        return 95;
      case WIDE:
        return 110;
      case NORMAL:
      default:
        return 100;
    }
  }

  switch (fontFamily) {
    case CHAREINK:
    case BITTER:
      switch (legacyValue) {
        case TIGHT:
          return 95;
        case WIDE:
          return 130;
        case NORMAL:
        default:
          return 110;
      }
    case LEXENDDECA:
    default:
      switch (legacyValue) {
        case TIGHT:
          return 90;
        case WIDE:
          return 120;
        case NORMAL:
        default:
          return 100;
      }
  }
}

uint8_t InkMODSettings::clampedLineHeightPercent(const uint8_t value) {
  if (value < MIN_LINE_HEIGHT_PERCENT) return MIN_LINE_HEIGHT_PERCENT;
  if (value > MAX_LINE_HEIGHT_PERCENT) return MAX_LINE_HEIGHT_PERCENT;
  return value;
}

uint8_t InkMODSettings::readingIdleTimeThresholdUnitsForSeconds(const uint16_t seconds) {
  const uint16_t clampedSeconds =
      std::clamp(seconds, MIN_READING_IDLE_TIME_THRESHOLD_SECONDS, MAX_READING_IDLE_TIME_THRESHOLD_SECONDS);
  return static_cast<uint8_t>((clampedSeconds + READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS - 1) /
                              READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS);
}

uint16_t InkMODSettings::readingIdleTimeThresholdSecondsForUnits(const uint8_t units) {
  const uint8_t clampedUnits =
      std::clamp(units, MIN_READING_IDLE_TIME_THRESHOLD_UNITS, MAX_READING_IDLE_TIME_THRESHOLD_UNITS);
  return static_cast<uint16_t>(clampedUnits) * READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS;
}

uint16_t InkMODSettings::getReadingIdleTimeThresholdSeconds() const {
  return readingIdleTimeThresholdSecondsForUnits(readingIdleTimeThresholdUnits);
}

bool InkMODSettings::saveToFile() const {
  Storage.mkdir("/.inkmod");
  return JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON);
}

bool InkMODSettings::loadFromFile() {
  enum class JsonLoadStatus : uint8_t { MissingOrEmpty, Loaded, Failed };

  auto loadJsonSettings = [this](const char* path, bool migrateToCurrentPath) -> JsonLoadStatus {
    if (!Storage.exists(path)) return JsonLoadStatus::MissingOrEmpty;

    String json = Storage.readFile(path);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = JsonSettingsIO::loadSettings(*this, json.c_str(), &resave);
      // Older settings files may explicitly store an empty SD font selection.
      // This firmware has no built-in reader family, so migrate that state to
      // the supplied default instead of falling back to the compact UI font.
      if (result && sdFontFamilyName[0] == '\0') {
        strncpy(sdFontFamilyName, "Bookerly", sizeof(sdFontFamilyName) - 1);
        sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
        resave = true;
      }
      if (result && (resave || migrateToCurrentPath)) {
        if (saveToFile()) {
          LOG_DBG("CPS", migrateToCurrentPath ? "Migrated legacy settings.json to inkmod-settings.json"
                                              : "Resaved settings to update format");
        } else {
          LOG_ERR("CPS", migrateToCurrentPath ? "Failed to save migrated settings to inkmod-settings.json"
                                              : "Failed to resave settings after format update");
        }
      }
      migrateLanguageBinaryFile();
      return result ? JsonLoadStatus::Loaded : JsonLoadStatus::Failed;
    }
    return JsonLoadStatus::MissingOrEmpty;
  };

  // Prefer inkMOD's namespaced settings file. Use the old generic file only
  // as a migration fallback so other firmware can keep its own settings.json.
  JsonLoadStatus jsonStatus = loadJsonSettings(SETTINGS_FILE_JSON, false);
  if (jsonStatus != JsonLoadStatus::MissingOrEmpty) return jsonStatus == JsonLoadStatus::Loaded;

  jsonStatus = loadJsonSettings(LEGACY_SETTINGS_FILE_JSON, true);
  if (jsonStatus != JsonLoadStatus::MissingOrEmpty) return jsonStatus == JsonLoadStatus::Loaded;

  // Fall back to binary migration
  if (Storage.exists(SETTINGS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      migrateLanguageBinaryFile();
      if (saveToFile()) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
        LOG_DBG("CPS", "Migrated settings.bin to inkmod-settings.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save migrated settings to JSON");
        return false;
      }
    }
  }

  // No settings files at all -- check for standalone language.bin
  return migrateLanguageBinaryFile();
}

bool InkMODSettings::migrateLanguageBinaryFile() {
  // V1_LANGUAGES / V1_LANGUAGE_COUNT are emitted by gen_i18n.py with the
  // frozen enum order from 2f969a9.
  if (!Storage.exists(LANG_FILE_BIN)) return false;

  FsFile f;
  if (Storage.openFileForRead("CPS", LANG_FILE_BIN, f)) {
    uint8_t version;
    serialization::readPod(f, version);
    if (version == 1) {
      uint8_t oldIndex;
      serialization::readPod(f, oldIndex);
      if (oldIndex < V1_LANGUAGE_COUNT) {
        language = static_cast<uint8_t>(V1_LANGUAGES[oldIndex]);
      }
    }
  }
  Storage.rename(LANG_FILE_BIN, LANG_FILE_BAK);
  saveToFile();
  LOG_DBG("CPS", "Migrated language.bin into inkmod-settings.json");
  return true;
}

bool InkMODSettings::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  uint8_t settingsRead = 0;
  bool frontButtonMappingRead = false;
  do {
    uint8_t storedSleepScreen = sleepScreenModeToStorage(sleepScreen);
    readAndValidate(inputFile, storedSleepScreen, SLEEP_SCREEN_STORAGE_ORDER_COUNT);
    sleepScreen = sleepScreenStorageToMode(storedSleepScreen);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      uint8_t legacyFontFamily;
      serialization::readPod(inputFile, legacyFontFamily);
      if (legacyFontFamily < BUILTIN_FONT_COUNT) {
        fontFamily = legacyFontFamily;
      }
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontSize, getActiveReaderFontSizeCount());
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    uint8_t legacySleepTimeout = SLEEP_10_MIN;
    readAndValidate(inputFile, legacySleepTimeout, SLEEP_TIMEOUT_COUNT);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacySleepTimeout);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    screenMargin = std::clamp<uint8_t>(screenMargin, 5, 40);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, longPressButtonBehavior, LONG_PRESS_BUTTON_BEHAVIOR_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      // Older builds wrote uiTheme via raw readPod, so any byte (including
      // values that were briefly assigned to themes that are not currently
      // exposed) may be on disk. Map anything outside the active theme count
      // to LYRA so the migration is deterministic instead of leaning on
      // readAndValidate's no-op-on-invalid behaviour.
      uint8_t rawTheme = LYRA;
      serialization::readPod(inputFile, rawTheme);
      uiTheme = (rawTheme < UI_THEME_COUNT) ? rawTheme : static_cast<uint8_t>(LYRA);
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    frontButtonMappingRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, embeddedStyle);
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  if (frontButtonMappingRead) {
    InkMODSettings::validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

  lineHeightPercent = legacyLineSpacingToPercent(lineSpacing, fontFamily, sdFontFamilyName[0] != '\0');

  LOG_DBG("CPS", "Settings loaded from binary file");
  return true;
}

float InkMODSettings::getReaderLineCompression() const {
  return static_cast<float>(clampedLineHeightPercent(lineHeightPercent)) / 100.0f;
}

unsigned long InkMODSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

#ifdef SIMULATOR
bool InkMODSettings::verifySleepTimeoutMigrationContract() {
  InkMODSettings& settings = getInstance();
  const uint8_t originalMinutes = settings.sleepTimeoutMinutes;

  settings.sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(SLEEP_5_MIN);
  const bool migratedValueDrivesTimeout = settings.getSleepTimeoutMs() == 5UL * 60UL * 1000UL;

  settings.sleepTimeoutMinutes = 12;
  const bool runtimeUsesMinutesOnly = settings.getSleepTimeoutMs() == 12UL * 60UL * 1000UL;

  settings.sleepTimeoutMinutes = originalMinutes;
  return migratedValueDrivesTimeout && runtimeUsesMinutesOnly;
}

bool InkMODSettings::verifySleepScreenMigrationContract() {
  constexpr uint8_t legacyModeCountBeforeMinimal = 8;
  constexpr uint8_t minimalSleepStorageValue = 8;
  constexpr uint8_t quickResumeStorageValue = 9;
  constexpr uint8_t minimalStatsStorageValue = 10;
  for (uint8_t storedValue = 0; storedValue < legacyModeCountBeforeMinimal; storedValue++) {
    if (sleepScreenStorageToMode(storedValue) != storedValue) {
      return false;
    }
  }

  return sleepScreenStorageToMode(minimalSleepStorageValue) == MINIMAL_SLEEP &&
         sleepScreenModeToStorage(MINIMAL_SLEEP) == minimalSleepStorageValue &&
         sleepScreenStorageToMode(quickResumeStorageValue) == QUICK_RESUME &&
         sleepScreenModeToStorage(QUICK_RESUME) == quickResumeStorageValue &&
         sleepScreenStorageToMode(minimalStatsStorageValue) == MINIMAL_STATS_SLEEP &&
         sleepScreenModeToStorage(MINIMAL_STATS_SLEEP) == minimalStatsStorageValue &&
         sleepScreenStorageToMode(UINT8_MAX) == DARK;
}
#endif

int InkMODSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

uint8_t InkMODSettings::getActiveReaderFontSizeCount() {
  return static_cast<uint8_t>(std::count_if(std::begin(READER_FONT_SIZE_STORAGE_ORDER),
                                            std::end(READER_FONT_SIZE_STORAGE_ORDER),
                                            [](const FONT_SIZE size) { return isReaderFontSizeAvailable(size); }));
}

uint8_t InkMODSettings::getStoredReaderFontSize(const FONT_SIZE size) {
  uint8_t stored = 0;
  for (const FONT_SIZE activeSize : READER_FONT_SIZE_STORAGE_ORDER) {
    if (!isReaderFontSizeAvailable(activeSize)) continue;
    if (size == activeSize) return stored;
    stored++;
  }
  return INVALID_READER_FONT_SIZE;
}

uint8_t InkMODSettings::getReaderFontPointSize(const FONT_SIZE size) {
  switch (size) {
    case TEENSY:
      return 8;
    case ITTY_BITTY:
      return 9;
    case TINY:
      return 10;
    case SMALL:
      return 12;
    case MEDIUM:
    default:
      return 14;
    case LARGE:
      return 16;
    case EXTRA_LARGE:
      return 18;
    case HUGE_SIZE:
      return 20;
  }
}

uint8_t InkMODSettings::getSdFontRangePointSize(uint8_t range, uint8_t step) {
  range = normalizedSdFontRange(range);
  const uint8_t stepCount = SD_FONT_RANGE_STEP_COUNTS[range];
  if (step >= stepCount) step = stepCount - 1;
  return SD_FONT_RANGE_POINT_SIZES[range][step];
}

bool InkMODSettings::isSdFontPointSizeAllowedForRange(const uint8_t pointSize, const uint8_t range) {
  const uint8_t normalizedRange = normalizedSdFontRange(range);
  const uint8_t stepCount = SD_FONT_RANGE_STEP_COUNTS[normalizedRange];
  for (uint8_t i = 0; i < stepCount; i++) {
    if (SD_FONT_RANGE_POINT_SIZES[normalizedRange][i] == pointSize) return true;
  }
  return false;
}

InkMODSettings::FONT_SIZE InkMODSettings::getEffectiveReaderFontSize() const {
  uint8_t stored = 0;
  for (const FONT_SIZE size : READER_FONT_SIZE_STORAGE_ORDER) {
    if (!isReaderFontSizeAvailable(size)) continue;
    if (fontSize == stored) return size;
    stored++;
  }
  return firstAvailableReaderFontSize();
}

uint8_t InkMODSettings::getSdFontTargetPointSize() const {
  return getSdFontRangePointSize(sdFontSizeRange, fontSize);
}

bool InkMODSettings::changeReaderFontSize(const bool larger) {
  const FONT_SIZE currentSize = getEffectiveReaderFontSize();
  int currentIndex = 0;
  constexpr size_t sizeCount = sizeof(READER_FONT_SIZE_CYCLE_ORDER) / sizeof(READER_FONT_SIZE_CYCLE_ORDER[0]);
  for (size_t i = 0; i < sizeCount; i++) {
    if (READER_FONT_SIZE_CYCLE_ORDER[i] == currentSize) {
      currentIndex = static_cast<int>(i);
      break;
    }
  }

  for (size_t step = 1; step < sizeCount; step++) {
    const int direction = larger ? 1 : -1;
    const size_t nextIndex =
        (currentIndex + direction * static_cast<int>(step) + static_cast<int>(sizeCount)) % sizeCount;
    const uint8_t stored = getStoredReaderFontSize(READER_FONT_SIZE_CYCLE_ORDER[nextIndex]);
    if (stored != INVALID_READER_FONT_SIZE) {
      fontSize = stored;
      return true;
    }
  }
  return false;
}

int InkMODSettings::getReaderFontId() const {
  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  return getBuiltInReaderFontId();
}

int InkMODSettings::getBuiltInReaderFontId() const {
  // No built-in reading fonts ship with this firmware (Bitter/ChareInk/
  // LexendDeca were removed; reading fonts now come only from the SD card).
  // `fontFamily` is kept solely so old settings files still parse correctly.
  return getFallbackReaderFontIdForFamily(static_cast<FONT_FAMILY>(fontFamily));
}
