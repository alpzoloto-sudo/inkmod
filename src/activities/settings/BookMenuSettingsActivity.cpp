#include "BookMenuSettingsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BookMenuSettingsActivity::onEnter() {
  Activity::onEnter();
  layout = loadBookMenuLayout();
  requestUpdate();
}

void BookMenuSettingsActivity::persist() {
  (void)saveBookMenuLayout(layout);
}

void BookMenuSettingsActivity::moveSelected(const int d) {
  const int target = selectedIndex + d;
  if (target < 0 || target >= static_cast<int>(layout.size())) return;

  std::swap(layout[selectedIndex], layout[target]);
  selectedIndex = target;
  persist();
  requestUpdate();
}

void BookMenuSettingsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !layout.empty()) {
    layout[selectedIndex].enabled = !layout[selectedIndex].enabled;
    persist();
    requestUpdate();
    return;
  }

  const uint32_t now = millis();

  // Short Up/Down navigates. Holding Up/Down moves the actual menu item.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    upPressedAt = now;
    upMoveHandled = false;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    downPressedAt = now;
    downMoveHandled = false;
  }

  if (!upMoveHandled && upPressedAt && mappedInput.isPressed(MappedInputManager::Button::Up) &&
      now - upPressedAt >= MOVE_HOLD_MS) {
    upMoveHandled = true;
    moveSelected(-1);
  }
  if (!downMoveHandled && downPressedAt && mappedInput.isPressed(MappedInputManager::Button::Down) &&
      now - downPressedAt >= MOVE_HOLD_MS) {
    downMoveHandled = true;
    moveSelected(1);
  }

  // Keep the older Left/Right hold shortcut for compatibility.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    leftPressedAt = now;
    leftMoveHandled = false;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    rightPressedAt = now;
    rightMoveHandled = false;
  }
  if (!leftMoveHandled && leftPressedAt && mappedInput.isPressed(MappedInputManager::Button::Left) &&
      now - leftPressedAt >= MOVE_HOLD_MS) {
    leftMoveHandled = true;
    moveSelected(-1);
  }
  if (!rightMoveHandled && rightPressedAt && mappedInput.isPressed(MappedInputManager::Button::Right) &&
      now - rightPressedAt >= MOVE_HOLD_MS) {
    rightMoveHandled = true;
    moveSelected(1);
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (!upMoveHandled && !layout.empty()) {
      selectedIndex = (selectedIndex + static_cast<int>(layout.size()) - 1) % static_cast<int>(layout.size());
      requestUpdate();
    }
    upPressedAt = 0;
    upMoveHandled = false;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (!downMoveHandled && !layout.empty()) {
      selectedIndex = (selectedIndex + 1) % static_cast<int>(layout.size());
      requestUpdate();
    }
    downPressedAt = 0;
    downMoveHandled = false;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (!leftMoveHandled && !layout.empty()) {
      selectedIndex = (selectedIndex + static_cast<int>(layout.size()) - 1) % static_cast<int>(layout.size());
      requestUpdate();
    }
    leftPressedAt = 0;
    leftMoveHandled = false;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (!rightMoveHandled && !layout.empty()) {
      selectedIndex = (selectedIndex + 1) % static_cast<int>(layout.size());
      requestUpdate();
    }
    rightPressedAt = 0;
    rightMoveHandled = false;
  }
}

void BookMenuSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_BOOK_MENU_SETTINGS), nullptr, false);

  // Hard-reserved help band above the hardware button hints.
  // drawButtonHints() owns exactly the bottom buttonHintsHeight pixels, so
  // keeping this band above that boundary guarantees it cannot be covered.
  constexpr int HELP_BAND_HEIGHT = 38;
  constexpr int HELP_GAP = 4;
  const int helpBottom = pageHeight - metrics.buttonHintsHeight;
  const int helpTop = helpBottom - HELP_BAND_HEIGHT;

  const int listHeight =
      std::max(0, helpTop - HELP_GAP - top);

  GUI.drawList(
      renderer, Rect{0, top, pageWidth, listHeight},
      static_cast<int>(layout.size()), selectedIndex,
      [this](int i) { return std::string(I18N.get(bookMenuItemLabel(layout[i].id))); },
      nullptr, nullptr,
      [this](int i) {
        if (layout[i].id == BookMenuItemId::DICTIONARY && dictionaryLookupAssignedToButton()) {
          return std::string(tr(STR_ASSIGNED_TO_BUTTON));
        }
        return std::string(layout[i].enabled ? tr(STR_SHOW) : tr(STR_HIDE));
      },
      true);

  // Paint the help band after the list so even a theme/list renderer cannot
  // leave old pixels or scrollbar fragments under the hint.
  renderer.fillRect(0, helpTop, pageWidth, HELP_BAND_HEIGHT, false);
  renderer.drawLine(10, helpTop, pageWidth - 10, helpTop, true);

  constexpr const char* HELP_TEXT = "Удерживайте Вверх/Вниз для перемещения";
  const auto helpLines = renderer.wrappedText(
      SMALL_FONT_ID, HELP_TEXT, pageWidth - 24, 2, EpdFontFamily::REGULAR);

  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int gap = 2;
  const int blockHeight =
      static_cast<int>(helpLines.size()) * lineHeight +
      std::max(0, static_cast<int>(helpLines.size()) - 1) * gap;
  int y = helpTop + std::max(1, (HELP_BAND_HEIGHT - blockHeight) / 2);

  for (const auto& line : helpLines) {
    renderer.drawCenteredText(SMALL_FONT_ID, y, line.c_str());
    y += lineHeight + gap;
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
