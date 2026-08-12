#pragma once

#include "components/themes/BaseTheme.h"

struct GlobalReadingStats;

// Dashboard: a real cover (correct aspect ratio, rounded corners) on the left and a
// column of right-aligned reading-stat rows on the right, for the home screen and
// (via drawDashboardSleepScreen) the sleep screen. Geometry and cover-loading are
// ported from CrossInk's DashboardTheme, sized to fit inkMOD's generic
// list-menu-below-the-cover home screen flow (unlike CrossInk, which pairs Dashboard
// with Minimal's full-screen, no-list navigation - porting that too would mean
// reworking HomeActivity's navigation, which this patch intentionally does not touch).
// Everything else (lists, popups, keyboard, settings screens, reader status bar, ...)
// is inherited unchanged from BaseTheme/Classic.
namespace DashboardMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = BaseMetrics::values;
  // Deliberately kept at/under Classic's own homeTopPadding+homeCoverTileHeight
  // (40+370=410) - Dashboard uses the same generic list-menu-below-the-cover
  // home screen flow as Classic, so it needs to leave the same room for the
  // menu and button hints below. Going bigger crowds/clips the menu.
  v.homeTopPadding = 40;
  // Height of the whole cover+cards row on the home screen and on the sleep screen.
  v.homeCoverHeight = 320;
  v.homeCoverTileHeight = 320;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 16;
  return v;
}
constexpr ThemeMetrics values = makeValues();
}  // namespace DashboardMetrics

class DashboardTheme : public BaseTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f) const override;

  // Sleep-screen counterpart of drawRecentBookCover: same cover+stats layout. globalStats
  // is accepted for parity with MinimalTheme::drawStatsSleepScreen() and future use, but
  // isn't drawn yet (see drawStatsColumn). Called directly by SleepActivity (not part of
  // the BaseTheme virtual interface).
  void drawDashboardSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                                const GlobalReadingStats* globalStats, float progressPercent) const;

 private:
  void drawDashboardRow(const GfxRenderer& renderer, Rect rect, const RecentBook* book, bool hasBook,
                        const BookReadingStats* stats, const GlobalReadingStats* globalStats,
                        float progressPercent) const;
  void drawCoverPanel(const GfxRenderer& renderer, Rect rect, const RecentBook* book, bool hasBook) const;
  void drawStatsColumn(const GfxRenderer& renderer, Rect rect, const BookReadingStats* stats,
                       const GlobalReadingStats* globalStats, float progressPercent) const;
};
