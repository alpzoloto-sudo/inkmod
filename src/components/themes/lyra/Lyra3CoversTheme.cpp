#include "Lyra3CoversTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
}  // namespace

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                           const BookReadingStats* /*stats*/, float /*progressPercent*/) const {
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0;
           i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount); i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath =
              UITheme::getCoverThumbPath(coverPath, Lyra3CoversMetrics::values.homeCoverHeight);

          // First time: load cover from SD and render
          HalFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              float coverHeight = static_cast<float>(bitmap.getHeight());
              float coverWidth = static_cast<float>(bitmap.getWidth());
              float ratio = coverWidth / coverHeight;
              const float tileRatio = static_cast<float>(tileWidth - 2 * hPaddingInSelection) /
                                      static_cast<float>(Lyra3CoversMetrics::values.homeCoverHeight);
              float cropX = 1.0f - (tileRatio / ratio);

              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection, Lyra3CoversMetrics::values.homeCoverHeight,
                                  cropX);
            } else {
              hasCover = false;
            }
            file.close();
          }
        }
        // Draw either way
        renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                          Lyra3CoversMetrics::values.homeCoverHeight, true);

        if (!hasCover) {
          // Render empty cover
          renderer.fillRect(tileX + hPaddingInSelection,
                            tileY + hPaddingInSelection + (Lyra3CoversMetrics::values.homeCoverHeight / 3),
                            tileWidth - 2 * hPaddingInSelection, 2 * Lyra3CoversMetrics::values.homeCoverHeight / 3,
                            true);
          renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount);
         i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;

      // Увеличиваем максимальную ширину текста, убираем лишние отступы
      const int maxLineWidth = tileWidth - hPaddingInSelection * 2;

      // Для русского текста используем больше строк, но меньший шрифт если нужно
      int fontId = SMALL_FONT_ID;
      int maxLines = 3;
      
      // Проверяем длину названия - если оно длинное, используем UI_10_FONT_ID
      std::string title = recentBooks[i].title;
      int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      if (titleWidth > maxLineWidth * 2) {
        fontId = UI_10_FONT_ID;
        maxLines = 4;
      }

      auto titleLines = renderer.wrappedText(fontId, recentBooks[i].title.c_str(), maxLineWidth, maxLines);

      const int titleLineHeight = renderer.getLineHeight(fontId);
      const int dynamicBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight;
      const int dynamicTitleBoxHeight = dynamicBlockHeight + hPaddingInSelection + 5;

      if (bookSelected) {
        // Draw selection box
        renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                                 Color::LightGray);
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                                Lyra3CoversMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, Lyra3CoversMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRoundedRect(tileX, tileY + Lyra3CoversMetrics::values.homeCoverHeight + hPaddingInSelection,
                                 tileWidth, dynamicTitleBoxHeight, cornerRadius, false, false, true, true,
                                 Color::LightGray);
      }

      int currentY = tileY + Lyra3CoversMetrics::values.homeCoverHeight + hPaddingInSelection + 5;
      for (const auto& line : titleLines) {
        renderer.drawText(fontId, tileX + hPaddingInSelection, currentY, line.c_str(), true);
        currentY += titleLineHeight;
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

int Lyra3CoversTheme::computeCoverTileHeight(const GfxRenderer& renderer, int pageWidth,
                                             const std::vector<RecentBook>& recentBooks) {
  const int baseHeight = Lyra3CoversMetrics::values.homeCoverTileHeight;
  if (recentBooks.empty()) return baseHeight;

  const int tileWidth = (pageWidth - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int maxLineWidth = tileWidth - hPaddingInSelection * 2;
  int tallestNeeded = baseHeight;

  for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount);
       i++) {
    // Mirrors drawRecentBookCover()'s own font/line-count selection - kept
    // in sync manually since duplicating a few lines here is simpler than
    // restructuring drawRecentBookCover() (a const override on the base
    // class's virtual signature) to expose this as a byproduct of drawing.
    int fontId = SMALL_FONT_ID;
    int maxLines = 3;
    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, recentBooks[i].title.c_str());
    if (titleWidth > maxLineWidth * 2) {
      fontId = UI_10_FONT_ID;
      maxLines = 4;
    }
    auto titleLines = renderer.wrappedText(fontId, recentBooks[i].title.c_str(), maxLineWidth, maxLines);
    const int titleLineHeight = renderer.getLineHeight(fontId);
    const int dynamicTitleBoxHeight =
        static_cast<int>(titleLines.size()) * titleLineHeight + hPaddingInSelection + 5;
    const int neededForThisTile =
        Lyra3CoversMetrics::values.homeCoverHeight + hPaddingInSelection + dynamicTitleBoxHeight;
    tallestNeeded = std::max(tallestNeeded, neededForThisTile);
  }
  return tallestNeeded;
}