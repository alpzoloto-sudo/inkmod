#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  selectorIndex = currentTocIndex >= 0 ? currentTocIndex : epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderChapterSelectionActivity::loop() {
  const int totalItems = getTotalItems();
  constexpr int kChapterJump = 5;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto tocItem = epub->getTocItem(selectorIndex);
    if (tocItem.spineIndex == -1) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    } else {
      setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
      finish();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }

  // Chapter selector: a short press always moves exactly one chapter so the
  // user can land on any TOC entry. Holding any navigation button accelerates
  // movement in fixed groups of five. ButtonNavigator suppresses the release
  // callback after continuous navigation has fired, so a long press does not
  // add an extra +/-1 step when the button is released.
  auto nextOne = [this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  };
  auto previousOne = [this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  };
  auto jumpForward = [this, totalItems] {
    if (totalItems <= 0) return;
    selectorIndex = (selectorIndex + kChapterJump) % totalItems;
    requestUpdate();
  };
  auto jumpBackward = [this, totalItems] {
    if (totalItems <= 0) return;
    selectorIndex = (selectorIndex - (kChapterJump % totalItems) + totalItems) % totalItems;
    requestUpdate();
  };

  buttonNavigator.onNextRelease(nextOne);
  buttonNavigator.onPreviousRelease(previousOne);
  buttonNavigator.onNextContinuous(jumpForward);
  buttonNavigator.onPreviousContinuous(jumpBackward);
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER), nullptr, true);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  const int totalItems = getTotalItems();
  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, totalItems, selectorIndex,
               [this](int index) {
                 auto item = epub->getTocItem(index);
                 std::string indent((item.level - 1) * 2, ' ');
                 return indent + item.title;
               });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
