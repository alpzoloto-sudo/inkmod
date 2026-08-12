

#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

namespace Lyra3CoversMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.menuRowHeight = 46;
  v.homeCoverTileHeight = 300;
  v.homeRecentBooksCount = 3;
  v.keyboardKeyHeight = 50;
  v.keyboardCenteredText = true;
  return v;
}();
}  // namespace Lyra3CoversMetrics

class Lyra3CoversTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f) const override;

  // Computes how tall the cover+title area actually needs to be for the
  // longest-wrapping title among recentBooks, at the given pageWidth -
  // callers positioning content below it (the home menu) can then reserve
  // std::max(Lyra3CoversMetrics::values.homeCoverTileHeight, this) instead
  // of assuming the fixed metric constant is always tall enough. A title
  // long enough to wrap to 3-4 lines (see drawRecentBookCover's own
  // dynamicTitleBoxHeight) can exceed it, which is what let a long title
  // overlap the menu below instead of pushing it down.
  static int computeCoverTileHeight(const GfxRenderer& renderer, int pageWidth,
                                    const std::vector<RecentBook>& recentBooks);
};
