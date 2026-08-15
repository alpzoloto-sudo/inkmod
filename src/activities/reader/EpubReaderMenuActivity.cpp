#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "InkMODSettings.h"
#include "BookMenuConfig.h"
#include "MappedInputManager.h"
#include "LegacyRenderDiagnostics.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

struct ReaderLayoutSettingsSnapshot {
  uint8_t fontFamily;
  uint8_t fontSize;
  uint8_t sdFontSizeRange;
  uint8_t lineHeightPercent;
  uint8_t orientation;
  uint8_t screenMargin;
  uint8_t readerClockAtBottom;
  uint8_t publisherPageNumbers;
  uint8_t paragraphAlignment;
  uint8_t embeddedStyle;
  uint8_t hyphenationEnabled;
  uint8_t imageRendering;
  uint8_t extraParagraphSpacing;
  uint8_t forceParagraphIndents;
  uint8_t bionicReadingEnabled;
  uint8_t guideReadingEnabled;
  char sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName)] = {};

  bool operator==(const ReaderLayoutSettingsSnapshot& other) const {
    return fontFamily == other.fontFamily && fontSize == other.fontSize && sdFontSizeRange == other.sdFontSizeRange &&
           lineHeightPercent == other.lineHeightPercent && orientation == other.orientation &&
           screenMargin == other.screenMargin && readerClockAtBottom == other.readerClockAtBottom &&
           publisherPageNumbers == other.publisherPageNumbers &&
           paragraphAlignment == other.paragraphAlignment && embeddedStyle == other.embeddedStyle &&
           hyphenationEnabled == other.hyphenationEnabled && imageRendering == other.imageRendering &&
           extraParagraphSpacing == other.extraParagraphSpacing &&
           forceParagraphIndents == other.forceParagraphIndents && bionicReadingEnabled == other.bionicReadingEnabled &&
           guideReadingEnabled == other.guideReadingEnabled &&
           std::strncmp(sdFontFamilyName, other.sdFontFamilyName, sizeof(sdFontFamilyName)) == 0;
  }
  bool operator!=(const ReaderLayoutSettingsSnapshot& other) const { return !(*this == other); }
};

ReaderLayoutSettingsSnapshot captureReaderLayoutSettings() {
  ReaderLayoutSettingsSnapshot snapshot{
      SETTINGS.fontFamily,
      SETTINGS.fontSize,
      SETTINGS.sdFontSizeRange,
      SETTINGS.lineHeightPercent,
      SETTINGS.orientation,
      SETTINGS.screenMargin,
      SETTINGS.readerClockAtBottom,
      SETTINGS.publisherPageNumbers,
      SETTINGS.paragraphAlignment,
      SETTINGS.embeddedStyle,
      SETTINGS.hyphenationEnabled,
      SETTINGS.imageRendering,
      SETTINGS.extraParagraphSpacing,
      SETTINGS.forceParagraphIndents,
      SETTINGS.bionicReadingEnabled,
      SETTINGS.guideReadingEnabled,
  };
  std::strncpy(snapshot.sdFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(snapshot.sdFontFamilyName) - 1);
  snapshot.sdFontFamilyName[sizeof(snapshot.sdFontFamilyName) - 1] = '\0';
  return snapshot;
}

bool haveReaderLayoutSettingsChanged(const ReaderLayoutSettingsSnapshot& before) {
  return before != captureReaderLayoutSettings();
}

}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title, const int currentPage,
    const int totalPages, const int bookProgressPercent, const uint8_t currentOrientation, const bool hasFootnotes,
    const bool hasBookmarks, const bool hasClippings, const bool isCurrentPageBookmarked, const bool isBookCompleted,
    const bool autoPageTurnActive, const uint16_t autoPageTurnIntervalSeconds, const bool showReadingPaceReset)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(
          buildMenuItems(hasFootnotes, hasBookmarks, hasClippings, isCurrentPageBookmarked, isBookCompleted, showReadingPaceReset)),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      autoPageTurnActive(autoPageTurnActive),
      autoPageTurnIntervalSeconds(autoPageTurnIntervalSeconds) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes,
                                                                                     bool hasBookmarks,
                                                                                     bool hasClippings,
                                                                                     bool isCurrentPageBookmarked,
                                                                                     bool isBookCompleted,
                                                                                     bool showReadingPaceReset) {
  std::vector<MenuItem> items;
  const auto layout = loadBookMenuLayout();
  items.reserve(layout.size());
  for (const auto& entry : layout) {
    if (!entry.enabled) continue;
    switch (entry.id) {
      case BookMenuItemId::FOOTNOTES: if (hasFootnotes) items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES}); break;
      case BookMenuItemId::SELECT_CHAPTER: items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER}); break;
      case BookMenuItemId::READER_OPTIONS: items.push_back({MenuAction::READER_OPTIONS, StrId::STR_READER_OPTIONS}); break;
      case BookMenuItemId::CONTROLS_OPTIONS: items.push_back({MenuAction::CONTROLS_OPTIONS, StrId::STR_CAT_CONTROLS}); break;
      case BookMenuItemId::ROTATE_SCREEN: items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION}); break;
      case BookMenuItemId::AUTO_PAGE_TURN: items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_INTERVAL_SECONDS}); break;
      case BookMenuItemId::GO_TO_PERCENT: items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT}); break;
      case BookMenuItemId::BOOKMARK_TOGGLE: items.push_back({MenuAction::BOOKMARK_TOGGLE,isCurrentPageBookmarked?StrId::STR_REMOVE_BOOKMARK:StrId::STR_ADD_BOOKMARK}); break;
      case BookMenuItemId::VIEW_BOOKMARKS: if(hasBookmarks)items.push_back({MenuAction::VIEW_BOOKMARKS,StrId::STR_VIEW_BOOKMARKS}); break;
      case BookMenuItemId::DELETE_BOOKMARKS: if(hasBookmarks)items.push_back({MenuAction::DELETE_BOOKMARKS,StrId::STR_DELETE_BOOKMARKS}); break;
      case BookMenuItemId::SCREENSHOT: items.push_back({MenuAction::SCREENSHOT,StrId::STR_SCREENSHOT_BUTTON}); break;
      case BookMenuItemId::DISPLAY_QR: items.push_back({MenuAction::DISPLAY_QR,StrId::STR_DISPLAY_QR}); break;
      case BookMenuItemId::DELETE_STATS: items.push_back({MenuAction::DELETE_STATS,StrId::STR_DELETE_BOOK_STATS}); break;
      case BookMenuItemId::DELETE_CACHE: items.push_back({MenuAction::DELETE_CACHE,StrId::STR_DELETE_CACHE}); break;
      case BookMenuItemId::RESET_READING_PACE: if(showReadingPaceReset)items.push_back({MenuAction::RESET_READING_PACE,StrId::STR_RESET_READING_PACE}); break;
      case BookMenuItemId::SYNC: items.push_back({MenuAction::SYNC,StrId::STR_SYNC_PROGRESS}); break;
      case BookMenuItemId::READING_STATS: items.push_back({MenuAction::READING_STATS,StrId::STR_READING_STATS}); break;
      case BookMenuItemId::TOGGLE_COMPLETED: items.push_back({MenuAction::TOGGLE_COMPLETED,isBookCompleted?StrId::STR_MARK_UNFINISHED:StrId::STR_MARK_FINISHED}); break;
      case BookMenuItemId::CREATE_CLIPPING: items.push_back({MenuAction::CREATE_CLIPPING,StrId::STR_CREATE_CLIPPING}); break;
      case BookMenuItemId::VIEW_CLIPPINGS: if(hasClippings)items.push_back({MenuAction::VIEW_CLIPPINGS,StrId::STR_CLIPPINGS}); break;
      case BookMenuItemId::DICTIONARY: if(!dictionaryLookupAssignedToButton())items.push_back({MenuAction::DICTIONARY,StrId::STR_DICTIONARY}); break;
      default: break;
    }
  }
  if (items.empty()) items.push_back({MenuAction::READER_OPTIONS, StrId::STR_READER_OPTIONS});
  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::READER_OPTIONS) {
      const auto before = captureReaderLayoutSettings();
      startActivityForResult(std::make_unique<ReaderOptionsActivity>(renderer, mappedInput),
                             [this, before](const ActivityResult&) {
                               settingsChanged = settingsChanged || haveReaderLayoutSettingsChanged(before);
                               pendingOrientation = SETTINGS.orientation;  // sync in case orientation changed
                               requestUpdate();
                             });
      return;
    }

    if (selectedAction == MenuAction::CONTROLS_OPTIONS) {
      startActivityForResult(std::make_unique<ControlsOptionsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, settingsChanged});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    (void)LegacyRenderDiagnostics::feed(LegacyRenderDiagnostics::Key::Back);
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, settingsChanged};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str(), nullptr, true);

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuItems.size(), selectedIndex,
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr,
      [this](int index) -> std::string {
        const auto value = menuItems[index].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          // Render current orientation value on the right edge of the content area.
          return I18N.get(orientationLabels[pendingOrientation]);
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          // Render current page turn value on the right edge of the content area.
          return autoPageTurnActive ? std::to_string(autoPageTurnIntervalSeconds) : "";
        } else {
          return "";
        }
      },
      true);

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
