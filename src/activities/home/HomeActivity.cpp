#include "HomeActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <Fb2.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../reader/BookReadingStats.h"
#include "../reader/BookStatsActivity.h"
#include "BookActions.h"
#include "BookmarkStore.h"
#include "BookmarksHomeActivity.h"
#include "InkMODSettings.h"
#include "InkMODState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/UITheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"
#include "util/BookArchiveUtils.h"
#include "util/FileSearchUtils.h"

namespace {
constexpr uint32_t CAROUSEL_CACHE_MAGIC = 0x43434152;  // "CCAR"
constexpr uint16_t CAROUSEL_CACHE_VERSION = 6;
constexpr char CAROUSEL_CACHE_PATH[] = "/.inkmod/home_carousel_cache.bin";
constexpr char CAROUSEL_CACHE_TMP_PATH[] = "/.inkmod/home_carousel_cache.tmp";
constexpr uint32_t CAROUSEL_FRAME_MIN_FREE_AFTER_ALLOC = 64U * 1024U;
constexpr uint32_t CAROUSEL_FRAME_MIN_MAX_ALLOC_AFTER_ALLOC = 24U * 1024U;
constexpr unsigned long HOME_BOOK_SWAP_LONG_PRESS_MS = 1000;
constexpr int HOME_BOOK_SWAP_RECENT_COUNT = 2;

enum class HomeMenuAction {
  BrowseFiles,
  SearchFiles,
  ContinueReading,
  RecentBooks,
  OpdsBrowser,
  ReadingStats,
  Bookmarks,
  FileTransfer,
  Settings,
};

struct HomeMenuEntry {
  const char* label;
  UIIcon icon;
  HomeMenuAction action;
};

struct HomeMenuEntries {
  static constexpr int kCapacity = 8;
  std::array<HomeMenuEntry, kCapacity> entries{};
  int count = 0;

  void push(const HomeMenuEntry& entry) {
    if (count >= kCapacity) return;
    entries[count++] = entry;
  }

  int size() const { return count; }

  const HomeMenuEntry& operator[](int index) const { return entries[index]; }
};

struct CarouselCacheHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t frameCount;
  uint32_t frameBufferSize;
  uint64_t keyHash;
  uint16_t screenWidth;
  uint16_t screenHeight;
  uint16_t centerCoverW;
  uint16_t centerCoverH;
  uint16_t sideCoverW;
  uint16_t sideCoverH;
};

uint64_t fnvHash64(const std::string& s) {
  uint64_t hash = 14695981039346656037ull;
  for (char c : s) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool hasAnyBookStats(const BookReadingStats& stats) {
  return stats.sessionCount > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 || stats.isCompleted ||
         stats.startDate.isValid() || stats.finishedDate.isValid();
}

bool hasAnyGlobalStats(const GlobalReadingStats& stats) {
  return stats.totalSessions > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 ||
         stats.completedBooks > 0 || stats.displayLongestReadingStreak() > 0;
}

bool hasHeapForCarouselFrameCache() {
  return ESP.getFreeHeap() >= CAROUSEL_FRAME_MIN_FREE_AFTER_ALLOC &&
         ESP.getMaxAllocHeap() >= CAROUSEL_FRAME_MIN_MAX_ALLOC_AFTER_ALLOC;
}

void appendHashedFileStateToKey(std::string& key, const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("HOME", path, file)) {
    key += "missing";
    key += '\0';
    return;
  }

  uint64_t hash = 14695981039346656037ull;
  size_t totalBytes = 0;
  uint8_t buffer[64];
  while (true) {
    const int bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;
    totalBytes += static_cast<size_t>(bytesRead);
    for (int i = 0; i < bytesRead; ++i) {
      hash ^= buffer[i];
      hash *= 1099511628211ull;
    }
  }
  file.close();

  char digest[48];
  snprintf(digest, sizeof(digest), "%zu:%" PRIu64, totalBytes, static_cast<uint64_t>(hash));
  key += digest;
  key += '\0';
}

std::string getRecentBookCachePath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub::cachePathForFilePath(book.path, "/.inkmod");
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return "/.inkmod/xtc_" + std::to_string(std::hash<std::string>{}(book.path));
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return "/.inkmod/txt_" + std::to_string(std::hash<std::string>{}(book.path));
  }
  return "";
}

BookReadingStats loadRecentBookStats(const RecentBook& book) {
  if (!FsHelpers::hasEpubExtension(book.path) && !FsHelpers::hasXtcExtension(book.path)) {
    return BookReadingStats{};
  }

  const std::string cachePath = getRecentBookCachePath(book);
  return BookReadingStats::load(cachePath);
}

void updateRecentBookCoverPath(const RecentBook& book, const std::string& coverBmpPath) {
  if (!RECENT_BOOKS.updateBook(book.path, book.title, book.author, coverBmpPath)) {
    LOG_ERR("HOME", "failed to update recent book metadata: %s", book.path.c_str());
  }
}

bool hasThumbnailPlaceholder(const std::string& coverBmpPath) {
  return coverBmpPath.find("[WIDTH]") != std::string::npos || coverBmpPath.find("[HEIGHT]") != std::string::npos;
}

std::string getReusableCoverPath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.inkmod").getThumbBmpPath();
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return Xtc(book.path, "/.inkmod").getThumbBmpPath();
  }
  return book.coverBmpPath;
}

bool ensureReusableCoverPath(RecentBook& book) {
  if (book.coverBmpPath.empty() || hasThumbnailPlaceholder(book.coverBmpPath)) {
    return false;
  }

  const std::string reusablePath = getReusableCoverPath(book);
  if (reusablePath.empty() || reusablePath == book.coverBmpPath) {
    return false;
  }

  book.coverBmpPath = reusablePath;
  updateRecentBookCoverPath(book, reusablePath);
  return true;
}

void appendHomeMenuItems(HomeMenuEntries& items, bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks) {
  items.push({tr(STR_BROWSE_FILES), Folder, HomeMenuAction::BrowseFiles});
  if (SETTINGS.showHomeSearch) {
    items.push({tr(STR_SEARCH_FILES), Search, HomeMenuAction::SearchFiles});
  }
  items.push({tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks});

  if (hasOpdsServers) {
    items.push({tr(STR_OPDS_BROWSER), Library, HomeMenuAction::OpdsBrowser});
  }
  if (hasReadingStats) {
    items.push({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }
  if (hasBookmarks) {
    items.push({tr(STR_BOOKMARKS), BookmarkIcon, HomeMenuAction::Bookmarks});
  }

  items.push({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  items.push({tr(STR_SETTINGS_TITLE), Settings, HomeMenuAction::Settings});
}

HomeMenuEntries buildHomeMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks) {
  HomeMenuEntries items;
  appendHomeMenuItems(items, hasOpdsServers, hasReadingStats, hasBookmarks);
  return items;
}

HomeMenuEntries buildMinimalMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks) {
  HomeMenuEntries items;
  items.push({tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks});
  if (SETTINGS.showHomeSearch) {
    items.push({tr(STR_SEARCH_FILES), Search, HomeMenuAction::SearchFiles});
  }

  if (hasOpdsServers) {
    items.push({tr(STR_OPDS_BROWSER), Library, HomeMenuAction::OpdsBrowser});
  }
  if (hasBookmarks) {
    items.push({tr(STR_BOOKMARKS), BookmarkIcon, HomeMenuAction::Bookmarks});
  }
  if (hasReadingStats) {
    items.push({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }

  items.push({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  return items;
}

HomeMenuEntries buildSelectableHomeMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks,
                                             bool includeContinueReading) {
  HomeMenuEntries items;
  if (includeContinueReading) {
    items.push({tr(STR_CONTINUE_READING), Book, HomeMenuAction::ContinueReading});
  }
  appendHomeMenuItems(items, hasOpdsServers, hasReadingStats, hasBookmarks);
  return items;
}

HomeMenuAction homeActionForInitialMenuItem(HomeMenuItem item) {
  switch (item) {
    case HomeMenuItem::FILE_BROWSER:
      return HomeMenuAction::BrowseFiles;
    case HomeMenuItem::RECENTS:
      return HomeMenuAction::RecentBooks;
    case HomeMenuItem::OPDS_BROWSER:
      return HomeMenuAction::OpdsBrowser;
    case HomeMenuItem::FILE_TRANSFER:
      return HomeMenuAction::FileTransfer;
    case HomeMenuItem::SETTINGS_MENU:
      return HomeMenuAction::Settings;
    case HomeMenuItem::NONE:
    default:
      return HomeMenuAction::ContinueReading;
  }
}

int findMenuActionIndex(const HomeMenuEntries& items, HomeMenuAction action) {
  for (int i = 0; i < items.size(); ++i) {
    if (items[i].action == action) {
      return i;
    }
  }
  return -1;
}

bool isMinimalTheme() {
  return static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme) == InkMODSettings::UI_THEME::MINIMAL;
}

bool isAnyFrontButtonPressed(const MappedInputManager& mappedInput) {
  return mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
}

int minimalHomeNavCount(const bool hasCurrentBook) { return hasCurrentBook ? 4 : 3; }

int minimalHomeCoverWidth(int coverHeight) {
  (void)coverHeight;
  return MinimalMetrics::homeCoverImageWidth;
}

int minimalHomeCoverHeight(int coverHeight) {
  (void)coverHeight;
  return MinimalMetrics::homeCoverImageHeight;
}

std::string minimalHomeCoverPath(const RecentBook& book, int coverHeight) {
  if (book.coverBmpPath.empty()) {
    return {};
  }
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.inkmod")
        .getAdaptiveThumbBmpPath(minimalHomeCoverWidth(coverHeight), minimalHomeCoverHeight(coverHeight));
  }
  return UITheme::getCoverThumbPath(book.coverBmpPath, minimalHomeCoverWidth(coverHeight),
                                    minimalHomeCoverHeight(coverHeight));
}

void appendCarouselCoverStateToKey(std::string& key, const RecentBook& book) {
  key += book.path;
  key += '\0';
  key += book.coverBmpPath;
  key += '\0';

  if (book.coverBmpPath.empty()) {
    key += "0:0";
    key += '\0';
    return;
  }

  const std::string centerPath =
      UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH);
  const std::string sidePath =
      UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH);
  key += Storage.exists(centerPath.c_str()) ? '1' : '0';
  key += ':';
  key += Storage.exists(sidePath.c_str()) ? '1' : '0';
  key += '\0';

  const std::string cachePath = getRecentBookCachePath(book);
  if (!cachePath.empty()) {
    appendHashedFileStateToKey(key, cachePath + "/progress.bin");
    if (FsHelpers::hasEpubExtension(book.path)) {
      appendHashedFileStateToKey(key, cachePath + "/stats.bin");
    }
  } else {
    key += "no-cache-path";
    key += '\0';
  }
}

void appendSyncedStatsStateToKey(std::string& key) {
  FsFile dir = Storage.open("/.inkmod/synced_stats");
  if (!dir) {
    key += "no-synced-stats";
    key += '\0';
    return;
  }

  if (!dir.isDirectory()) {
    dir.close();
    key += "synced-stats-not-dir";
    key += '\0';
    return;
  }

  char name[128];
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    if (!isDirectory && nameLen > 0) {
      key += name;
      key += '\0';
      file.close();
      appendHashedFileStateToKey(key, std::string("/.inkmod/synced_stats/") + name);
      continue;
    }
    file.close();
  }
  dir.close();
}

void appendCarouselMenuStateToKey(std::string& key, const bool hasOpdsServers, const bool hasReadingStats,
                                  const bool hasBookmarks) {
  // The cached carousel frame includes the bottom menu row. Search visibility
  // changes that row too, so it must be part of the cache key.
  key += SETTINGS.showHomeSearch ? "search:1" : "search:0";
  key += '\0';
  key += hasOpdsServers ? "opds:1" : "opds:0";
  key += '\0';
  key += hasReadingStats ? "stats:1" : "stats:0";
  key += '\0';
  key += hasBookmarks ? "bookmarks:1" : "bookmarks:0";
  key += '\0';
}

void buildCarouselCacheKey(const std::vector<RecentBook>& recentBooks, const bool hasOpdsServers,
                           const bool hasReadingStats, const bool hasBookmarks, std::string& key, uint64_t& keyHash) {
  key.clear();
  key.reserve(512);
  // The carousel cache stores the bottom icon row too, so menu visibility must
  // be part of the key alongside book covers/progress.
  appendCarouselMenuStateToKey(key, hasOpdsServers, hasReadingStats, hasBookmarks);
  for (const auto& book : recentBooks) {
    appendCarouselCoverStateToKey(key, book);
  }
  appendHashedFileStateToKey(key, "/.inkmod/global_stats.bin");
  appendSyncedStatsStateToKey(key);
  keyHash = fnvHash64(key);
}

bool isCarouselCacheHeaderValid(const CarouselCacheHeader& header, uint64_t cacheKeyHash, int bookCount,
                                const GfxRenderer& renderer) {
  return header.magic == CAROUSEL_CACHE_MAGIC && header.version == CAROUSEL_CACHE_VERSION &&
         header.keyHash == cacheKeyHash && header.frameCount == bookCount &&
         header.frameBufferSize == renderer.getBufferSize() && header.screenWidth == renderer.getScreenWidth() &&
         header.screenHeight == renderer.getScreenHeight() && header.centerCoverW == LyraCarouselTheme::kCenterThumbW &&
         header.centerCoverH == LyraCarouselTheme::kCenterThumbH &&
         header.sideCoverW == LyraCarouselTheme::kSideCoverW && header.sideCoverH == LyraCarouselTheme::kSideCoverH;
}

bool readCarouselCacheHeader(FsFile& file, CarouselCacheHeader& header) {
  CarouselCacheHeader readHeader{};
  if (!serialization::tryReadPod(file, readHeader)) {
    return false;
  }
  header = readHeader;
  return true;
}

bool hasValidCarouselDiskCache(const std::vector<RecentBook>& recentBooks, const GfxRenderer& renderer,
                               const bool hasOpdsServers, const bool hasReadingStats, const bool hasBookmarks) {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount <= 0) return false;

  std::string cacheKey;
  uint64_t cacheKeyHash = 0;
  buildCarouselCacheKey(recentBooks, hasOpdsServers, hasReadingStats, hasBookmarks, cacheKey, cacheKeyHash);

  FsFile cacheFile;
  if (!Storage.openFileForRead("HOME", CAROUSEL_CACHE_PATH, cacheFile)) {
    return false;
  }

  CarouselCacheHeader header{};
  const bool readOk = readCarouselCacheHeader(cacheFile, header);
  cacheFile.close();
  return readOk && isCarouselCacheHeaderValid(header, cacheKeyHash, bookCount, renderer);
}

int getVisibleRecentBookCount(const std::vector<RecentBook>& recentBooks) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return std::min(static_cast<int>(recentBooks.size()), metrics.homeRecentBooksCount);
}

int getHomeMenuSelectionOffset(const std::vector<RecentBook>& recentBooks) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.homeContinueReadingInMenu ? 0 : getVisibleRecentBookCount(recentBooks);
}
}  // namespace

// ---------------------------------------------------------------------------
// Static carousel frame cache — survives HomeActivity re-creation so that
// returning to home (e.g. after settings) doesn't re-read covers from SD.
// Freed explicitly in onSelectBook() before entering the reader.
// ---------------------------------------------------------------------------
namespace {
class CarouselCache {
 public:
  uint8_t* frames[HomeActivity::kCarouselFrameCount] = {};
  int frameBookIdx[HomeActivity::kCarouselFrameCount] = {-1};
  int frameCount = 0;
  int lastCenterIdx = -1;
  std::string key;
  uint64_t keyHash = 0;

  int findFrameSlot(int bookIdx) const {
    for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
      if (frameBookIdx[i] == bookIdx && frames[i] != nullptr) return i;
    }
    return -1;
  }

  void invalidate() {
    for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
      if (frames[i]) {
        free(frames[i]);
        frames[i] = nullptr;
      }
      frameBookIdx[i] = -1;
    }
    frameCount = 0;
    lastCenterIdx = -1;
    key.clear();
    keyHash = 0;
  }
};

CarouselCache gCarouselCache;
}  // namespace

static_assert(HomeActivity::kMaxCachedBooks >= LyraCarouselMetrics::values.homeRecentBooksCount,
              "kMaxCachedBooks must cover all carousel slots");

int HomeActivity::getMenuItemCount() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool includeContinueReading = metrics.homeContinueReadingInMenu && !recentBooks.empty();

  // Derive the selector count from the exact same builder used for rendering.
  // A hard-coded base count still included Search after it was hidden, letting
  // selectorIndex advance past the real menu and causing a panic on return Home.
  int count = static_cast<int>(
      buildSelectableHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks, includeContinueReading).size());

  if (!metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    count += getVisibleRecentBookCount();
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& storedBook : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    RecentBook book = storedBook;
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    ensureReusableCoverPath(book);
    recentBooks.push_back(book);
  }
}

void HomeActivity::loadAllBookStats() {
  const auto start = millis();
  const int count = std::min(static_cast<int>(recentBooks.size()), kMaxCachedBooks);
  for (int i = 0; i < count; ++i) {
    cachedBookStats[i] = loadRecentBookStats(recentBooks[i]);
    cachedBookProgress[i] = RecentBookProgress::loadPercent(recentBooks[i]);
  }
  bookStatsCached = true;
  LOG_DBG("HOME", "carousel: cached stats/progress for %d book(s) in %lums", count, millis() - start);
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  const bool isCarouselTheme =
      static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme) == InkMODSettings::UI_THEME::LYRA_CAROUSEL;
  const bool isMinimal = isMinimalTheme();
  const size_t recentBookCount = recentBooks.size();
  // Tracks which book indices had a thumbnail generated this pass.
  std::vector<char> bookUpdated(recentBookCount, false);
  const int progressIncrement = 90 / static_cast<int>(std::max<size_t>(1, recentBookCount));

  int progress = 0;
  for (size_t bookIdx = 0; bookIdx < recentBooks.size(); ++bookIdx) {
    RecentBook& book = recentBooks[bookIdx];
    if (!Storage.exists(book.path.c_str())) {
      progress++;
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      if (isCarouselTheme) {
        // For carousel: generate exact-size thumbnails for the center image rect and side slots.
        // Load the source image once even when both sizes are missing.
        const std::string centerPath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterThumbW,
                                                                  LyraCarouselTheme::kCenterThumbH);
        const std::string sidePath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW,
                                                                LyraCarouselTheme::kSideCoverH);
        const bool centerMissing = !Storage.exists(centerPath.c_str());
        const bool sideMissing = !Storage.exists(sidePath.c_str());

        if (centerMissing || sideMissing) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.inkmod");
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * progressIncrement);
            if (!epub.load(false, true)) {
              LOG_ERR("HOME", "carousel: failed to load EPUB cache for thumb generation: %s", book.path.c_str());
              updateRecentBookCoverPath(book, "");
              book.coverBmpPath = "";
              coverRendered = false;
              requestUpdate();
              progress++;
              continue;
            }
            bool success = true;
            if (centerMissing)
              success =
                  epub.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH) && success;
            if (sideMissing)
              success =
                  epub.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
            if (!success) {
              updateRecentBookCoverPath(book, "");
              book.coverBmpPath = "";
            } else {
              bookUpdated[bookIdx] = true;
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.inkmod");
            if (xtc.load()) {
              if (!showingLoading) {
                showingLoading = true;
                popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
              }
              GUI.fillPopupProgress(renderer, popupRect, 10 + progress * progressIncrement);
              bool success = true;
              if (centerMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH) && success;
              if (sideMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
              if (!success) {
                updateRecentBookCoverPath(book, "");
                book.coverBmpPath = "";
              } else {
                bookUpdated[bookIdx] = true;
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      } else {
        // Non-carousel: generate the active theme's thumbnail size.
        const bool useMinimalThumb =
            isMinimal && (FsHelpers::hasEpubExtension(book.path) || FsHelpers::hasXtcExtension(book.path));
        const std::string coverPath = useMinimalThumb ? minimalHomeCoverPath(book, coverHeight)
                                                      : UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
        if (coverPath.empty() || !Storage.exists(coverPath.c_str())) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.inkmod");
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * progressIncrement);
            if (!epub.load(false, true)) {
              LOG_ERR("HOME", "failed to load EPUB cache for thumb generation: %s", book.path.c_str());
              updateRecentBookCoverPath(book, "");
              book.coverBmpPath = "";
              coverRendered = false;
              requestUpdate();
              progress++;
              continue;
            }
            const bool success = useMinimalThumb ? epub.generateAdaptiveThumbBmp(minimalHomeCoverWidth(coverHeight),
                                                                                 minimalHomeCoverHeight(coverHeight))
                                                 : epub.generateThumbBmp(0, coverHeight);
            if (!success) {
              updateRecentBookCoverPath(book, "");
              book.coverBmpPath = "";
            } else {
              bookUpdated[bookIdx] = true;  // non-carousel path reuses same tracking
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.inkmod");
            if (xtc.load()) {
              if (!showingLoading) {
                showingLoading = true;
                popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
              }
              GUI.fillPopupProgress(renderer, popupRect, 10 + progress * progressIncrement);
              const bool success =
                  useMinimalThumb ? xtc.generateThumbBmp(static_cast<uint16_t>(minimalHomeCoverWidth(coverHeight)),
                                                         static_cast<uint16_t>(minimalHomeCoverHeight(coverHeight)))
                                  : xtc.generateThumbBmp(coverHeight);
              if (!success) {
                updateRecentBookCoverPath(book, "");
                book.coverBmpPath = "";
              } else {
                bookUpdated[bookIdx] = true;
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;

  // Re-render only the affected slots rather than rebuilding the entire cache.
  if (isCarouselTheme) {
    bool anyUpdated = false;
    for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
      if (static_cast<size_t>(i) >= bookUpdated.size() || !bookUpdated[i]) continue;
      anyUpdated = true;
      if (carouselFramesReady) {
        // Only re-render the slot holding this book; books outside the window
        // will be picked up by updateSlidingWindowCache on next navigation.
        const int slot = gCarouselCache.findFrameSlot(i);
        if (slot >= 0) renderCarouselFrame(i, slot);
      }
    }
    if (anyUpdated) {
      if (!carouselFramesReady) {
        // Cover assets changed before the carousel cache was initialised, so
        // any existing SD snapshot may still contain placeholder frames.
        // Force a rebuild from the fresh thumbs instead of reusing stale
        // `home_carousel_cache.bin` content keyed only by book order/layout.
        if (Storage.exists(CAROUSEL_CACHE_PATH)) {
          Storage.remove(CAROUSEL_CACHE_PATH);
        }
        if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
          Storage.remove(CAROUSEL_CACHE_TMP_PATH);
        }
        preRenderCarouselFrames();
      } else {
        // The live carousel frames are already updated above. Keep Home
        // responsive by invalidating any stale SD snapshot instead of
        // rewriting all 5 frames synchronously on this return-to-Home path.
        if (Storage.exists(CAROUSEL_CACHE_PATH)) {
          Storage.remove(CAROUSEL_CACHE_PATH);
        }
        if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
          Storage.remove(CAROUSEL_CACHE_TMP_PATH);
        }
      }
      requestUpdate();
    }
  }
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  const bool isCarouselTheme =
      static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme) == InkMODSettings::UI_THEME::LYRA_CAROUSEL;

  // Check if any books have bookmarks (directory scan only, no file parsing)
  hasBookmarks = BookmarkStore::hasAnyBookmarks();

  selectorIndex = 0;
  lastCarouselBookIndex = 0;
  minimalMenuOpen = false;
  minimalSuppressInitialFrontRelease = isMinimalTheme();
  minimalMenuIndex = 0;
  minimalHomeNavIndex = -1;
  carouselFramesReady = false;
  carouselWarmupPending = isCarouselTheme;
  coverRendered = false;
  // A saved cover snapshot (see storeCoverBuffer()/restoreCoverBuffer()) is
  // only ever valid for the exact screen content it was captured from.
  // Coming back here after Settings/FileBrowser drew something completely
  // different over the same screen region leaves whatever was captured
  // before that trip stale - restoring it would blit old (or, worse,
  // since-reused-by-something-else) pixel data instead of the current
  // cover. Dropping it here forces a fresh render on next draw, the same
  // as every other piece of carousel state onEnter() already resets.
  freeCoverBuffer();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int recentBooksToLoad =
      std::min(kMaxCachedBooks, std::max(metrics.homeRecentBooksCount, HOME_BOOK_SWAP_RECENT_COUNT));
  loadRecentBooks(recentBooksToLoad);

  if (!APP_STATE.openEpubPath.empty()) {
    for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
      if (recentBooks[i].path == APP_STATE.openEpubPath) {
        if (metrics.homeRecentBooksCount == 1 && i > 0) {
          std::rotate(recentBooks.begin(), recentBooks.begin() + i, recentBooks.end());
          selectorIndex = 0;
          lastCarouselBookIndex = 0;
        } else {
          selectorIndex = i;
          lastCarouselBookIndex = i;
        }
        break;
      }
    }
  }

  globalStats = GlobalReadingStats::load();
  showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
  allDevicesGlobalStats = showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
  if (isCarouselTheme) {
    loadAllBookStats();
  }
  updateHighlightedBookContext();

  if (initialMenuItem != HomeMenuItem::NONE) {
    const bool includeContinueReading = metrics.homeContinueReadingInMenu && !recentBooks.empty();
    const auto menuItems =
        buildSelectableHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks, includeContinueReading);
    const int menuIndex = findMenuActionIndex(menuItems, homeActionForInitialMenuItem(initialMenuItem));
    if (menuIndex >= 0) {
      selectorIndex = getHomeMenuSelectionOffset(recentBooks) + menuIndex;
      updateHighlightedBookContext();
    }
  }

  if (isCarouselTheme) {
    // Full-screen carousel snapshots are intentionally disabled for release
    // stability. Remove any cache left by older beta builds.
    preRenderCarouselFrames(false);
  }

  requestUpdate();
}

int HomeActivity::getHighlightedBookIndex() const {
  if (recentBooks.empty()) {
    return -1;
  }

  const int visibleBookCount = getVisibleRecentBookCount();
  const int highlightedBookIdx = (selectorIndex < visibleBookCount) ? selectorIndex : lastCarouselBookIndex;
  return std::clamp(highlightedBookIdx, 0, visibleBookCount - 1);
}

int HomeActivity::getVisibleRecentBookCount() const { return ::getVisibleRecentBookCount(recentBooks); }

bool HomeActivity::canSwapHomeBook() const {
  return UITheme::getInstance().getMetrics().homeRecentBooksCount == 1 && recentBooks.size() > 1;
}

void HomeActivity::showNextRecentBookOnHome() {
  if (!canSwapHomeBook()) {
    return;
  }

  std::rotate(recentBooks.begin(), recentBooks.begin() + 1, recentBooks.end());
  selectorIndex = 0;
  lastCarouselBookIndex = 0;
  bookStatsCached = false;
  updateHighlightedBookContext();
  coverRendered = false;
  freeCoverBuffer();
  requestUpdate();
}

std::string HomeActivity::getCurrentBookPath() const {
  const int idx = getHighlightedBookIndex();
  return idx >= 0 ? recentBooks[idx].path : std::string{};
}

void HomeActivity::updateHighlightedBookContext() {
  const auto start = millis();
  currentBookStats = BookReadingStats{};
  currentBookProgressPercent = -1.0f;

  const int idx = getHighlightedBookIndex();
  const bool useCachedStats = idx >= 0 && bookStatsCached && idx < kMaxCachedBooks;
  if (idx >= 0) {
    if (useCachedStats) {
      currentBookStats = cachedBookStats[idx];
      currentBookProgressPercent = cachedBookProgress[idx];
    } else {
      currentBookStats = loadRecentBookStats(recentBooks[idx]);
      currentBookProgressPercent = RecentBookProgress::loadPercent(recentBooks[idx]);
    }
  }

  hasReadingStats = hasAnyBookStats(currentBookStats) || hasAnyGlobalStats(globalStats) ||
                    (showAllDevicesStats && hasAnyGlobalStats(allDevicesGlobalStats));
  LOG_DBG("HOME", "updateHighlightedBookContext idx=%d cached=%s took %lums", idx, useCachedStats ? "yes" : "no",
          millis() - start);
}

void HomeActivity::onExit() {
  Activity::onExit();

  freeCoverBuffer();
  gCarouselCache.invalidate();
  freeCarouselFrames();
  carouselWarmupPending = false;
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::freeCarouselFrames() {
  // Instance pointers are aliases into the static cache — do not free here.
  for (int i = 0; i < kCarouselFrameCount; ++i) carouselFrames[i] = nullptr;
  carouselFramesReady = false;
}

bool HomeActivity::allocateCarouselFrameSlots(int targetFrameCount) {
  const size_t bufferSize = renderer.getBufferSize();
  int frameCount = 0;
  for (int attemptFrameCount = targetFrameCount; attemptFrameCount >= 1; --attemptFrameCount) {
    bool allocFailed = false;
    for (int i = 0; i < attemptFrameCount; ++i) {
      gCarouselCache.frames[i] = static_cast<uint8_t*>(malloc(bufferSize));
      if (!gCarouselCache.frames[i]) {
        LOG_ERR("HOME", "preRenderCarouselFrames: malloc failed for frame %d while allocating %d frame(s)", i,
                attemptFrameCount);
        allocFailed = true;
        break;
      }
      if (!hasHeapForCarouselFrameCache()) {
        LOG_INF("HOME", "carousel: low heap after frame cache alloc (%u free, %u maxAlloc); skipping cache",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        free(gCarouselCache.frames[i]);
        gCarouselCache.frames[i] = nullptr;
        allocFailed = true;
        break;
      }
      gCarouselCache.frameBookIdx[i] = -1;
    }

    if (!allocFailed) {
      frameCount = attemptFrameCount;
      break;
    }

    for (int i = 0; i < attemptFrameCount; ++i) {
      if (gCarouselCache.frames[i]) {
        free(gCarouselCache.frames[i]);
        gCarouselCache.frames[i] = nullptr;
      }
      gCarouselCache.frameBookIdx[i] = -1;
    }
  }

  if (frameCount == 0) {
    gCarouselCache.invalidate();
    return false;
  }

  gCarouselCache.frameCount = frameCount;
  LOG_INF("HOME", "carousel: frame cache capacity %d/%d", frameCount, targetFrameCount);
  return true;
}

void HomeActivity::renderCarouselFrameToCurrentBuffer(int bookIdx, BookReadingStats* outStats,
                                                      float* outProgressPercent, bool* outUsedCachedStats) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int bookCount = static_cast<int>(recentBooks.size());
  bool dummy1 = false, dummy2 = false, dummy3 = false;
  BookReadingStats frameStats;
  const BookReadingStats* frameStatsPtr = nullptr;
  float frameProgressPercent = -1.0f;
  bool usedCachedStats = false;

  if (bookIdx >= 0 && bookIdx < bookCount) {
    if (bookStatsCached && bookIdx < kMaxCachedBooks) {
      usedCachedStats = true;
      frameStats = cachedBookStats[bookIdx];
      frameProgressPercent = cachedBookProgress[bookIdx];
    } else {
      frameStats = loadRecentBookStats(recentBooks[bookIdx]);
      frameProgressPercent = RecentBookProgress::loadPercent(recentBooks[bookIdx]);
    }
    if (hasAnyBookStats(frameStats)) frameStatsPtr = &frameStats;
  }

  LyraCarouselTheme::setPreRenderIndex(bookIdx);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
  GUI.drawRecentBookCover(
      renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, recentBooks, bookCount, dummy1,
      dummy2, dummy3, []() { return true; }, frameStatsPtr, frameProgressPercent);

  const bool frameHasReadingStats = hasAnyBookStats(frameStats) || hasAnyGlobalStats(globalStats) ||
                                    (showAllDevicesStats && hasAnyGlobalStats(allDevicesGlobalStats));
  const auto menuItems = buildHomeMenuItems(hasOpdsServers, frameHasReadingStats, hasBookmarks);
  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), -1, [&menuItems](int index) { return std::string(menuItems[index].label); },
      [&menuItems](int index) { return menuItems[index].icon; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (outStats) *outStats = frameStats;
  if (outProgressPercent) *outProgressPercent = frameProgressPercent;
  if (outUsedCachedStats) *outUsedCachedStats = usedCachedStats;
}

bool HomeActivity::buildCarouselCacheFile(const std::string& cacheKey, uint64_t cacheKeyHash, int bookCount,
                                          bool showProgressPopup) {
  (void)cacheKey;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer || bookCount <= 0) return false;

  Storage.mkdir("/.inkmod");
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
  }

  FsFile file;
  if (!Storage.openFileForWrite("HOME", CAROUSEL_CACHE_TMP_PATH, file)) {
    return false;
  }

  const CarouselCacheHeader header = {
      CAROUSEL_CACHE_MAGIC,
      CAROUSEL_CACHE_VERSION,
      static_cast<uint16_t>(bookCount),
      static_cast<uint32_t>(renderer.getBufferSize()),
      cacheKeyHash,
      static_cast<uint16_t>(renderer.getScreenWidth()),
      static_cast<uint16_t>(renderer.getScreenHeight()),
      static_cast<uint16_t>(LyraCarouselTheme::kCenterThumbW),
      static_cast<uint16_t>(LyraCarouselTheme::kCenterThumbH),
      static_cast<uint16_t>(LyraCarouselTheme::kSideCoverW),
      static_cast<uint16_t>(LyraCarouselTheme::kSideCoverH),
  };
  if (!serialization::tryWritePod(file, header)) {
    file.close();
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to write SD cache header");
    return false;
  }

  const auto start = millis();
  Rect popupRect{};
  uint8_t* progressFrameBuffer = nullptr;
  const size_t bufferSize = renderer.getBufferSize();
  if (showProgressPopup) {
    progressFrameBuffer = static_cast<uint8_t*>(malloc(bufferSize));
    if (!progressFrameBuffer) {
      LOG_ERR("HOME", "carousel: failed to allocate progress overlay buffer");
      showProgressPopup = false;
    } else {
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
      memcpy(progressFrameBuffer, frameBuffer, bufferSize);
    }
  }
  bool writeFailed = false;
  for (int i = 0; i < bookCount; ++i) {
    const int cachedSlot = gCarouselCache.findFrameSlot(i);
    if (cachedSlot >= 0 && carouselFrames[cachedSlot]) {
      memcpy(frameBuffer, carouselFrames[cachedSlot], renderer.getBufferSize());
    } else {
      renderCarouselFrameToCurrentBuffer(i, nullptr, nullptr, nullptr);
    }
    if (file.write(frameBuffer, renderer.getBufferSize()) != renderer.getBufferSize()) {
      writeFailed = true;
      break;
    }
    if (showProgressPopup) {
      memcpy(frameBuffer, progressFrameBuffer, bufferSize);
      GUI.fillPopupProgress(renderer, popupRect, ((i + 1) * 100) / bookCount);
    }
  }

  const bool syncOk = file.sync();
  file.close();

  if (writeFailed || !syncOk) {
    free(progressFrameBuffer);
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to write SD cache snapshot");
    return false;
  }

  if (Storage.exists(CAROUSEL_CACHE_PATH)) {
    Storage.remove(CAROUSEL_CACHE_PATH);
  }
  if (!Storage.rename(CAROUSEL_CACHE_TMP_PATH, CAROUSEL_CACHE_PATH)) {
    free(progressFrameBuffer);
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to promote SD cache snapshot");
    return false;
  }

  free(progressFrameBuffer);
  LOG_DBG("HOME", "carousel: built SD cache for %d book(s) in %lums", bookCount, millis() - start);
  return true;
}

bool HomeActivity::loadCarouselFrameFromDisk(uint64_t cacheKeyHash, int bookCount, int bookIdx, int slotIdx) {
  if (slotIdx < 0 || slotIdx >= kCarouselFrameCount || !gCarouselCache.frames[slotIdx] || bookIdx < 0 ||
      bookIdx >= bookCount) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("HOME", CAROUSEL_CACHE_PATH, file)) {
    return false;
  }

  CarouselCacheHeader header{};
  if (!readCarouselCacheHeader(file, header) ||
      !isCarouselCacheHeaderValid(header, cacheKeyHash, bookCount, renderer)) {
    file.close();
    return false;
  }

  const size_t frameOffset = sizeof(CarouselCacheHeader) + static_cast<size_t>(bookIdx) * renderer.getBufferSize();
  if (!file.seek(frameOffset)) {
    file.close();
    return false;
  }
  const size_t expectedBytes = renderer.getBufferSize();
  size_t totalBytesRead = 0;
  while (totalBytesRead < expectedBytes) {
    const int bytesRead = file.read(gCarouselCache.frames[slotIdx] + totalBytesRead, expectedBytes - totalBytesRead);
    if (bytesRead <= 0) {
      break;
    }
    totalBytesRead += static_cast<size_t>(bytesRead);
  }
  file.close();
  if (totalBytesRead != expectedBytes) {
    LOG_ERR("HOME", "carousel: short read for slot %d (%zu/%zu bytes)", slotIdx, totalBytesRead, expectedBytes);
    return false;
  }

  gCarouselCache.frameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCarouselCache.frames[slotIdx];
  return true;
}

int HomeActivity::chooseCarouselEvictionSlot(int centerIdx, int bookCount, std::optional<int> protectedBookIdx) const {
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (gCarouselCache.frames[i] && gCarouselCache.frameBookIdx[i] < 0) {
      return i;
    }
  }

  int evictSlot = -1;
  int maxDist = -1;
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (!gCarouselCache.frames[i]) continue;
    const int cachedBookIdx = gCarouselCache.frameBookIdx[i];
    if (protectedBookIdx.has_value() && cachedBookIdx == protectedBookIdx.value()) continue;
    const int diff = std::abs(cachedBookIdx - centerIdx);
    const int dist = std::min(diff, bookCount - diff);
    if (dist > maxDist) {
      maxDist = dist;
      evictSlot = i;
    }
  }
  return evictSlot;
}

bool HomeActivity::preRenderCarouselFrames(bool showProgressPopup) {
  (void)showProgressPopup;

  // RELEASE-STABILITY PATH:
  // Do not cache complete 48-KB Home framebuffers for the carousel.
  //
  // The carousel already caches/resizes the expensive cover bitmaps themselves.
  // Caching the *entire rendered screen* on top of that introduced an intermittent
  // failure mode where a stale/partially-valid snapshot could be restored and
  // only the live selection border was drawn over it. Visually this produced the
  // wandering "empty rectangle + bottom icons" Home screen.
  //
  // Live composition is cheap compared with the E-Ink refresh (~500 ms) and
  // avoids another framebuffer-sized allocation entirely. Keep thumbnail caches,
  // but always compose Home from current state.
  gCarouselCache.invalidate();
  freeCarouselFrames();

  if (Storage.exists(CAROUSEL_CACHE_PATH)) {
    Storage.remove(CAROUSEL_CACHE_PATH);
  }
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
  }

  carouselFramesReady = false;
  coverRendered = false;
  coverBufferStored = false;
  return false;
}

void HomeActivity::loop() {
  if (isMinimalTheme()) {
    const int pressedFrontButton = mappedInput.getPressedFrontButton();
    const int releasedFrontButton = mappedInput.getReleasedFrontButton();

    if (minimalSuppressInitialFrontRelease) {
      if (releasedFrontButton >= 0) {
        minimalSuppressInitialFrontRelease = false;
        return;
      }
      if (!isAnyFrontButtonPressed(mappedInput)) {
        minimalSuppressInitialFrontRelease = false;
      }
    }

    if (homeBookSwapLongPressHandled) {
      if (releasedFrontButton == HalGPIO::BTN_BACK || !mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK)) {
        homeBookSwapLongPressHandled = false;
      }
      return;
    }

    if (minimalMenuOpen) {
      const auto menuItems = buildMinimalMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks);
      const int menuCount = static_cast<int>(menuItems.size());
      if (menuCount <= 0) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }

      if (minimalMenuIndex >= menuCount) {
        minimalMenuIndex = menuCount - 1;
      }

      buttonNavigator.onPreviousPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::previousIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      buttonNavigator.onNextPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::nextIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        switch (menuItems[minimalMenuIndex].action) {
          case HomeMenuAction::BrowseFiles:
            onFileBrowserOpen();
            break;
          case HomeMenuAction::SearchFiles:
            onSearchFilesOpen();
            break;
          case HomeMenuAction::RecentBooks:
            onRecentsOpen();
            break;
          case HomeMenuAction::OpdsBrowser:
            onOpdsBrowserOpen();
            break;
          case HomeMenuAction::ReadingStats:
            onReadingStatsOpen();
            break;
          case HomeMenuAction::Bookmarks:
            onBookmarksOpen();
            break;
          case HomeMenuAction::FileTransfer:
            onFileTransferOpen();
            break;
          case HomeMenuAction::ContinueReading:
          case HomeMenuAction::Settings:
            break;
        }
      }
      return;
    }

    if (canSwapHomeBook() && mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) &&
        mappedInput.getHeldTime() >= HOME_BOOK_SWAP_LONG_PRESS_MS) {
      homeBookSwapLongPressHandled = true;
      showNextRecentBookOnHome();
      return;
    }

    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      minimalHomeNavIndex = minimalHomeNavIndex < 0 ? homeNavCount - 1
                                                    : ButtonNavigator::previousIndex(minimalHomeNavIndex, homeNavCount);
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      minimalHomeNavIndex = minimalHomeNavIndex < 0 ? 0 : ButtonNavigator::nextIndex(minimalHomeNavIndex, homeNavCount);
      requestUpdate();
      return;
    }

    auto activateMinimalHomeNav = [this](int index) {
      switch (index) {
        case 0:
          minimalMenuOpen = true;
          minimalMenuIndex = 0;
          requestUpdate();
          break;
        case 1:
          onFileBrowserOpen();
          break;
        case 2:
          onSettingsOpen();
          break;
        case 3:
          onContinueReading();
          break;
      }
    };

    if (releasedFrontButton == HalGPIO::BTN_BACK) {
      minimalHomeNavIndex = 0;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_CONFIRM) {
      minimalHomeNavIndex = 1;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_LEFT) {
      minimalHomeNavIndex = 2;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_RIGHT) {
      if (!recentBooks.empty()) {
        minimalHomeNavIndex = 3;
        activateMinimalHomeNav(minimalHomeNavIndex);
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (minimalHomeNavIndex >= 0) {
        activateMinimalHomeNav(minimalHomeNavIndex);
      }
      return;
    }
    return;
  }

  const bool isCarousel =
      static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme) == InkMODSettings::UI_THEME::LYRA_CAROUSEL;
  const int previousHighlightedBookIdx = getHighlightedBookIndex();
  const int visibleBookCount = getVisibleRecentBookCount();

  if (homeBookSwapLongPressHandled) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      homeBookSwapLongPressHandled = false;
    }
    return;
  }

  if (!isCarousel && canSwapHomeBook() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= HOME_BOOK_SWAP_LONG_PRESS_MS) {
    homeBookSwapLongPressHandled = true;
    showNextRecentBookOnHome();
    return;
  }

  const bool isLyraExtended =
      static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme) == InkMODSettings::UI_THEME::LYRA_3_COVERS;

  if (isCarousel) {
    const int bookCount = visibleBookCount;
    const int menuItemCount =
        static_cast<int>(buildHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks).size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int menuIdx = inCarouselRow ? 0 : (selectorIndex - bookCount);

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (inCarouselRow && bookCount > 0)
        selectorIndex = (selectorIndex + 1) % bookCount;
      else if (!inCarouselRow)
        selectorIndex = bookCount + (menuIdx + 1) % menuItemCount;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (inCarouselRow && bookCount > 0)
        selectorIndex = (selectorIndex + bookCount - 1) % bookCount;
      else if (!inCarouselRow)
        selectorIndex = bookCount + (menuIdx + menuItemCount - 1) % menuItemCount;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (inCarouselRow) {
        lastCarouselBookIndex = selectorIndex;
        selectorIndex = bookCount;
      } else {
        selectorIndex = lastCarouselBookIndex;
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (inCarouselRow) {
        lastCarouselBookIndex = selectorIndex;
        selectorIndex = bookCount;
      } else {
        selectorIndex = lastCarouselBookIndex;
      }
      requestUpdate();
    }
  } else if (isLyraExtended) {
    // Lyra Extended has two intentionally different navigation axes.
    //
    // Front Left/Right buttons traverse one continuous ring:
    // books 1..3 -> home menu rows -> book 1.
    //
    // Side Up/Down buttons treat the three-cover row as a single vertical
    // entry: from ANY selected cover Down enters the first menu row, Up enters
    // the last menu row, and crossing the menu boundary always returns to
    // Book 1.  This exactly matches the physical layout on X4.
    const int totalCount = getMenuItemCount();
    const int bookCount = visibleBookCount;
    const int firstMenuIndex = bookCount;
    const int lastMenuIndex = totalCount - 1;

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalCount);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalCount);
      requestUpdate();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (bookCount > 0 && selectorIndex < bookCount) {
        selectorIndex = firstMenuIndex;
      } else if (selectorIndex >= firstMenuIndex && selectorIndex < lastMenuIndex) {
        ++selectorIndex;
      } else {
        selectorIndex = 0;
      }
      requestUpdate();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (bookCount > 0 && selectorIndex < bookCount) {
        selectorIndex = lastMenuIndex;
      } else if (selectorIndex > firstMenuIndex) {
        --selectorIndex;
      } else {
        selectorIndex = 0;
      }
      requestUpdate();
    }
  } else {
    const int menuCount = getMenuItemCount();
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  if (getHighlightedBookIndex() != previousHighlightedBookIdx) {
    updateHighlightedBookContext();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    if (!metrics.homeContinueReadingInMenu && selectorIndex < visibleBookCount) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }

    auto menuItems = buildSelectableHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks,
                                                  metrics.homeContinueReadingInMenu && !recentBooks.empty());
    const int menuSelectedIndex = selectorIndex - getHomeMenuSelectionOffset(recentBooks);
    if (menuSelectedIndex < 0 || menuSelectedIndex >= static_cast<int>(menuItems.size())) {
      return;
    }

    switch (menuItems[menuSelectedIndex].action) {
      case HomeMenuAction::BrowseFiles:
        onFileBrowserOpen();
        break;
      case HomeMenuAction::SearchFiles:
        onSearchFilesOpen();
        break;
      case HomeMenuAction::ContinueReading:
        onContinueReading();
        break;
      case HomeMenuAction::RecentBooks:
        onRecentsOpen();
        break;
      case HomeMenuAction::OpdsBrowser:
        onOpdsBrowserOpen();
        break;
      case HomeMenuAction::ReadingStats:
        onReadingStatsOpen();
        break;
      case HomeMenuAction::Bookmarks:
        onBookmarksOpen();
        break;
      case HomeMenuAction::FileTransfer:
        onFileTransferOpen();
        break;
      case HomeMenuAction::Settings:
        onSettingsOpen();
        break;
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (isMinimalTheme()) {
    renderer.clearScreen();

    if (minimalMenuOpen) {
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
      const auto menuItems = buildMinimalMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks);
      GUI.drawButtonMenu(
          renderer, Rect{0, metrics.homeTopPadding, pageWidth, pageHeight - metrics.homeTopPadding},
          static_cast<int>(menuItems.size()), minimalMenuIndex,
          [&menuItems](int index) { return std::string(menuItems[index].label); },
          [&menuItems](int index) { return menuItems[index].icon; });
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }

    bool bufferRestored = coverBufferStored && restoreCoverBuffer();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

    GUI.drawRecentBookCover(
        renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, recentBooks, selectorIndex,
        coverRendered, coverBufferStored, bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this),
        hasAnyBookStats(currentBookStats) ? &currentBookStats : nullptr, currentBookProgressPercent);

    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }
    MinimalTheme::setHomeButtonHintSelection(minimalHomeNavIndex);
    GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_BROWSE), tr(STR_SETTINGS_SHORT),
                        recentBooks.empty() ? "" : tr(STR_READ));

    renderer.displayBuffer();

    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
      return;
    }

    if (!recentsLoaded && !recentsLoading) {
      recentsLoading = true;
      loadRecentCovers(metrics.homeCoverHeight);
    }
    return;
  }

  // Fast path: pre-rendered frames ready — memcpy + border overlay
  if (carouselFramesReady) {
    uint8_t* frameBuffer = renderer.getFrameBuffer();
    const int bookCount = static_cast<int>(recentBooks.size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int centerIdx = inCarouselRow ? selectorIndex : lastCarouselBookIndex;
    int slotIdx = gCarouselCache.findFrameSlot(centerIdx);

    if (frameBuffer && slotIdx < 0 && gCarouselCache.keyHash != 0 && bookCount > 0) {
      const int evictSlot = chooseCarouselEvictionSlot(centerIdx, bookCount);
      if (evictSlot >= 0 && loadCarouselFrameFromDisk(gCarouselCache.keyHash, bookCount, centerIdx, evictSlot)) {
        slotIdx = evictSlot;
      }
    }

    if (frameBuffer && slotIdx >= 0 && carouselFrames[slotIdx]) {
      memcpy(frameBuffer, carouselFrames[slotIdx], renderer.getBufferSize());
      LyraCarouselTheme::setPreRenderIndex(centerIdx);

      // The cached frame's header row (battery/charging icon, clock) is only as
      // fresh as whenever that frame was captured, so a charger plugged in (or
      // unplugged) after caching would otherwise stay invisible until the frame
      // cache itself is rebuilt. Redraw the header on top of the copied bitmap,
      // same as the slow path below, so it always reflects live state.
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                     metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str()
                                                                                : nullptr);

      GUI.drawCarouselBorder(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                             recentBooks, centerIdx, inCarouselRow);
      if (!inCarouselRow) {
        const auto menuItems = buildHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks);
        if (static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme) ==
            InkMODSettings::UI_THEME::LYRA_CAROUSEL) {
          static_cast<const LyraCarouselTheme&>(GUI).drawButtonMenuSelectionOverlay(
              renderer, static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
              [&menuItems](int index) { return std::string(menuItems[index].label); },
              [&menuItems](int index) { return menuItems[index].icon; });
        }
      }

      renderer.displayBuffer();
      // E-ink refresh complete — pre-render the missing adjacent frame while idle.
      updateSlidingWindowCache(centerIdx, bookCount);
      // Mirror the slow-path trigger: generate missing thumbnails on the second
      // render so the E-ink is already showing something before the SD work starts.
      if (!firstRenderDone) {
        firstRenderDone = true;
        requestUpdate();
      } else if (!recentsLoaded && !recentsLoading) {
        recentsLoading = true;
        loadRecentCovers(metrics.homeCoverHeight);
      }
      return;
    }
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this),
                          hasAnyBookStats(currentBookStats) ? &currentBookStats : nullptr, currentBookProgressPercent);

  auto menuItems = buildSelectableHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks,
                                                metrics.homeContinueReadingInMenu && !recentBooks.empty());

  // Lyra Extended (LYRA_3_COVERS) stacks each cover's title text directly
  // beneath it, wrapped to as many as 4 lines for a long title (see
  // Lyra3CoversTheme::drawRecentBookCover) - the fixed homeCoverTileHeight
  // metric assumes a shorter, more typical title, so a long one could grow
  // taller than that reserved space and run into the menu drawn right
  // below it. Only this theme needs the taller, actually-measured value;
  // every other theme's cover area doesn't grow with title length the
  // same way, so they keep using the plain metric.
  const bool isLyra3Covers =
      static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme) == InkMODSettings::UI_THEME::LYRA_3_COVERS;
  const int coverTileHeight = isLyra3Covers
                                  ? Lyra3CoversTheme::computeCoverTileHeight(renderer, pageWidth, recentBooks)
                                  : metrics.homeCoverTileHeight;

  const int menuStartY = metrics.homeTopPadding + coverTileHeight + metrics.homeMenuTopOffset;
  const int menuEndY = pageHeight - metrics.buttonHintsHeight;
  const int menuHeight = std::max(0, menuEndY - menuStartY);

  GUI.drawButtonMenu(
      renderer, Rect{0, menuStartY, pageWidth, menuHeight}, static_cast<int>(menuItems.size()),
      selectorIndex - getHomeMenuSelectionOffset(recentBooks),
      [&menuItems](int index) { return std::string(menuItems[index].label); },
      [&menuItems](int index) { return menuItems[index].icon; });

  const bool isCarouselTheme =
      static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme) == InkMODSettings::UI_THEME::LYRA_CAROUSEL;
  const auto labels = isCarouselTheme ? mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                                      : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
    return;
  }

  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }

  if (carouselWarmupPending && !carouselFramesReady) {
    // Resolve any missing cover thumbs first, then warm the carousel snapshot.
    // Cover generation needs more contiguous heap than the frame cache path.
    carouselWarmupPending = false;
    const bool showedWarmupProgress = preRenderCarouselFrames(true);
    if (carouselFramesReady || showedWarmupProgress) {
      requestUpdate();
    }
  }
}

void HomeActivity::renderCarouselFrame(int bookIdx, int slotIdx) {
  const auto start = millis();
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer || !gCarouselCache.frames[slotIdx]) return;
  BookReadingStats frameStats;
  float frameProgressPercent = -1.0f;
  bool usedCachedStats = false;
  renderCarouselFrameToCurrentBuffer(bookIdx, &frameStats, &frameProgressPercent, &usedCachedStats);

  memcpy(gCarouselCache.frames[slotIdx], frameBuffer, renderer.getBufferSize());
  gCarouselCache.frameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCarouselCache.frames[slotIdx];
  LOG_DBG("HOME", "carousel: renderCarouselFrame book=%d slot=%d cached=%s took %lums", bookIdx, slotIdx,
          usedCachedStats ? "yes" : "no", millis() - start);
}

void HomeActivity::updateSlidingWindowCache(int centerIdx, int bookCount) {
  (void)centerIdx;
  (void)bookCount;
  // The current carousel cache keeps one frame in RAM; other frames are paged
  // from the SD snapshot cache on demand in render().
}

void HomeActivity::onSelectBook(const std::string& path) {
  gCarouselCache.invalidate();
  freeCarouselFrames();
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
  }
  activityManager.goToReader(path);
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::openSearchResultPath(const std::string& fullPath) {
  // Mirrors FileBrowserActivity's own short-press-open handling for a
  // non-directory entry - a search result found this same file by walking
  // the same directories that screen lists from, so it needs the same
  // "is this actually a raw FB2 (or FB2-in-zip) that has to be converted
  // to an EPUB package first" check before handing off to onSelectBook().
  // Skipping this (calling onSelectBook() directly on the raw .fb2/.zip
  // path) is exactly what made a found FB2 book fail to open - the reader
  // received a path that was never actually a valid EPUB package to begin
  // with, and errored out trying to parse it as one instead.
  const bool isZip = FsHelpers::checkFileExtension(fullPath, ".zip");
  const BookArchiveType archiveType = isZip ? detectBookArchiveType(fullPath) : BookArchiveType::None;
  const bool isFb2 = FsHelpers::checkFileExtension(fullPath, ".fb2") || archiveType == BookArchiveType::Fb2;

  if (isFb2) {
    const std::string cacheBasePath = "/.inkmod";
    const Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    GUI.fillPopupProgress(renderer, popupRect, 0);

    Fb2 fb2Converter(fullPath, cacheBasePath);
    const std::string preparedPackage = fb2Converter.getCachePath() + "/package.epub";
    const std::string preparedMarker = fb2Converter.getCachePath() + "/.browser_prepared_epub";
    if (Storage.exists(preparedPackage.c_str()) && Storage.exists(preparedMarker.c_str())) {
      LOG_INF("FB2", "Opening browser-prepared package for original: %s", fullPath.c_str());
      onSelectBook(preparedPackage);
    } else if (fb2Converter.load([this, popupRect](int percent) { GUI.fillPopupProgress(renderer, popupRect, percent); })) {
      onSelectBook(fb2Converter.getPackagePath());
    } else {
      LOG_ERR("FB2", "Failed to load FB2 file: %s", fullPath.c_str());
    }
  } else if (!isZip || archiveType == BookArchiveType::Epub) {
    onSelectBook(fullPath);
  } else {
    LOG_ERR("Home", "ZIP is neither an EPUB nor an FB2 book: %s", fullPath.c_str());
    GUI.drawPopup(renderer, tr(STR_UNKNOWN_ERROR));
  }
}

void HomeActivity::onSearchFilesOpen() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH_FILES_PROMPT),
                                              "",  // No initial text
                                              64,  // Reasonable query length cap
                                              InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const std::string query = std::get<KeyboardResult>(result.data).text;
        if (query.empty()) return;

        // "/" - the whole card, not just one folder - since the point is
        // finding a book regardless of which directory it ended up in.
        pendingSearchResultPaths = searchBookFiles("/", query);
        LOG_INF("SEARCH", "Query '%s' found %zu results", query.c_str(), pendingSearchResultPaths.size());
        if (pendingSearchResultPaths.empty()) {
          // A toast (same one used for "cache cleared" etc. elsewhere in
          // this app), not a dialog with buttons - there's nothing to
          // confirm or choose here, just something to notice and move on
          // from, so a brief non-blocking message beats making the user
          // press a button to dismiss a button-shaped question that was
          // never really a question.
          BookActions::drawToast(renderer, tr(STR_SEARCH_NO_RESULTS));
          delay(1000);
          requestUpdate();
          // Straight back into another search attempt instead of home -
          // a typo is the most likely reason for zero results, so getting
          // to fix it and retry without detouring through the menu again
          // is the more useful default here.
          onSearchFilesOpen();
          return;
        }

        showSearchResultsPicker();
      });
}

void HomeActivity::showSearchResultsPicker() {
  std::vector<std::string> displayNames;
  displayNames.reserve(pendingSearchResultPaths.size());
  for (const auto& entry : pendingSearchResultPaths) {
    const size_t slashPos = entry.path.find_last_of('/');
    std::string name = slashPos == std::string::npos ? entry.path : entry.path.substr(slashPos + 1);
    // Trailing slash marks a folder result in the list, same convention
    // FileBrowserActivity's own directory rows use.
    if (entry.kind == SearchResultKind::Folder) name += "/";
    displayNames.push_back(std::move(name));
  }

  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "FileSearchResults",
                                                StrId::STR_SEARCH_FILES, std::move(displayNames), 0,
                                                false, false),
      [this](const ActivityResult& pickResult) {
        if (pickResult.isCancelled) {
          pendingSearchResultPaths.clear();
          return;
        }
        const uint8_t index = std::get<OptionSelectionResult>(pickResult.data).index;
        if (index < pendingSearchResultPaths.size()) {
          const SearchResultEntry entry = pendingSearchResultPaths[index];
          pendingSearchResultPaths.clear();
          if (entry.kind == SearchResultKind::Folder) {
            // Straight to that folder in the file browser, not an attempt
            // to open it as a book - it's a folder, not a book file.
            activityManager.goToFileBrowser(entry.path);
          } else {
            openSearchResultPath(entry.path);
          }
        } else {
          pendingSearchResultPaths.clear();
        }
      });
}

void HomeActivity::onContinueReading() {
  if (!recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
  }
}

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onReadingStatsOpen() {
  const int highlightedBookIdx = getHighlightedBookIndex();
  const std::string bookTitle =
      highlightedBookIdx >= 0 ? recentBooks[highlightedBookIdx].title : std::string(tr(STR_READING_STATS));
  const std::string bookPath = getCurrentBookPath();
  const std::string cachePath =
      FsHelpers::hasEpubExtension(bookPath) ? Epub::cachePathForFilePath(bookPath, "/.inkmod") : std::string{};
  if (showAllDevicesStats) {
    startActivityForResult(std::make_unique<BookStatsActivity>(renderer, mappedInput, bookTitle, cachePath,
                                                               currentBookStats, currentBookProgressPercent, false, 0,
                                                               globalStats, allDevicesGlobalStats, true),
                           [this](const ActivityResult& result) {
                             mappedInput.suppressNextConfirmRelease();
                             const auto* statsResult = std::get_if<ReadingStatsResult>(&result.data);
                             if (statsResult && statsResult->changed) {
                               globalStats = GlobalReadingStats::load();
                               showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
                               allDevicesGlobalStats =
                                   showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
                               bookStatsCached = false;
                               updateHighlightedBookContext();
                             }
                             requestUpdate();
                           });
  } else {
    startActivityForResult(
        std::make_unique<BookStatsActivity>(renderer, mappedInput, bookTitle, cachePath, currentBookStats,
                                            currentBookProgressPercent, false, 0, globalStats, true),
        [this](const ActivityResult& result) {
          mappedInput.suppressNextConfirmRelease();
          const auto* statsResult = std::get_if<ReadingStatsResult>(&result.data);
          if (statsResult && statsResult->changed) {
            globalStats = GlobalReadingStats::load();
            showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
            allDevicesGlobalStats = showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
            bookStatsCached = false;
            updateHighlightedBookContext();
          }
          requestUpdate();
        });
  }
}

void HomeActivity::onBookmarksOpen() {
  startActivityForResult(std::make_unique<BookmarksHomeActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}
