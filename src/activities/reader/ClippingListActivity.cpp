#include "ClippingListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include "MappedInputManager.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ROW_HEIGHT = 78;
constexpr int LIST_START_Y = 58;
constexpr unsigned long DELETE_HOLD_MS = 1000;

// Clipping text intentionally keeps line breaks for My Clippings.txt.
// The compact list row is single-line, though, and drawText() treats '\n'
// and other control whitespace as glyphs on some SD fonts, producing the
// replacement diamond. Flatten whitespace only for list presentation.
std::string singleLineSnippet(const char* text) {
  std::string out;
  if (!text) return out;
  out.reserve(160);

  bool pendingSpace = false;
  for (const unsigned char ch : std::string(text)) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      pendingSpace = !out.empty();
      continue;
    }

    if (pendingSpace) {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(static_cast<char>(ch));
  }

  // Collapse repeated ASCII spaces. UTF-8 bytes are otherwise copied intact.
  size_t write = 0;
  bool previousSpace = false;
  for (size_t read = 0; read < out.size(); ++read) {
    const bool isSpace = out[read] == ' ';
    if (isSpace && previousSpace) continue;
    out[write++] = out[read];
    previousSpace = isSpace;
  }
  out.resize(write);

  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}
}

void ClippingListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  requestUpdate(true);
}

int ClippingListActivity::pageItems() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int available = renderer.getScreenHeight() - LIST_START_Y - metrics.buttonHintsHeight - 6;
  return std::max(1, available / ROW_HEIGHT);
}

void ClippingListActivity::openDeleteMenu(const bool ignoreInitialConfirmRelease) {
  const auto& items = store_.getClippings();
  if (items.empty() || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items.size())) return;
  const size_t selected = static_cast<size_t>(selectedIndex_);
  std::vector<FileBrowserActionActivity::MenuItem> menu;
  menu.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});
  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, tr(STR_CLIPPINGS), std::move(menu),
                                                  ignoreInitialConfirmRelease),
      [this, selected](const ActivityResult& result) {
        longPressHandled_ = false;
        if (!result.isCancelled) {
          const auto* action = std::get_if<FileBrowserActionResult>(&result.data);
          if (action && static_cast<FileBrowserAction>(action->action) == FileBrowserAction::Delete) {
            store_.removeAt(selected);
            const auto size = static_cast<int>(store_.getClippings().size());
            selectedIndex_ = size == 0 ? 0 : std::min(selectedIndex_, size - 1);
          }
        }
        requestUpdate();
      });
}

void ClippingListActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  const auto& clips = store_.getClippings();
  if (!clips.empty() && !longPressHandled_ && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= DELETE_HOLD_MS) {
    longPressHandled_ = true;
    openDeleteMenu(true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressHandled_) {
      longPressHandled_ = false;
      return;
    }
    if (!clips.empty() && selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(clips.size())) {
      const auto& clip = clips[static_cast<size_t>(selectedIndex_)];
      setResult(ClippingJumpResult{clip.spineIndex, clip.pageNumber});
      finish();
    }
    return;
  }

  const int total = static_cast<int>(clips.size());
  if (total == 0) return;
  const int perPage = pageItems();
  navigator_.onNextRelease([this, total] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, total);
    requestUpdate();
  });
  navigator_.onPreviousRelease([this, total] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, total);
    requestUpdate();
  });
  navigator_.onNextContinuous([this, total, perPage] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, total, perPage);
    requestUpdate();
  });
  navigator_.onPreviousContinuous([this, total, perPage] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, total, perPage);
    requestUpdate();
  });
}

void ClippingListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_CLIPPINGS), nullptr, true);

  const auto& clips = store_.getClippings();
  if (clips.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, LIST_START_Y + 28, tr(STR_NO_CLIPPINGS));
  } else {
    const int perPage = pageItems();
    const int first = (selectedIndex_ / perPage) * perPage;
    for (int row = 0; row < perPage; ++row) {
      const int index = first + row;
      if (index >= static_cast<int>(clips.size())) break;
      const int y = LIST_START_Y + row * ROW_HEIGHT;
      const bool selected = index == selectedIndex_;
      if (selected) renderer.fillRect(0, y, renderer.getScreenWidth() - 1, ROW_HEIGHT, true);

      const auto& clip = clips[static_cast<size_t>(index)];
      const std::string flattened = singleLineSnippet(clip.text);
      const auto snippet =
          renderer.truncatedText(UI_10_FONT_ID, flattened.c_str(), renderer.getScreenWidth() - 38);
      renderer.drawText(UI_10_FONT_ID, 18, y + 6, snippet.c_str(), !selected);

      const char* chapter = clip.chapterTitle[0] ? clip.chapterTitle : tr(STR_UNKNOWN_CHAPTER);
      const auto chapterText = renderer.truncatedText(SMALL_FONT_ID, chapter, renderer.getScreenWidth() - 120);
      renderer.drawText(SMALL_FONT_ID, 18, y + 34, chapterText.c_str(), !selected);

      char page[32];
      if (clip.endPageNumber > clip.pageNumber) {
        snprintf(page, sizeof(page), "%u-%u/%u", static_cast<unsigned>(clip.pageNumber + 1),
                 static_cast<unsigned>(clip.endPageNumber + 1),
                 static_cast<unsigned>(std::max<uint16_t>(clip.pageCount, 1)));
      } else {
        snprintf(page, sizeof(page), "%u/%u", static_cast<unsigned>(clip.pageNumber + 1),
                 static_cast<unsigned>(std::max<uint16_t>(clip.pageCount, 1)));
      }
      renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - 88, y + 34, page, !selected);
      renderer.drawText(SMALL_FONT_ID, 18, y + 55, tr(STR_CLIPPING_DELETE_HINT), !selected);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), clips.empty() ? "" : tr(STR_OPEN),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
