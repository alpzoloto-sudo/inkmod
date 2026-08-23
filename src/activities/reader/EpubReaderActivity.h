#pragma once
#include <Arduino.h>
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ReaderWork.h>

#include <atomic>
#include <optional>
#include <string>

#include "BookReadingStats.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int activeSectionFontId = 0;
  bool activeSectionUsesFallbackFont = false;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;

  // Status-bar-only cache for a logical TOC chapter that spans several
  // memory-safe spine chunks. It is refreshed only when the active spine or
  // its local page count changes, so normal page turns do not reread SD cache
  // headers.
  mutable int logicalStatusCacheSpineIndex = -1;
  mutable int logicalStatusCacheLocalPageCount = -1;
  mutable int logicalStatusPagesBefore = 0;
  mutable int logicalStatusKnownPageCount = 0;
  // Exact lightweight carry for a sequential transition between internal
  // fragments of one logical chapter. Cache-header recovery remains useful
  // after reopening a book, but must not be the only source of the offset:
  // a previous fragment may have been laid out in another memory mode.
  int logicalPageCarryNextSpine = -1;
  int logicalPageCarryPagesBefore = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  unsigned long pageShownAtMs = 0UL;
  bool paceSampleWarmupPending = true;
  uint32_t sessionPaceSampleSeconds = 0;
  uint16_t sessionPaceSampleCount = 0;
  uint32_t sessionReadingSeconds = 0;
  uint16_t lastAutoPageTurnIntervalSeconds = 0;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  ClippingStore clippings;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  uint16_t pendingBookmarkParagraphIndex = UINT16_MAX;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool longPressMenuHandled = false;
  bool longPressBackHandled = false;
  bool longPowerButtonHandled = false;
  bool sideButtonLongPressHandled = false;
  bool frontButtonLongPressHandled = false;
  int pageLoadRetryCount = 0;
  // One synchronous reader operation at a time. Navigation only invalidates
  // its generation; no background parser or second SD reader is created.
  reader::ReaderWorkController readerWork;
  std::atomic<int32_t> coalescedPageDelta{0};
  std::atomic<int32_t> coalescedSpineDelta{0};
  std::atomic<uint32_t> navigationSettleUntilMs{0};
  enum class BookmarkFeedbackType : uint8_t {
    Added,
    Removed,
    LimitReached,
  };
  bool pendingBookmarkFeedback = false;
  BookmarkFeedbackType bookmarkFeedbackType = BookmarkFeedbackType::Added;
  unsigned long bookmarkFeedbackShowTime = 0UL;
  bool pendingCompletedFeedback = false;
  bool completedFeedbackIsFinished = false;
  unsigned long completedFeedbackShowTime = 0UL;
  bool pendingTiltPageTurnFeedback = false;
  bool tiltPageTurnFeedbackEnabled = false;
  unsigned long tiltPageTurnFeedbackShowTime = 0UL;
  int completionTriggerSpineIndex = -1;
  float completionTriggerSpineProgress = 1.0f;
  bool completionPromptQueued = false;
  bool completionPromptShown = false;
  bool completionTriggerSeenBelow = false;
  bool completionTriggerCrossed = false;
  bool lastAtOrPastCompletionTrigger = false;

  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int fontId, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void rememberLogicalForwardCarry();
  void clearLogicalPageCarry();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void pauseReadingPaceTimer(const char* reason = "unknown");
  void resumeReadingPaceTimer(const char* reason = "unknown");
  void armReadingPaceWarmup(const char* reason = "unknown");
  bool forwardPageReadElapsed(uint32_t& seconds, const char* source) const;
  bool currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const;
  void recordCurrentPageReadingTime(const char* source = "unknown");
  void recordForwardPagePaceSample(uint32_t seconds, const char* source);
  bool getSessionAveragePaceSeconds(uint16_t& avgSeconds) const;
  void recoverStoredPaceFromSession(const char* reason = "unknown");
  bool getTimeLeftPaceSeconds(uint16_t& avgSeconds, const char*& source, uint16_t& sampleCount) const;
  bool estimateRemainingTimeLeftPages(bool bookEstimate, float& remainingPages) const;
  bool estimateProgressTimeLeftSeconds(uint32_t& seconds) const;
  bool estimateTimeLeftSeconds(bool bookEstimate, uint32_t& seconds) const;
  bool formatTimeLeftLabel(char* buf, size_t len) const;
  void resetCurrentBookStatsAfterDelete();
  void showChapterLoadingPopup();
  void openFileTransfer();
  void openAutoPageTurnIntervalPicker(bool ignoreInitialConfirmRelease = false);
  void returnToReaderMenu();
  void resetReadingPaceData();
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void reindexCurrentSection();
  void executeReaderQuickAction(InkMODSettings::LONG_PRESS_MENU_ACTION action);
  void executeFootnoteQuickAction();
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  bool executeShortPowerButtonAction();
  bool executeLongPowerButtonAction();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);

  // Set only when a child screen was entered through the book menu. This keeps
  // the existing behaviour of the same activities when launched by a shortcut.
  bool returnToReaderMenuOnBack = false;
  bool readerMenuRequested = false;
  void applyOrientation(uint8_t orientation);
  void executeLongPressMenuAction();
  void pageTurn(bool isForwardTurn, const char* source = "unknown");
  bool coalesceNavigationWhileLoading();
  void applyCoalescedNavigationIfReady();
  float getCurrentBookProgressPercent() const;
  void initializeCompletionPromptTrigger();
  bool isAtOrPastCompletionTrigger() const;
  bool shouldQueueCompletionPromptOnChapterExit() const;
  void queueCompletionPromptIfNeeded();
  void setBookCompleted(bool isCompleted);
  void showCompletedFeedback(bool isCompleted);
  void showTiltPageTurnFeedback(bool enabled);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

  // A chapter/page-layout transition intentionally drops `section` while the
  // target is rebuilt or reloaded. Previously, a page-turn arriving in the
  // tiny unlocked gap before that render simply reached the `!section` branch
  // in loop(), called requestUpdate(), and was forgotten. Preserve that one
  // navigation event in the existing bounded coalesced queue.
  //
  // Do not duplicate the event which *caused* a normal chapter boundary: the
  // pageTurn() path stamps lastPageTurnTime immediately before requestUpdate().
  // Also leave configured long-press actions alone because Chapter Skip,
  // 10-page skip, font-size and orientation have already handled their input.
  void requestUpdate(bool immediate = false) override {
    if (!section && epub) {
      const unsigned long now = millis();
      const auto turn = ReaderUtils::detectPageTurn(mappedInput);
      const bool triggered = turn.prev || turn.next;
      const bool longPress = !turn.fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
      const bool specialLongPress =
          longPress &&
          (turn.fromSideBtn
               ? SETTINGS.sideButtonLongPress != InkMODSettings::SIDE_LONG_PRESS::SIDE_LONG_OFF
               : SETTINGS.longPressButtonBehavior != InkMODSettings::LONG_PRESS_BUTTON_BEHAVIOR::OFF);

      if (triggered && !specialLongPress && now != lastPageTurnTime) {
        coalescedPageDelta.fetch_add(turn.next ? 1 : -1, std::memory_order_relaxed);
        // A single preserved release does not need the normal 180 ms burst
        // window. Any non-zero deadline lets the existing render path consume
        // it only after the target section/page count exists.
        navigationSettleUntilMs.store(1, std::memory_order_relaxed);
      }
    }
    Activity::requestUpdate(immediate);
  }

  bool preventAutoSleep() override { return automaticPageTurnActive; }
  bool isReaderActivity() const override { return true; }
  bool canSnapshotForSleepOverlay() const override { return true; }
  std::string getCurrentBookPath() const override { return epub ? epub->getPath() : std::string{}; }
  // Same cleanup the in-reader SYNC menu action already does before handing
  // off to KOReaderSyncActivity (see the MenuAction::SYNC case in
  // handleMenuAction()) - free the heavy per-chapter Section now rather than
  // leaving it resident for the whole time a pushed activity runs on top.
  // It gets rebuilt normally from currentSpineIndex/nextPageNumber on return.
  void releaseHeavyResourcesForBackgroundActivity() override {
    if (!section) return;
    RenderLock lock(*this);
    nextPageNumber = section->currentPage;
    section.reset();
  }
  void setAutoPageTurnIntervalSeconds(uint16_t seconds);
  uint16_t getAutoPageTurnIntervalSeconds() const;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  ScreenshotInfo getScreenshotInfo() const override;
};
