#include "LegacyRenderPromptActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void LegacyRenderPromptActivity::onEnter() {
  Activity::onEnter();
  ignoreInitialConfirmRelease_ = true;
  requestUpdate(true);
}

void LegacyRenderPromptActivity::loop() {
  if (ignoreInitialConfirmRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Power)) {
      ignoreInitialConfirmRelease_ = false;
      return;
    }

    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.isPressed(MappedInputManager::Button::Power)) {
      ignoreInitialConfirmRelease_ = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;  // Нет
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    ActivityResult result;
    result.isCancelled = false;  // Да
    setResult(std::move(result));
    finish();
  }
}

void LegacyRenderPromptActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  const int cardWidth = std::min(screenWidth - 36, 520);
  const int cardHeight = 230;
  const int cardX = (screenWidth - cardWidth) / 2;
  const int usableBottom = screenHeight - metrics.buttonHintsHeight - 12;
  const int cardY = std::max(28, (usableBottom - cardHeight) / 2);

  renderer.fillRect(cardX, cardY, cardWidth, cardHeight, false);
  renderer.drawRect(cardX, cardY, cardWidth, cardHeight, 2, true);

  renderer.drawCenteredText(UI_10_FONT_ID, cardY + 18, "INKMOD // AFTER DARK", true, EpdFontFamily::BOLD);

  const auto question =
      renderer.wrappedText(UI_12_FONT_ID, "Ты устал в этом бренном мире?", cardWidth - 42, 3, EpdFontFamily::BOLD);

  int y = cardY + 66;
  for (const auto& line : question) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 3;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, cardY + cardHeight - 58, "Back — НЕТ      OK — ДА",
                            true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels("Нет", "Да", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  // Quiet open: no forced FULL_REFRESH. This removes the visible flash/disco
  // while keeping the question screen readable and interactive.
  renderer.displayBuffer();
}
