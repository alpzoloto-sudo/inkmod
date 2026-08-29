#include <Arduino.h>
#include <BootLog.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <MemoryBudget.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>
#include <ctime>
#include <new>

#ifdef SIMULATOR
using esp_reset_reason_t = int;
using esp_sleep_wakeup_cause_t = int;
enum : int {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
  ESP_RST_USB,
  ESP_RST_JTAG,
  ESP_RST_EFUSE,
  ESP_RST_PWR_GLITCH,
  ESP_RST_CPU_LOCKUP
};
enum : int {
  ESP_SLEEP_WAKEUP_UNDEFINED = 0,
  ESP_SLEEP_WAKEUP_ALL,
  ESP_SLEEP_WAKEUP_EXT0,
  ESP_SLEEP_WAKEUP_EXT1,
  ESP_SLEEP_WAKEUP_TIMER,
  ESP_SLEEP_WAKEUP_TOUCHPAD,
  ESP_SLEEP_WAKEUP_ULP,
  ESP_SLEEP_WAKEUP_GPIO,
  ESP_SLEEP_WAKEUP_UART,
  ESP_SLEEP_WAKEUP_WIFI,
  ESP_SLEEP_WAKEUP_COCPU,
  ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG,
  ESP_SLEEP_WAKEUP_BT
};
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_UNKNOWN; }
inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_UNDEFINED; }
#else
#include <esp_sleep.h>
#include <esp_system.h>
#endif

#include <algorithm>
#include <cstring>

#include "AppVersion.h"
#include "InkMODSettings.h"
#include "InkMODState.h"
#include "GlobalActions.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "activities/reader/StatsBackup.h"
#include "activities/settings/KOReaderSettingsActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#ifndef SIMULATOR
#include "network/FirmwareFlasher.h"
#endif
#include "components/UITheme.h"
#include "fontIds.h"
#include "UiTextSize.h"
#ifdef SIMULATOR
#include "simulator/SimulatorSmokeTest.h"
#endif
#include "SdCardFontSystem.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Fonts
// Only the system UI font (Inter) is built in; it is used for both the
// interface and book text.
EpdFont smallFont(&inter_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&inter_10_regular);
EpdFont ui10BoldFont(&inter_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&inter_12_regular);
EpdFont ui12BoldFont(&inter_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

EpdFont ui14RegularFont(&inter_14_regular);
EpdFont ui14BoldFont(&inter_14_bold);
EpdFontFamily ui14FontFamily(&ui14RegularFont, &ui14BoldFont);

EpdFont ipaFallback12Font(&inter_12_ipa_fallback);
EpdFontFamily ipaFallback12Family(&ipaFallback12Font);

EpdFont ipaFallback14Font(&inter_14_ipa_fallback);
EpdFontFamily ipaFallback14Family(&ipaFallback14Font);

EpdFont testFonts12Regular(&dejavu_sans_12_regular);
EpdFont testFonts12Bold(&dejavu_sans_12_bold);
EpdFont testFonts12Italic(&dejavu_sans_12_italic);
EpdFont testFonts12BoldItalic(&dejavu_sans_12_bolditalic);
EpdFontFamily testFonts12Family(&testFonts12Regular, &testFonts12Bold, &testFonts12Italic, &testFonts12BoldItalic);

// See UiTextSize.h. Only UI_10_FONT_ID (menu/list body text) grows; two roles
// are deliberately left untouched:
//  - UI_12_FONT_ID has no larger built-in size to grow into.
//  - SMALL_FONT_ID backs the status bar clock and header title, which are
//    vertically centered inside a FIXED-height bar via
//    "(statusBarHeight - lineHeight) / 2" (see BaseTheme::drawTopStatusBarClock).
//    That bar's height doesn't grow with the font, so a taller SMALL_FONT_ID
//    pushes the centered text above the bar - visibly off the top of the
//    screen. Fixing that properly means growing statusBarHeight itself
//    (and everything positioned relative to it) per theme, which needs
//    on-device verification this build can't do; leaving SMALL_FONT_ID alone
//    avoids the regression entirely.
//
// UI_10_FONT_ID's LARGE size is theme-dependent: BaseTheme::drawList() draws
// each row's text flush with the top of a FIXED-height row (no vertical
// centering), so a font taller than the row just crowds into the row below
// instead of vanishing - less severe than the status-bar case, but still not
// something to ship blind everywhere. Measured line heights (EpdFontData's
// advanceY) vs. each theme's ThemeMetrics::listRowHeight:
//   Inter 10pt ~26px / Inter 12pt ~30px / LexendDeca 14pt ~36px
//   BaseMetrics (Classic/Dashboard/Minimal): listRowHeight = 30  -> no room past 12pt
//   LyraCarouselTheme: listRowHeight = 35 / LyraTheme: 36        -> no clean room past 12pt
//   RoundedRaffTheme: listRowHeight = 42                          -> fits LexendDeca 14pt with margin
// So only RoundedRaff gets the bigger LexendDeca bump for now; everything
// else keeps the previously-verified Inter 12pt bump. If you confirm on a
// device that another theme's rows have room too, add it to the switch below.
void applyUiTextSize(GfxRenderer& r) {
  if (SETTINGS.uiTextSize == InkMODSettings::UI_TEXT_SIZE_LARGE) {
    // NOTE: RoundedRaff used to get a bigger LexendDeca 14pt bump here instead of the
    // Inter 12pt one every other theme uses (its taller list rows have room for it - see
    // the metrics comment above). Reverted to Inter 12pt for all themes: the checked-in
    // lexenddeca_14_bold.h/lexenddeca_14_regular.h are missing several Cyrillic glyphs
    // (и, ш, л, е, я and others - confirmed against the font's own additional-intervals
    // fallback list in lib/EpdFont/scripts/convert-builtin-fonts.sh) that a ChareInk7
    // fallback was meant to supply at generation time but evidently didn't end up in
    // these particular committed headers, so Cyrillic UI text renders with missing-glyph
    // boxes for those letters. Re-enable the RoundedRaff branch once lexenddeca_14_*.h
    // are regenerated (needs the LexendDeca + ChareInk7 source fonts, not present in
    // this checkout) and verified to have full Cyrillic coverage.
    r.replaceFont(UI_10_FONT_ID, ui12FontFamily);
  } else {
    r.replaceFont(UI_10_FONT_ID, ui10FontFamily);
  }
}

int uiControlFontId() {
  return SETTINGS.uiTextSize == InkMODSettings::UI_TEXT_SIZE_LARGE ? UI_14_FONT_ID : UI_12_FONT_ID;
}

int uiHintFontId() {
  return SETTINGS.uiTextSize == InkMODSettings::UI_TEXT_SIZE_LARGE ? UI_10_FONT_ID : SMALL_FONT_ID;
}

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Set when the screenshot combo (Power + Volume Down) fires, so the subsequent
// power button release does not also trigger a short-press action (e.g. sleep).
static bool screenshotComboHandled = false;

const char* resetReasonName(const esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "EFUSE";
    case ESP_RST_PWR_GLITCH:
      return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP:
      return "CPU_LOCKUP";
    case ESP_RST_UNKNOWN:
    default:
      return "UNKNOWN";
  }
}

const char* wakeupCauseName(const esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "UNDEFINED";
    case ESP_SLEEP_WAKEUP_ALL:
      return "ALL";
    case ESP_SLEEP_WAKEUP_EXT0:
      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "GPIO";
    case ESP_SLEEP_WAKEUP_UART:
      return "UART";
    case ESP_SLEEP_WAKEUP_WIFI:
      return "WIFI";
    case ESP_SLEEP_WAKEUP_COCPU:
      return "COCPU";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
      return "COCPU_TRAP";
    case ESP_SLEEP_WAKEUP_BT:
      return "BT";
    default:
      return "UNKNOWN";
  }
}

const char* wakeupRouteName(const HalGPIO::WakeupReason reason) {
  switch (reason) {
    case HalGPIO::WakeupReason::PowerButton:
      return "PowerButton";
    case HalGPIO::WakeupReason::AfterFlash:
      return "AfterFlash";
    case HalGPIO::WakeupReason::AfterUSBPower:
      return "AfterUSBPower";
    case HalGPIO::WakeupReason::Other:
    default:
      return "Other";
  }
}

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

struct EmergencyFirmwareFile {
  const char* path;
  const char* appliedPath;
  const char* incompatiblePath;
  const char* failedPath;
};

// Both names are supported as headless, power-on recovery triggers. Keep the
// inkMOD-specific name first so an explicitly prepared recovery image wins if
// both files happen to be present on the same SD card.
static constexpr EmergencyFirmwareFile EMERGENCY_FIRMWARE_FILES[] = {
    {"/inkmod-recovery.bin", "/inkmod-recovery.applied.bin", "/inkmod-recovery.incompatible.bin",
     "/inkmod-recovery.failed.bin"},
    {"/update.bin", "/update.applied.bin", "/update.incompatible.bin", "/update.failed.bin"},
};

static bool renameEmergencyFirmware(const char* source, const char* destination) {
  Storage.remove(destination);
  if (Storage.rename(source, destination)) return true;
  LOG_ERR("FW", "Could not rename recovery trigger %s -> %s", source, destination);
  return false;
}

static void tryEmergencyFirmwareUpdate() {
#ifndef SIMULATOR
  for (const auto& fw : EMERGENCY_FIRMWARE_FILES) {
    if (!Storage.exists(fw.path)) continue;

    LOG_INF("FW", "Emergency SD firmware detected: %s", fw.path);
    const auto result = firmware_flash::flashFromSdPath(fw.path, nullptr, nullptr);
    if (result != firmware_flash::Result::OK) {
      LOG_ERR("FW", "Emergency SD firmware failed: %s (%s)", fw.path, firmware_flash::resultName(result));

      // A C3/S3 mismatch is not a broken image: mark it explicitly so the user
      // can see the reason directly on the SD card and so boot does not retry it
      // forever. Most importantly, FirmwareFlasher returns this result before
      // any erase/write operation touches the OTA partition.
      if (result == firmware_flash::Result::INCOMPATIBLE_CHIP) {
        if (renameEmergencyFirmware(fw.path, fw.incompatiblePath)) {
          LOG_ERR("FW", "Incompatible recovery firmware renamed to %s", fw.incompatiblePath);
        }
        return;
      }

      // Preserve the existing behavior for corrupt/truncated/I/O-failed files.
      // If rename itself fails, remove the trigger as the old implementation did
      // to prevent an endless failing recovery loop.
      if (!renameEmergencyFirmware(fw.path, fw.failedPath)) {
        Storage.remove(fw.path);
      }
      return;
    }

    bool disarmed = renameEmergencyFirmware(fw.path, fw.appliedPath);
    if (!disarmed) {
      disarmed = Storage.remove(fw.path);
    }
    if (!disarmed) {
      LOG_ERR("FW", "Emergency firmware applied but trigger could not be disarmed; refusing auto-restart");
      return;
    }

    LOG_INF("FW", "Emergency SD firmware applied from %s; restarting", fw.path);
    delay(500);
    ESP.restart();
    return;
  }
#endif
}

static constexpr char EMERGENCY_FIRMWARE_PATH[] = "/inkmod-recovery.bin";
static constexpr char EMERGENCY_FIRMWARE_APPLIED_PATH[] = "/inkmod-recovery.applied.bin";
static constexpr char EMERGENCY_FIRMWARE_FAILED_PATH[] = "/inkmod-recovery.failed.bin";

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

bool isGlobalPowerButtonAction(const InkMODSettings::SHORT_PWRBTN action) {
  return isPowerButtonActionAvailableOutsideReader(action);
}

// UNUSED as of the SYNC_PROGRESS removal from the power-button action lists
// (see SettingsList.h and the SYNC_PROGRESS case in
// handleGlobalPowerButtonAction() below) - kept only in case a
// reader-independent sync entry point is worth revisiting later. Do not wire
// this back up to a button without also re-solving why it kept hanging: it
// went through peak-memory OOM, then a RenderLock deadlock, then a
// render-task SD-card race, each fixed in turn and each still followed by a
// hang on real hardware. The safe, working equivalent is
// EpubReaderMenuActivity::MenuAction::SYNC, reached only from inside the
// reader, which reuses the already-loaded Epub instead of loading a second
// one and never leaves the reader running in the background while it does.
bool startGlobalSyncProgress() {
  // If a reader is currently open (e.g. this was triggered by a long-press
  // while reading, not from the in-reader menu), its decoded Section is
  // still fully resident at this point. Free it now, before loading a second
  // independent Epub for potentially the same file below - otherwise both
  // the reader's full working set AND this fresh load compete for RAM at
  // once, which is exactly the peak that was crashing/hanging the device.
  // (The in-reader "Sync" menu item never hit this because it reuses the
  // already-loaded Epub and frees its own Section itself before proceeding.)
  activityManager.releaseCurrentActivityHeavyResources();

  if (!KOREADER_STORE.hasCredentials()) {
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  const std::string epubPath = APP_STATE.openEpubPath;
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath) || !Storage.exists(epubPath.c_str())) {
    LOG_DBG("MAIN", "No syncable EPUB open, opening KOReader settings instead");
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  auto epub = std::make_shared<Epub>(epubPath, "/.inkmod");
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_ERR("MAIN", "Failed to load EPUB for global sync: %s", epubPath.c_str());
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  epub->setupCacheDir();

  int spineIndex = 0;
  int pageNumber = 0;
  int totalPagesInSpine = 1;
  EpubReaderUtils::Progress progress;
  if (EpubReaderUtils::loadProgress(*epub, progress, "MAIN")) {
    spineIndex = progress.spineIndex;
    pageNumber = progress.pageNumber;
    if (progress.hasPageCount) {
      totalPagesInSpine = std::max(1, progress.pageCount);
    }
  }

  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    spineIndex = 0;
  }

  InkMODPosition localPos = {spineIndex, pageNumber, totalPagesInSpine};
  KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(spineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";

  activityManager.pushActivity(
      std::make_unique<KOReaderSyncActivity>(renderer, mappedInputManager, epubPath, spineIndex, pageNumber,
                                             totalPagesInSpine, std::move(localKoPos), std::move(localChapterName)));
  return true;
}

InkMODSettings::SHORT_PWRBTN getPowerButtonAction() {
  static bool longPowerButtonHandled = false;

  if (mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    if (longPowerButtonHandled) {
      longPowerButtonHandled = false;
      screenshotComboHandled = false;
      return InkMODSettings::SHORT_PWRBTN::IGNORE;
    }

    if (screenshotComboHandled) {
      screenshotComboHandled = false;
      return InkMODSettings::SHORT_PWRBTN::IGNORE;
    }

    return mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()
               ? static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)
               : static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  }

  if (longPowerButtonHandled || !mappedInputManager.isPressed(MappedInputManager::Button::Power) ||
      mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return InkMODSettings::SHORT_PWRBTN::IGNORE;
  }

  const auto action = static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  if (!isGlobalPowerButtonAction(action)) {
    return InkMODSettings::SHORT_PWRBTN::IGNORE;
  }

  longPowerButtonHandled = true;
  return action;
}

bool handleGlobalPowerButtonAction(const InkMODSettings::SHORT_PWRBTN action) {
  switch (action) {
    case InkMODSettings::SHORT_PWRBTN::SLEEP:
      enterDeepSleep();
      return true;
    case InkMODSettings::SHORT_PWRBTN::QUICK_RESUME_SLEEP:
      enterDeepSleep(true);
      return true;
    case InkMODSettings::SHORT_PWRBTN::FORCE_REFRESH: {
      LOG_DBG("MAIN", "Manual screen refresh triggered");
      const bool repaintReaderGrayscale = activityManager.isCurrentReaderActivity();
      {
        RenderLock lock;
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      // HALF_REFRESH sends the current 1-bit framebuffer and clears ghosting,
      // but it does not rebuild the reader's later grayscale/AA passes. Ask
      // only an actually visible reader to run its normal render pipeline
      // once more. Menus and the file browser keep their old one-pass action.
      if (repaintReaderGrayscale) {
        activityManager.requestUpdate();
      }
      return true;
    }
    case InkMODSettings::SHORT_PWRBTN::SCREENSHOT: {
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    }
    case InkMODSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      // No longer assignable from Settings (see SettingsList.h) after
      // repeated hangs on this path despite several attempted fixes
      // (peak-memory OOM, then a RenderLock deadlock, then a render-task
      // race - all specific to reaching sync from *outside* the reader).
      // A pre-existing saved setting could still carry this raw value, so
      // handle it explicitly rather than relying on it simply not being
      // selectable going forward: do nothing, same as IGNORE. The identical
      // sync action remains available and reliable from the in-reader menu
      // (EpubReaderMenuActivity::MenuAction::SYNC), which reuses the
      // already-loaded Epub instead of loading a second one.
      return false;
    case InkMODSettings::SHORT_PWRBTN::FILE_TRANSFER:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToFileTransfer();
      return true;
    case InkMODSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToCalibreWireless();
      return true;
    case InkMODSettings::SHORT_PWRBTN::JOIN_NETWORK:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToJoinNetworkFileTransfer();
      return true;
    case InkMODSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToHotspotFileTransfer();
      return true;
    default:
      return false;
  }
}

namespace {
constexpr uint16_t POST_SLEEP_SCREEN_SETTLE_MS = 500;
constexpr uint8_t TILT_SLEEP_MAX_ATTEMPTS = 3;
constexpr uint16_t TILT_SLEEP_RETRY_DELAY_MS = 10;

void putTiltSensorToSleepForDeepSleep() {
  if (!halTiltSensor.isAvailable()) {
    return;
  }

  for (uint8_t attempt = 0; attempt < TILT_SLEEP_MAX_ATTEMPTS; ++attempt) {
    if (halTiltSensor.deepSleep()) {
      return;
    }
    delay(TILT_SLEEP_RETRY_DELAY_MS);
  }
  LOG_ERR("MAIN", "Tilt sensor did not confirm sleep before deep sleep");
}
}  // namespace

constexpr char SLEEP_FRAME_FILE[] = "/.inkmod/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == InkMODSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == InkMODSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Any automatic timeout sleep should wake like a real resume, not a cold boot.
  // The selected timeout sleep screen is still rendered; we simply preserve it
  // and skip the inkMOD splash on wake. Manual/power-button sleep keeps the splash.
  const bool resumeWithoutSplash = fromTimeout || isQuickResumeSleep;
  APP_STATE.showBootScreen = !resumeWithoutSplash;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (resumeWithoutSplash) {
    saveSleepFrameBuffer();
  } else {
    delay(POST_SLEEP_SCREEN_SETTLE_MS);
  }

  if (gpio.deviceIsX3() && SETTINGS.autoBackupStats != 0) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now) && !backupGlobalStats(false)) {
      LOG_ERR("MAIN", "Automatic reading-stats backup failed before deep sleep");
    }
  }

  putTiltSensorToSleepForDeepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  BootLog::step("MAIN", "setupDisplayAndFonts: calling display.begin()");
#ifdef SIMULATOR
  (void)seamless;
  display.begin();
#else
  display.begin(seamless);
#endif
  BootLog::step("MAIN", "setupDisplayAndFonts: display.begin() returned");
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");
  BootLog::step("MAIN", "renderer/activityManager begin() returned");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);

  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(UI_14_FONT_ID, ui14FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  renderer.insertFont(IPA_FALLBACK_12_FONT_ID, ipaFallback12Family);
  renderer.insertFont(IPA_FALLBACK_14_FONT_ID, ipaFallback14Family);
  renderer.insertFont(TEST_FONTS_12_FONT_ID, testFonts12Family);

  // Missing glyphs in a user reading font are drawn from the IPA-capable
  // built-in Inter fallback. Normal glyphs stay in the selected book font.
  renderer.setMissingGlyphFallbackFonts(IPA_FALLBACK_12_FONT_ID, IPA_FALLBACK_14_FONT_ID);

  applyUiTextSize(renderer);

  // Discover and load SD card fonts (font-pack removed, so this will find none,
  // but other activities still depend on sdFontSystem being initialized).
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

// Devices without a battery-backed RTC (X4) lose track of time on every real power
// loss. If we don't already have a valid time (i.e. this is a cold boot, not a
// resume from deep sleep, where the ESP32's internal clock is preserved), briefly
// and silently join the last-used saved WiFi network, sync via NTP, then disconnect
// again. No UI is shown for this - it either succeeds quietly or is skipped/fails
// quietly, and reading/browsing continues normally either way.
//
// Split in two: checkSilentBootTimeSyncCandidate() only touches the SD card, so it
// must run before the display/SPI bus is initialized. attemptSilentBootTimeSync()
// only touches WiFi/NTP (no SD access), so it's safe to run afterwards, behind the
// splash screen, without racing the shared SPI bus.
struct BootTimeSyncCandidate {
  bool shouldAttempt = false;
  std::string ssid;
  std::string password;
};

BootTimeSyncCandidate checkSilentBootTimeSyncCandidate() {
  BootTimeSyncCandidate candidate;

  if (SETTINGS.clockDisabled) {
    return candidate;  // User turned the clock off (X4) - don't join WiFi just to sync it.
  }

  if (!halClock.needsPeriodicNTPSync()) {
    return candidate;  // X3 has a battery-backed RTC; nothing to do here.
  }

  uint8_t hour, minute;
  if (halClock.getTime(hour, minute)) {
    return candidate;  // Already have a valid time (e.g. woke from deep sleep) - nothing to do.
  }

  WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_DBG("CLK", "No saved WiFi network - skipping silent boot-time sync");
    return candidate;
  }
  const auto* cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_DBG("CLK", "Saved network '%s' has no stored credential - skipping silent boot-time sync",
            lastSsid.c_str());
    return candidate;
  }

  candidate.shouldAttempt = true;
  candidate.ssid = cred->ssid;
  candidate.password = cred->password;
  return candidate;
}

// Best-effort immediate fallback for boots where checkSilentBootTimeSyncCandidate() found no
// valid time yet: seed the software clock from the last value a real NTP sync produced, so
// date/time-dependent UI (e.g. the Calendar sleep screen) has *something* correct-ish to show
// right away instead of nothing while attemptSilentBootTimeSync() below tries (and may fail,
// e.g. no WiFi in range) to get a fresh one. No-op on X3 (real battery-backed RTC) or if we've
// never synced before.
void seedClockFromLastKnownTime() {
  if (SETTINGS.clockDisabled || !halClock.needsPeriodicNTPSync()) return;
  uint8_t hour, minute;
  if (halClock.getTime(hour, minute)) return;  // already have a valid time this boot
  if (SETTINGS.clockLastSyncedEpoch == 0) return;  // never synced before - nothing to fall back to
  halClock.seedFallbackTime(static_cast<time_t>(SETTINGS.clockLastSyncedEpoch));
}

void attemptSilentBootTimeSync(const BootTimeSyncCandidate& candidate) {
  if (!candidate.shouldAttempt) {
    return;
  }

  LOG_INF("CLK", "No valid time on boot - briefly joining saved network '%s' to sync time",
          candidate.ssid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.begin(candidate.ssid.c_str(), candidate.password.c_str());

  constexpr unsigned long kConnectTimeoutMs = 6000;
  const unsigned long connectStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - connectStart) < kConnectTimeoutMs) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (halClock.syncFromNTP()) {
      // Display is already initialized at this point (SPI is shared with the SD card),
      // so this save needs the same render lock other post-boot SD writes use.
      RenderLock lock;
      SETTINGS.clockHasBeenSynced = 1;
      SETTINGS.clockDateHasBeenSynced = 1;
      SETTINGS.clockLastSyncedEpoch = static_cast<uint32_t>(time(nullptr));
      SETTINGS.saveToFile();
      LOG_INF("CLK", "Silent boot-time sync succeeded");
    } else {
      LOG_ERR("CLK", "Silent boot-time sync: NTP request failed");
    }
  } else {
    LOG_ERR("CLK", "Silent boot-time sync: could not reach saved network '%s'", candidate.ssid.c_str());
  }

  // This was only ever meant to be a brief dip to fetch the time, not a lasting
  // connection - always leave WiFi off again afterwards, success or failure.
  WiFi.disconnect(true);
  delay(30);
  WiFi.mode(WIFI_OFF);
}

// Called by every plain `new`/`std::vector`/`std::string` growth (anything not using
// `new (std::nothrow)`) when the allocator can't satisfy the request. Exceptions are
// disabled in this build (-fno-exceptions), so without this handler installed, a failed
// allocation anywhere in the app - not just the "big ticket" spots already guarded by
// MemoryBudget checks (image decoders, inline EPUB images) - silently aborts and resets
// the device with no diagnostic at all. This doesn't recover the allocation (there's
// nothing safe to free from here), but it gets the heap state into the serial log before
// the inevitable restart, so a low-memory crash is diagnosable from one log capture
// instead of hours of static code review.
void outOfMemoryHandler() {
  const auto heap = MemoryBudget::snapshot();
  char breadcrumb[48];
  snprintf(breadcrumb, sizeof(breadcrumb), "OOM free=%u max=%u", heap.freeHeap, heap.maxAllocHeap);
  HalSystem::recordBreadcrumb(breadcrumb);
  HalSystem::markDiagnosticReboot();
  LOG_ERR("MEM", "operator new failed - out of memory (free=%u maxAlloc=%u) - restarting", heap.freeHeap,
          heap.maxAllocHeap);
#ifdef ENABLE_SERIAL_LOG
  logSerial.flush();
#endif
  delay(50);
  esp_restart();
}

void setup() {
  t1 = millis();
  std::set_new_handler(outOfMemoryHandler);

  const esp_reset_reason_t rawResetReason = esp_reset_reason();
  const esp_sleep_wakeup_cause_t rawWakeupCause = esp_sleep_get_wakeup_cause();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
#ifndef SIMULATOR
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  LOG_INF("BOOT", "Reset diagnostic: reset=%d(%s) sleepWake=%d(%s)", static_cast<int>(rawResetReason),
          resetReasonName(rawResetReason), static_cast<int>(rawWakeupCause), wakeupCauseName(rawWakeupCause));

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
  LOG_INF("BOOT", "Post-GPIO diagnostic: device=%s usb=%d silentReboot=%d silentTarget=%lu",
          gpio.deviceIsX3() ? "X3" : "X4", gpio.isUsbConnected() ? 1 : 0, isSilentReboot ? 1 : 0,
          static_cast<unsigned long>(snapshotTarget));

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  // Headless emergency recovery: root-of-SD /inkmod-recovery.bin is validated
  // and flashed before display/UI initialization.
  tryEmergencyFirmwareUpdate();

  BootLog::begin();
  BootLog::stepf("MAIN", "SD mounted; device=%s usb=%d silentReboot=%d", gpio.deviceIsX3() ? "X3" : "X4",
                  gpio.isUsbConnected() ? 1 : 0, isSilentReboot ? 1 : 0);

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();
  powerManager.seedLastChargeEpochSeconds(APP_STATE.lastChargeEpochSeconds);
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  // SD-only check (see comment on the function): must happen before the display/SPI
  // bus is initialized below. The actual WiFi/NTP step runs later, behind the splash.
  const BootTimeSyncCandidate bootTimeSyncCandidate = checkSilentBootTimeSyncCandidate();
  seedClockFromLastKnownTime();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  // Check wake duration before the remaining file loads so the user does not
  // have to hold the power button across all of the SD reads below.
  const auto wakeupReason = gpio.getWakeupReason();
  LOG_INF("BOOT", "Wake route: %s", wakeupRouteName(wakeupReason));
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_INF("BOOT", "Power-button wake: verifying duration required=%u shortAllowed=%d",
              SETTINGS.getPowerButtonWakeDuration(), SETTINGS.shortPwrBtn == InkMODSettings::SHORT_PWRBTN::SLEEP);
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonWakeDuration(),
                                   SETTINGS.shortPwrBtn == InkMODSettings::SHORT_PWRBTN::SLEEP);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // TEMP: continue booting while diagnosing post-flash/reset behavior.
      // Normal behavior is to go back to sleep when USB power causes a cold boot.
      LOG_INF("BOOT", "AfterUSBPower route: TEMP continuing boot instead of deep sleep");
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
      LOG_INF("BOOT", "AfterFlash route: continuing boot");
      break;
    case HalGPIO::WakeupReason::Other:
    default:
      LOG_INF("BOOT", "Other wake route: continuing boot");
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting inkMOD version " INKMOD_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  BootLog::step("MAIN", "entering setupDisplayAndFonts() from setup()");
  setupDisplayAndFonts(resume != BootResume::Splash);
  BootLog::step("MAIN", "back from setupDisplayAndFonts() in setup() - display/fonts OK");

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Restore the pre-sleep reader framebuffer to the panel, but do not
        // draw any Quick Resume marker.  The refresh is required so the sleep
        // wallpaper is replaced immediately; omitting it leaves the sleep
        // image visible until a later unrelated redraw.
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // Placed after the splash/quick-resume screen is already painted, so a cold boot's
  // brief silent WiFi dip (only happens when there's no valid time yet - see the
  // function) happens behind a visible screen instead of a black/frozen one.
  attemptSilentBootTimeSync(bootTimeSyncCandidate);

  // Pre-warm WiFi into a stable STA/disconnected state here, in the quiet
  // early-boot window, rather than lazily inside
  // WifiSelectionActivity::attemptConnection() when the user actually
  // triggers a connection. Several boot-log captures now confirm
  // WiFi.mode(WIFI_STA)/WiFi.persistent(false) can hang the whole device -
  // even an independent esp_timer watchdog, armed and confirmed running,
  // never fires when it happens, which points to something disabling
  // interrupts/the scheduler inside the call itself rather than just one
  // task getting stuck. Every one of those hangs happened during
  // interactive use (button held, render task active, hours into a
  // session); the identical calls have never once hung here, run fresh
  // during boot, across dozens of captures. Doing it once now means
  // attemptConnection() later finds WiFi.getMode() already == WIFI_STA and
  // skips this exact call pair entirely (see its own skip-if-already-STA
  // check) - trading a small amount of extra idle-radio power draw for the
  // rest of the session against not touching this call again outside of
  // boot's quiet window.
  if (WiFi.getMode() != WIFI_STA) {
    BootLog::step("WIFI", "boot pre-warm: WiFi.mode(WIFI_OFF) -> persistent(false) -> mode(WIFI_STA) -> disconnect(true)");
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    BootLog::step("WIFI", "boot pre-warm: done");
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  static unsigned long lastMemPrint = 0;
#endif

  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.tiltPageTurnDirection, SETTINGS.orientation,
                       activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  // Periodic heap telemetry is useful for development, but release-level
  // logging must not wake USB/UART every ten seconds while the reader is idle.
  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }
#endif

  // Persist "since last charge" so it survives a sleep cycle or a
  // reflash/reset, not just staying valid within one awake session. Two
  // triggers, for two different reasons:
  //
  // - Immediately, the moment charging stops (USB was connected, now
  //   isn't): on battery, this device's deep sleep fully cuts power to
  //   the MCU - RTC memory included (see HalPowerManager::startDeepSleep()'s
  //   own comment) - and there's no USB-connect wake source configured
  //   either, so the very next sleep (which can happen within seconds of
  //   unplugging) would otherwise wipe the in-RAM value before it's ever
  //   written anywhere durable. This is a rare event (once per unplug),
  //   so writing to SD right when it happens isn't a wear concern.
  // - Every 5 minutes as a fallback, for the rarer case of a reflash/reset
  //   happening in the middle of an still-ongoing charge session, before
  //   any "stopped charging" transition has occurred yet.
  {
    static bool wasUsbConnected = false;
    // This updates only a timestamp from HalGPIO's cached charge state and is
    // internally limited to once per BATTERY_POLL_MS. Do not call
    // getBatteryPercentage() here: on X4 that would start an ADC conversion on
    // every main-loop pass, even while the screen is completely idle.
    powerManager.trackChargingState();

    const bool isUsbConnected = gpio.isUsbConnectedCached();
    const bool chargingJustStopped = wasUsbConnected && !isUsbConnected;
    wasUsbConnected = isUsbConnected;

    static unsigned long lastChargePersistCheck = 0;
    constexpr unsigned long kChargePersistIntervalMs = 5 * 60 * 1000;
    const bool periodicCheckDue = millis() - lastChargePersistCheck >= kChargePersistIntervalMs;

    if (chargingJustStopped || periodicCheckDue) {
      lastChargePersistCheck = millis();
      const uint64_t currentLastCharge = powerManager.getLastChargeEpochSeconds();
      if (currentLastCharge != APP_STATE.lastChargeEpochSeconds) {
        APP_STATE.lastChargeEpochSeconds = currentLastCharge;
        APP_STATE.saveToFile();
      }
    }
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      screenshotComboHandled = true;
      mappedInputManager.suppressNextPowerConfirmRelease();
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

#ifdef SIMULATOR
  if (gpio.consumeSimulatorSleepRequest()) {
    enterDeepSleep();
    lastActivityTime = millis();
    return;
  }
#endif

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    // In the simulator, deep sleep is a no-op and returns — reset the timer so
    // the main loop does not immediately re-trigger auto-sleep.
    lastActivityTime = millis();
    return;
  }

  if (millis() >= allowSleepAt && handleGlobalPowerButtonAction(getPowerButtonAction())) {
    lastActivityTime = millis();
    return;
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

#ifdef SIMULATOR
  runSimulatorSmokeTestTick();
#endif

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
      (void)activityDuration;
    }
  }

  // Safe idle power saving for X3/X4:
  //   < 500 ms idle : normal CPU, 10 ms loop delay
  //   >=500 ms idle : 10 MHz CPU + ordinary 50 ms delay
  //
  // IMPORTANT: no esp_light_sleep_start() is used here. On X4, entering
  // timer light-sleep immediately after USB disconnect can leave the device
  // stuck depending on the USB/power/GPIO state. Downclocking still provides
  // a large part of the idle saving without that risk.
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);
    yield();
  } else {
    const unsigned long idleMs = millis() - lastActivityTime;
    if (idleMs >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      powerManager.setPowerSaving(true);
      delay(50);
    } else {
      powerManager.setPowerSaving(false);
      delay(10);
    }
  }
}
