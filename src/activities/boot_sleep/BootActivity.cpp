#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "AppVersion.h"
#include "fontIds.h"
#include "images/InkMODLogo240.h"

namespace {
void drawInkMODBitmap(const GfxRenderer& renderer, const uint8_t* bitmap, const int width, const int height,
                      const int x, const int y) {
  const int bytesPerRow = (width + 7) / 8;
  for (int row = 0; row < height; ++row) {
    const uint8_t* srcRow = bitmap + row * bytesPerRow;
    int runStart = -1;
    for (int col = 0; col <= width; ++col) {
      const bool black =
          col < width && !((srcRow[col / 8] >> (7 - (col % 8))) & 1);
      if (black && runStart < 0) {
        runStart = col;
      } else if (!black && runStart >= 0) {
        renderer.fillRect(x + runStart, y + row, col - runStart, 1, true);
        runStart = -1;
      }
    }
  }
}
}  // namespace

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int loadingLineH = renderer.getLineHeight(SMALL_FONT_ID);
  constexpr int kLogoLoadingGap = 10;
  const int splashBlockH = INKMODLOGO240_HEIGHT + kLogoLoadingGap + loadingLineH;
  const int logoY = (pageHeight - splashBlockH) / 2;

  drawInkMODBitmap(renderer, InkMODLogo240, INKMODLOGO240_WIDTH, INKMODLOGO240_HEIGHT,
                   (pageWidth - INKMODLOGO240_WIDTH) / 2, logoY);
  renderer.drawCenteredText(SMALL_FONT_ID, logoY + INKMODLOGO240_HEIGHT + kLogoLoadingGap, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, INKMOD_VERSION);
  renderer.displayBuffer();
}
