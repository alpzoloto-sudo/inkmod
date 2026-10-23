#pragma once

#include <FreeInkUIGfxRenderer.h>

#include "UITheme.h"

// Compatibility bridge for the CrossPoint X4 Pro FreeInkUI controls.
// inkMOD's ThemeMetrics predates CrossPoint's extra FreeInkUI-only theme
// fields, so keep inkMOD's existing theme structure untouched and apply the
// exact CrossPoint Classic defaults for those newer tokens here.
inline freeink::ui::ThemeTokens uiThemeTokens(const freeink::ui::GfxRendererTarget& target) {
  namespace fui = freeink::ui;
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  fui::ThemeTokens tokens = fui::themeTokensForLineHeight(target.lineHeight(fui::GfxRendererTarget::FONT_BODY));

  // Values copied from CrossPoint 1.6.0 BaseMetrics for fields that do not
  // exist in inkMOD's older ThemeMetrics layout.
  tokens.listRowGap = 0;
  tokens.listRowRadius = 0;
  tokens.listInset = 0;
  tokens.listSidePadding = 20;
  tokens.listSelectionStyle = fui::SelectionStyle::InvertFill;
  tokens.listScrollWidth = 4;
  tokens.listScrollSide = 0;
  tokens.listScrollInset = 0;

  // Keep the band height in sync with inkMOD's active theme while using the
  // CrossPoint Classic defaults for the new header-only tokens.
  tokens.headerHeight = static_cast<int16_t>(metrics.headerHeight);
  tokens.headerSidePadding = 18;
  tokens.headerUnderline = 0;
  tokens.headerTitleAlign = fui::TextAlign::Center;

  // CrossPoint 1.6.0 Classic control-centre defaults.
  tokens.controlRadius = 0;
  tokens.sheetRadius = 0;
  tokens.capsuleRadius = 0;
  tokens.bodyText.bold = false;

  // Touch-first Paper UI controls: larger hit target spacing without extra
  // refresh passes or additional framebuffers.
  if (SETTINGS.uiTheme == InkMODSettings::UI_THEME::PAPER) {
    tokens.listRowGap = 6;
    tokens.listRowRadius = 10;
    tokens.listInset = 4;
    tokens.listSidePadding = 16;
    tokens.headerSidePadding = 16;
    tokens.headerUnderline = 1;
    tokens.headerTitleAlign = fui::TextAlign::Left;
    tokens.controlRadius = 12;
    tokens.sheetRadius = 14;
    tokens.capsuleRadius = 12;
    tokens.bodyText.bold = false;
  }

  return tokens;
}
