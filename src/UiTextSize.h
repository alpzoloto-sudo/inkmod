#pragma once

class GfxRenderer;

// Applies the current InkMODSettings::uiTextSize setting by swapping which
// compiled Inter size backs UI_10_FONT_ID (menu/list body text). The font ID
// itself never changes, so every existing call site
// (renderer.drawText(UI_10_FONT_ID, ...), etc.) picks up the new size
// automatically with no changes elsewhere.
//
// Call once at boot (after the base fonts are registered) and again
// whenever the user changes the setting, so it takes effect immediately
// without a restart.
void applyUiTextSize(GfxRenderer& renderer);

// Font roles for controls which must visibly grow in accessibility mode but
// must not change the reader's fallback typeface.
int uiControlFontId();
int uiHintFontId();
