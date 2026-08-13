#pragma once
#include <atomic>
#include <cstdint>
#include <string>

class InkMODState {
  // Static instance
  static InkMODState instance;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  std::string favoriteSleepImagePath;
  std::string timeoutSleepImagePath;
  std::string preferredSleepFolderPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // time(nullptr) value (wall-clock epoch seconds) as of the last time USB
  // charging was observed, persisted so "since last charge" (see
  // HalPowerManager::getLastChargeEpochSeconds()) survives a sleep cycle
  // or a reflash/reset - wall-clock time specifically, not
  // esp_timer_get_time(), since this device fully powers the MCU off on
  // battery-only deep sleep, resetting that boot-relative counter to ~0 on
  // every wake (see HalPowerManager.cpp's trackChargingState() for the
  // full explanation). The in-RAM (RTC_DATA_ATTR) copy that setting reads
  // during normal operation is faster to update and doesn't need an SD
  // write per poll, but doesn't survive that same battery-only sleep
  // either, hence persisting here too. Written on charge-state
  // transitions only (see main.cpp's loop()), and read once at boot to
  // seed the RTC copy - never written moment-to-moment.
  uint64_t lastChargeEpochSeconds = 0;

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  void clearRecentSleepHistory();
  ~InkMODState() = default;

  // Get singleton instance
  static InkMODState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();
  uint16_t pendingBookmarkSpine = UINT16_MAX;
  float pendingBookmarkProgress = -1.0f;
  uint16_t pendingBookmarkParagraphIndex = UINT16_MAX;

  // Set by background move task on failure; read and cleared by ActivityManager to show AlertActivity.
  // Title/body are written before the flag is set to ensure they are visible when flag is read.
  std::atomic<bool> hasPendingAlert{false};
  std::atomic<bool> pendingAlertGoHomeOnBack{false};
  char pendingAlertTitle[64] = {};
  char pendingAlertBody[256] = {};

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE InkMODState::getInstance()
