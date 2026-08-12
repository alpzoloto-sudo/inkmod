#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "InkMODState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  requestUpdate();
}

void ClearCacheActivity::onExit() { Activity::onExit(); }

void ClearCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLEAR_READING_CACHE));

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, tr(STR_CLEAR_CACHE_WARNING_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_CLEAR_CACHE_WARNING_2), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CLEAR_CACHE_WARNING_3), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_CLEAR_CACHE_WARNING_4), true);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CLEARING_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_CLEARED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CLEAR_CACHE_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::clearCache() {
  LOG_DBG("CLEAR_CACHE", "Clearing cache...");

  // Open .inkmod directory
  auto root = Storage.open("/.inkmod");
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool preserve = std::strcmp(name, "wifi.json") == 0 || std::strcmp(name, "inkmod-settings.json") == 0;
    const bool isDirectory = file.isDirectory();
    if (preserve) {
      file.close();
      continue;
    }

    // Keep the path on the stack: the directory entry name is bounded to 128
    // bytes, so this avoids heap churn while removing many cache entries.
    char fullPath[140];
    snprintf(fullPath, sizeof(fullPath), "/.inkmod/%s", name);
    file.close();  // SdFat permits only one active handle during deletion.

    const bool removed = isDirectory ? Storage.removeDir(fullPath) : Storage.remove(fullPath);
    if (removed) {
      clearedCount++;
    } else {
      LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath);
      failedCount++;
    }
  }
  root.close();

  // The on-disk recent-books/state files are gone now, but their singleton
  // objects remain alive until reboot. Clear the volatile copies too, or Home
  // will keep showing the old books and may write them back later.
  RECENT_BOOKS.clearInMemory();
  APP_STATE.openEpubPath.clear();
  APP_STATE.lastSleepFromReader = false;
  APP_STATE.readerActivityLoadCount = 0;

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
      {
        RenderLock lock(*this);
        state = CLEARING;
      }
      if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
        LOG_ERR("CLEAR_CACHE", "Clearing cache screen could not be rendered synchronously; aborting cache clear");
        {
          RenderLock lock(*this);
          state = FAILED;
        }
        requestUpdate(true);
        return;
      }

      clearCache();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
