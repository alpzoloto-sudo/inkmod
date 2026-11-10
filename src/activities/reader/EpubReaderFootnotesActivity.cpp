#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "InkMODHumor.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kFootnoteRowHeight = 52;
constexpr int kFootnoteListTop = 60;
constexpr int kFootnoteTextInset = 32;
}  // namespace

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderFootnotesActivity::onExit() { Activity::onExit(); }

void EpubReaderFootnotesActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(footnotes.size())) {
      setResult(FootnoteResult{footnotes[selectedIndex].href});
      finish();
    }
    return;
  }

  if (!footnotes.empty() && mappedInput.hasTouch()) {
    const auto orientation = renderer.getOrientation();
    const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
    const int contentY = isPortraitInverted ? 50 : 0;
    const int listTop = kFootnoteListTop + contentY;
    const int listHeight = std::max(1, renderer.getScreenHeight() - listTop -
                                         UITheme::getInstance().getMetrics().buttonHintsHeight);
    const int visibleCount = std::max(1, listHeight / kFootnoteRowHeight);

    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int maxOffset = std::max(0, static_cast<int>(footnotes.size()) - visibleCount);
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleCount : -visibleCount;
      scrollOffset = std::clamp(scrollOffset + delta, 0, maxOffset);
      // Keep a visible selection so Confirm still operates predictably after a touch scroll.
      selectedIndex = std::clamp(selectedIndex, scrollOffset,
                                 std::min(static_cast<int>(footnotes.size()) - 1,
                                          scrollOffset + visibleCount - 1));
      requestUpdate();
      return;
    }

    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty) && ty >= listTop && ty < listTop + listHeight) {
      const int row = (ty - listTop) / kFootnoteRowHeight;
      const int index = scrollOffset + row;
      if (row >= 0 && row < visibleCount && index >= 0 && index < static_cast<int>(footnotes.size())) {
        selectedIndex = index;
        mappedInput.suppressTouchContact();
        setResult(FootnoteResult{footnotes[selectedIndex].href});
        finish();
        return;
      }
    }
  }

  buttonNavigator.onNext([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex + 1) % footnotes.size();
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex - 1 + footnotes.size()) % footnotes.size();
      requestUpdate();
    }
  });
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_FOOTNOTES), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_FOOTNOTES), true, EpdFontFamily::BOLD);

  if (footnotes.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 90 + contentY, tr(STR_NO_FOOTNOTES));
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int jokeTop = 128 + contentY;
    const int jokeBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - 8;
    InkMODHumor::drawBounded(renderer, InkMODHumor::Moment::NoFootnotes, contentX, jokeTop, contentWidth,
                             std::max(0, jokeBottom - jokeTop), SMALL_FONT_ID, true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int marginLeft = contentX + kFootnoteTextInset;
  const int listTop = kFootnoteListTop + contentY;
  const int listHeight = std::max(1, renderer.getScreenHeight() - listTop -
                                       UITheme::getInstance().getMetrics().buttonHintsHeight);
  const int visibleCount = std::max(1, listHeight / kFootnoteRowHeight);
  if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
  if (selectedIndex >= scrollOffset + visibleCount) scrollOffset = selectedIndex - visibleCount + 1;

  for (int i = scrollOffset; i < static_cast<int>(footnotes.size()) && i < scrollOffset + visibleCount; i++) {
    const int y = listTop + (i - scrollOffset) * kFootnoteRowHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(0, y, screenWidth, kFootnoteRowHeight, true);
    }

    // Show footnote number and abbreviated href
    std::string label = footnotes[i].number;
    if (label.empty()) {
      label = tr(STR_LINK);
    }
    renderer.drawText(UI_10_FONT_ID, marginLeft, y + 12, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
