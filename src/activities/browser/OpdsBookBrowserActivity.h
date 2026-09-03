#pragma once
#include <OpdsParser.h>

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "OpdsCacheStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    DOWNLOADING,
    ERROR,
    SEARCH_INPUT,
    FORMAT_SELECTION,
    INFO_VIEW
  };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server)
      : Activity("OpdsBookBrowser", renderer, mappedInput), buttonNavigator(), server(std::move(server)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  OpdsCacheStore cacheStore;
  std::array<OpdsEntry, 23> pageEntries{};
  size_t pageStart = static_cast<size_t>(-1);
  size_t pageEntryCount = 0;
  size_t cachedEntryCount = 0;
  size_t entryCount = 0;
  bool hasSearchRow = false;
  bool hasPrevRow = false;
  bool hasNextRow = false;
  bool hasRefreshRow = false;
  std::string prevPageUrl;
  std::string nextPageUrl;
  OpdsEntry selectedEntry;
  bool selectedEntryValid = false;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;
  bool longBackTriggered = false;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  size_t lastDownloadUiBytes = 0;
  uint32_t lastDownloadUiMs = 0;

  // In-place OPDS format/details screen. Fixed-size state avoids temporary
  // vectors while the catalog strings are still resident on the C3.
  size_t formatEntryIndex = 0;
  std::array<uint8_t, MAX_OPDS_ACQUISITIONS> formatAcquisitionIndexes{};
  uint8_t formatAcquisitionCount = 0;
  uint8_t formatSelectionIndex = 0;
  int descriptionScroll = 0;
  size_t infoEntryIndex = 0;
  int infoDescriptionScroll = 0;

  OpdsServer server;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoadingBeforeFetch();
  void fetchFeed(const std::string& path, bool forceRefresh = false);
  bool ensureEntryBuffer();
  void clearEntries();
  bool appendEntry(OpdsEntry&& entry);
  bool loadEntry(size_t index, OpdsEntry& out);
  bool loadPageFor(size_t index);
  const OpdsEntry* visibleEntry(size_t index);
  void applyCacheMeta();
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void selectBookFormat(size_t entryIndex);
  void downloadBook(const OpdsEntry& book, size_t acquisitionIndex);
  void showInfoEntry(size_t entryIndex);
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override;
};
