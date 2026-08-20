#include "InkMODState.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 5;
constexpr char STATE_FILE_BIN[] = "/.inkmod/state.bin";
constexpr char STATE_FILE_JSON[] = "/.inkmod/state.json";
constexpr char STATE_FILE_BAK[] = "/.inkmod/state.bin.bak";
}  // namespace

InkMODState InkMODState::instance;

bool InkMODState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void InkMODState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

void InkMODState::clearRecentSleepHistory() {
  std::fill_n(recentSleepImages, SLEEP_RECENT_COUNT, static_cast<uint16_t>(0));
  recentSleepPos = 0;
  recentSleepFill = 0;
}

bool InkMODState::saveToFile() const {
  Storage.mkdir("/.inkmod");
  return JsonSettingsIO::saveState(*this, STATE_FILE_JSON);
}

bool InkMODState::loadFromFile() {
  // readFile() already performs the open, so probing with exists() first only
  // doubles the FAT lookup during every normal boot.
  String json = Storage.readFile(STATE_FILE_JSON);
  if (!json.isEmpty()) {
    return JsonSettingsIO::loadState(*this, json.c_str());
  }

  // Fall back to binary migration. The loader itself detects a missing file,
  // avoiding a second exists()+open pair on systems long since migrated.
  if (loadFromBinaryFile()) {
    if (saveToFile()) {
      Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
      LOG_DBG("CPS", "Migrated state.bin to state.json");
      return true;
    }
    LOG_ERR("CPS", "Failed to save state during migration");
    return false;
  }

  return false;
}

bool InkMODState::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  if (version >= 5) {
    serialization::readPod(inputFile, pendingBookmarkSpine);
    serialization::readPod(inputFile, pendingBookmarkProgress);
    pendingBookmarkParagraphIndex = UINT16_MAX;
  } else {
    pendingBookmarkSpine = UINT16_MAX;
    pendingBookmarkProgress = -1.0f;
    pendingBookmarkParagraphIndex = UINT16_MAX;
  }

  return true;
}
