#pragma once

class GfxRenderer;
struct Rect;
struct ThemeMetrics;

int headerDateReservedWidth(const GfxRenderer& renderer);
int headerDateLineBottomY(const GfxRenderer& renderer, const ThemeMetrics& metrics, int headerHeight = -1);
void drawHeaderDateAtLineBottom(const GfxRenderer& renderer, int pageWidth, int lineBottomY);
// X3 Classic/Dashboard helper: draw the date on the exact same baseline/font
// as the top-right clock, immediately to its left. Returns true when drawn.
bool drawHeaderDateBeforeClock(const GfxRenderer& renderer, const Rect& headerRect, const ThemeMetrics& metrics);
void drawHeaderDate(const GfxRenderer& renderer, int pageWidth, const ThemeMetrics& metrics, int headerHeight = -1);
