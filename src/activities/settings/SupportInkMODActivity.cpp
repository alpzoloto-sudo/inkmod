#include "SupportInkMODActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
constexpr char SUPPORT_URL[] = "https://t.me/inkmodx4";
}

void SupportInkMODActivity::onEnter() {
  Activity::onEnter();
  requestUpdate(true);
}

void SupportInkMODActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void SupportInkMODActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_SUPPORT_INKMOD), nullptr, false);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  constexpr int sideMargin = 18;
  const int textWidth = pageWidth - sideMargin * 2;

  const auto introLines =
      renderer.wrappedText(UI_10_FONT_ID, tr(STR_SUPPORT_INKMOD_TEXT), textWidth, 3, EpdFontFamily::REGULAR);

  int y = contentTop + 4;
  for (const auto& line : introLines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID) + 2;
  }

  y += 6;
  const int footerReserve = renderer.getLineHeight(UI_10_FONT_ID) * 2 + 22;
  const int qrMax = std::max(80, std::min(pageWidth - sideMargin * 2, contentBottom - y - footerReserve));
  const Rect qrBounds((pageWidth - qrMax) / 2, y, qrMax, qrMax);
  QrUtils::drawQrCode(renderer, qrBounds, SUPPORT_URL);

  const int channelY = qrBounds.y + qrBounds.height + 8;
  if (channelY + renderer.getLineHeight(UI_10_FONT_ID) < contentBottom) {
    renderer.drawCenteredText(UI_10_FONT_ID, channelY, "@inkmodx4", true, EpdFontFamily::BOLD);
  }

  const int thanksY = channelY + renderer.getLineHeight(UI_10_FONT_ID) + 5;
  if (thanksY + renderer.getLineHeight(SMALL_FONT_ID) < contentBottom) {
    const auto thanks =
        renderer.wrappedText(SMALL_FONT_ID, tr(STR_SUPPORT_INKMOD_THANKS), textWidth, 2, EpdFontFamily::REGULAR);
    int ty = thanksY;
    for (const auto& line : thanks) {
      renderer.drawCenteredText(SMALL_FONT_ID, ty, line.c_str());
      ty += renderer.getLineHeight(SMALL_FONT_ID) + 2;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
