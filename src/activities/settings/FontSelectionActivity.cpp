#include "FontSelectionActivity.h"

#include <GfxRenderer.h>
#include <FontCacheManager.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <I18n.h>
#include <Logging.h>

#include "InkMODSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
uint8_t closestSizeIndex(const std::vector<uint8_t>& sizes, const uint8_t targetPointSize) {
  if (sizes.empty()) return 0;

  uint8_t bestIndex = 0;
  uint8_t bestDiff = UINT8_MAX;
  for (size_t i = 0; i < sizes.size(); i++) {
    const uint8_t size = sizes[i];
    const uint8_t diff = size > targetPointSize ? size - targetPointSize : targetPointSize - size;
    if (diff < bestDiff || (diff == bestDiff && size < sizes[bestIndex])) {
      bestIndex = static_cast<uint8_t>(i);
      bestDiff = diff;
    }
  }
  return bestIndex;
}





// Draw the SD-card preview directly from the currently loaded .cpfont mini-cache.
// This intentionally bypasses EpdFontFamily fallback selection: the picker must
// show the highlighted face itself, not the UI fallback. The normal reader path
// remains unchanged.
bool drawDirectSdPreviewLine(GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text) {
  const auto& sdFonts = renderer.getSdCardFonts();
  const auto it = sdFonts.find(fontId);
  if (it == sdFonts.end() || !it->second || !text || !*text) return false;

  SdCardFont* sdFont = it->second;
  EpdFont* epd = sdFont->getEpdFont(0);
  if (!epd || !epd->data) return false;
  const EpdFontData* data = epd->data;
  const int baselineY = y + data->ascender;
  int cursorX = x;
  uint32_t prevCp = 0;

  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    uint32_t cp = utf8NextCodepoint(&cursor);
    if (!cp) break;

    const EpdGlyph* glyph = epd->findGlyph(cp);
    if (!glyph) {
      // The preview is diagnostic by design: do not silently substitute DejaVu.
      // A genuinely missing glyph is rendered as a small gap.
      cursorX += 4;
      prevCp = 0;
      continue;
    }

    if (prevCp != 0) cursorX += fp4::toPixel(epd->getKerning(prevCp, cp));

    const uint8_t* bitmap = renderer.getGlyphBitmap(data, glyph);
    if (bitmap && glyph->width && glyph->height) {
      int pixelPosition = 0;
      const int gx0 = cursorX + glyph->left;
      const int gy0 = baselineY - glyph->top;
      if (data->is2Bit) {
        for (int gy = 0; gy < glyph->height; ++gy) {
          for (int gx = 0; gx < glyph->width; ++gx, ++pixelPosition) {
            const uint8_t byte = bitmap[pixelPosition >> 2];
            const uint8_t shift = (3 - (pixelPosition & 3)) * 2;
            const uint8_t level = (byte >> shift) & 0x3;
            if (level != 0) renderer.drawPixel(gx0 + gx, gy0 + gy, true);
          }
        }
      } else {
        for (int gy = 0; gy < glyph->height; ++gy) {
          for (int gx = 0; gx < glyph->width; ++gx, ++pixelPosition) {
            const uint8_t byte = bitmap[pixelPosition >> 3];
            const uint8_t shift = 7 - (pixelPosition & 7);
            if ((byte >> shift) & 1) renderer.drawPixel(gx0 + gx, gy0 + gy, true);
          }
        }
      }
    }

    cursorX += fp4::toPixel(glyph->advanceX);
    prevCp = cp;
  }
  return true;
}

uint8_t currentFontPointSize(const SdCardFontRegistry* registry) {
  if (SETTINGS.sdFontFamilyName[0] == '\0' && SETTINGS.fontFamily == InkMODSettings::TEST_FONTS) {
    return 12;
  }

  if (registry && SETTINGS.sdFontFamilyName[0] != '\0') {
    const SdCardFontFamilyInfo* family = registry->findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      const std::vector<uint8_t> sizes = family->availableSizes();
      if (!sizes.empty()) {
        const uint8_t index =
            SETTINGS.fontSize < sizes.size() ? SETTINGS.fontSize : static_cast<uint8_t>(sizes.size() - 1);
        return sizes[index];
      }
    }
  }
  return InkMODSettings::getReaderFontPointSize(SETTINGS.getEffectiveReaderFontSize());
}
}  // namespace

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry)
    : Activity("FontSelect", renderer, mappedInput), registry_(registry) {}

void FontSelectionActivity::onEnter() {
  Activity::onEnter();

  // CrossPoint's live-preview implementation deliberately previews by
  // changing SETTINGS, then restores the old values on Back. Keep an exact
  // snapshot before the first highlighted family is applied.
  savedFontFamily_ = SETTINGS.fontFamily;
  savedFontSize_ = SETTINGS.fontSize;
  savedSdFontFamilyName_ = SETTINGS.sdFontFamilyName;
  previewTargetPointSize_ = currentFontPointSize(registry_);
  selectionCommitted_ = false;

  // DejaVu Sans is the built-in reader font. SD-card families follow it so
  // the same picker works whether or not the user has installed extra fonts.
  fonts_.clear();
  fonts_.reserve(1 + (registry_ ? registry_->getFamilyCount() : 0));
  fonts_.push_back({"DejaVu Sans", true, static_cast<uint8_t>(InkMODSettings::TEST_FONTS)});

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(InkMODSettings::BUILTIN_FONT_COUNT + i)});
    }
  }

  // Find current selection. DejaVu Sans is index 0; SD-card families start at 1.
  selectedIndex_ = 0;
  if (SETTINGS.sdFontFamilyName[0] != '\0' && registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == SETTINGS.sdFontFamilyName) {
        selectedIndex_ = i + 1;
        break;
      }
    }
  }

  updatePreviewFont();
  requestUpdate();
}

void FontSelectionActivity::restoreSavedFontSettings() {
  SETTINGS.fontFamily = savedFontFamily_;
  SETTINGS.fontSize = savedFontSize_;
  std::strncpy(SETTINGS.sdFontFamilyName, savedSdFontFamilyName_.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
  SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
}

void FontSelectionActivity::onExit() {
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

  // Highlighting is only a temporary preview. Back/cancel must leave the
  // actual reader font untouched; Confirm marks the current SETTINGS as the
  // committed selection and skips this restore.
  if (!selectionCommitted_) {
    restoreSavedFontSettings();
  }

  sdFontSystem.releaseLoadedFont(renderer);
  sdFontSystem.ensureLoaded(renderer);
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();
  previewedIndex_ = -1;
  previewFontId_ = 0;
  Activity::onExit();
}

// Font switching runs on the main task from loop(), which otherwise holds no
// RenderLock. ensureLoaded() deletes the resident SdCardFont before loading the
// next one, while the render task walks that same object inside render()'s
// prewarm/direct-draw call - so without this lock a font switch can free the
// mini glyph arrays out from under the render task (or simply have the render
// task draw a font that's already been swapped out again, which is why the
// preview looked "stuck" on one face: the update kept racing ahead of the
// paint that was supposed to show it). Mirrors CrossPoint's applyFamily().
void FontSelectionActivity::updatePreviewFont() {
  if (selectedIndex_ == previewedIndex_ || fonts_.empty()) return;
  previewedIndex_ = selectedIndex_;

  RenderLock lock;

  const auto& font = fonts_[selectedIndex_];
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

  // Do not use a separate preview-only font slot. CrossPoint #2349 fixed its
  // preview by mutating SETTINGS even for built-in fonts. This makes the
  // preview exercise exactly the same family/size resolver as a book page.
  SETTINGS.fontFamily = InkMODSettings::TEST_FONTS;

  if (font.isBuiltin) {
    const uint8_t stored = InkMODSettings::getStoredReaderFontSize(InkMODSettings::SMALL);
    SETTINGS.fontSize = stored == UINT8_MAX ? 0 : stored;
    SETTINGS.sdFontFamilyName[0] = '\0';
  } else if (registry_) {
    const int sdIdx = font.settingIndex - InkMODSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < 0 || sdIdx >= static_cast<int>(families.size())) {
      previewFontId_ = TEST_FONTS_12_FONT_ID;
      LOG_ERR("FONT", "Preview family index invalid: %s", font.name.c_str());
      return;
    }

    const auto sizes = families[sdIdx].availableSizes();
    SETTINGS.fontSize = closestSizeIndex(sizes, previewTargetPointSize_);
    std::strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
    SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  }

  // This is the normal reader hand-off. ensureLoaded() unloads the old SD
  // family and loads the highlighted family according to SETTINGS.
  sdFontSystem.ensureLoaded(renderer);
  previewFontId_ = SETTINGS.getReaderFontId();
  if (previewFontId_ == 0) previewFontId_ = TEST_FONTS_12_FONT_ID;

  if (font.isBuiltin) {
    LOG_INF("FONT", "Preview active via SETTINGS builtin=%s id=%d", font.name.c_str(), previewFontId_);
  } else {
    LOG_INF("FONT", "Preview active via SETTINGS family=%s id=%d sizeStep=%u", SETTINGS.sdFontFamilyName,
            previewFontId_, static_cast<unsigned>(SETTINGS.fontSize));
  }
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();
}

void FontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    mappedInput.suppressNextBackRelease();
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int listSize = static_cast<int>(fonts_.size());
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    updatePreviewFont();
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    updatePreviewFont();
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    updatePreviewFont();
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    updatePreviewFont();
    requestUpdate();
  });
}

void FontSelectionActivity::handleSelection() {
  if (fonts_.empty()) {
    finish();
    return;
  }

  // The highlighted family is already applied to SETTINGS by
  // updatePreviewFont(). Commit that exact state instead of translating the
  // selection through a second code path. This mirrors CrossPoint's preview
  // design and guarantees preview == reader selection.
  const auto& font = fonts_[selectedIndex_];
  const int appliedFontId = SETTINGS.getReaderFontId();
  if (!font.isBuiltin &&
      (SETTINGS.sdFontFamilyName[0] == '\0' || appliedFontId == 0 || !renderer.isSdCardFont(appliedFontId))) {
    LOG_ERR("FONT", "Highlighted family is not active at commit: %s id=%d", font.name.c_str(), appliedFontId);
    requestUpdate(true);
    return;
  }

  selectionCommitted_ = true;
  SETTINGS.saveToFile();
  LOG_INF("FONT", "Committed reader font family=%s id=%d sizeStep=%u",
          SETTINGS.sdFontFamilyName[0] ? SETTINGS.sdFontFamilyName : "DejaVu Sans", appliedFontId,
          static_cast<unsigned>(SETTINGS.fontSize));
  finish();
}

void FontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safeArea =
      UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);

  GUI.drawHeader(renderer, Rect{safeArea.x, metrics.topPadding, safeArea.width, metrics.headerHeight},
                 tr(STR_FONT_FAMILY));

  // CrossPoint #2349 uses a translated pangram rather than a language switch
  // embedded in the activity. Keep the sample tied to the interface i18n.
  const char* previewText = tr(STR_FONT_PREVIEW_TEXT);

  // Fixed-height preview band: always reserves the same two-line space
  // regardless of which font/text is shown, so switching the highlighted
  // font never shifts the list below it - unlike shrinking/growing the
  // preview to fit its content, which would otherwise shove every row down
  // by a different amount on every keypress.
  const int previewFontId = previewFontId_ != 0 ? previewFontId_ : TEST_FONTS_12_FONT_ID;

  // IMPORTANT: do not full-prewarm the SD font yet. wrappedText() performs
  // metadata-only measurement and may rebuild the SD mini-cache. A full
  // prewarm done before wrapping would therefore be discarded before drawText,
  // leaving only metrics from the selected family while glyph bitmaps fall back
  // to the UI font. Match the reader pipeline instead: layout first, then full
  // glyph prewarm, then draw with no further measuring calls.

  const int previewLineHeight = std::max(1, renderer.getLineHeight(previewFontId));
  const int previewTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  constexpr int previewPadding = 8;
  constexpr int previewHeight = 112;  // Fixed band large enough for two lines of every preview face.
  const int previewInnerHeight = previewHeight - previewPadding * 2;
  const int maxPreviewLines = 2;
  const int previewTextWidth = std::max(1, safeArea.width - 2 * previewPadding);

  renderer.fillRect(safeArea.x, previewTop, safeArea.width, previewHeight, false);
  if (renderer.isSdCardFont(previewFontId)) {
    // CrossPoint measures with the selected reader face, not the UI fallback.
    // Populate only the compact advance table before wrapping; bitmap glyphs are
    // loaded once, after the final lines are known.
    renderer.ensureSdCardFontReady(previewFontId, previewText, 0x01);
  }
  const auto previewLines = renderer.wrappedText(previewFontId, previewText, previewTextWidth, maxPreviewLines,
                                                 EpdFontFamily::REGULAR);

  // Render the sample through the exact same two-pass font-cache pipeline as
  // the reader. The first pass records drawText() calls without touching the
  // framebuffer, endScanAndPrewarm() loads the real SD-card glyph bitmaps, and
  // the second pass draws them. A manual prewarm here is subtly different and
  // was the reason preview metrics changed while the visible face stayed on
  // the fallback font.
  const int usedTextHeight = static_cast<int>(previewLines.size()) * previewLineHeight;
  const int previewStartY = previewTop + previewPadding + std::max(0, (previewInnerHeight - usedTextHeight) / 2);
  const int previewBottom = previewTop + previewHeight - previewPadding;
  auto drawPreviewLines = [&]() {
    int y = previewStartY;
    for (const auto& line : previewLines) {
      if (y + previewLineHeight > previewBottom) break;
      renderer.drawText(previewFontId, safeArea.x + previewPadding, y, line.c_str());
      y += previewLineHeight;
    }
  };

  if (renderer.isSdCardFont(previewFontId)) {
    // The picker must show the highlighted .cpfont itself. Prewarm the exact
    // final lines once, then blit those glyph bitmaps directly without passing
    // through the UI fallback resolver. This is isolated to the preview pane.
    std::string glyphSet;
    for (const auto& line : previewLines) {
      if (!glyphSet.empty()) glyphSet.push_back(' ');
      glyphSet += line;
    }
    const auto& sdFonts = renderer.getSdCardFonts();
    const auto sdIt = sdFonts.find(previewFontId);
    if (sdIt != sdFonts.end() && sdIt->second) {
      const int missed = sdIt->second->prewarm(glyphSet.c_str(), 0x01, false);
      LOG_INF("FONT", "Preview direct cpfont id=%d glyphs=%u missed=%d", previewFontId,
              static_cast<unsigned>(glyphSet.size()), missed);
    }

    int y = previewStartY;
    for (const auto& line : previewLines) {
      if (y + previewLineHeight > previewBottom) break;
      drawDirectSdPreviewLine(renderer, previewFontId, safeArea.x + previewPadding, y, line.c_str());
      y += previewLineHeight;
    }
  } else if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    drawPreviewLines();
    scope.endScanAndPrewarm();
    drawPreviewLines();
  } else {
    drawPreviewLines();
  }
  renderer.drawLine(safeArea.x, previewTop + previewHeight, safeArea.x + safeArea.width, previewTop + previewHeight,
                    true);

  const int contentTop = previewTop + previewHeight + metrics.verticalSpacing;
  const int contentHeight = safeArea.y + safeArea.height - contentTop - metrics.verticalSpacing;

  // Determine which font index is currently active (to mark as "Selected").
  int currentFontIndex =
      (savedSdFontFamilyName_.empty() && savedFontFamily_ == InkMODSettings::TEST_FONTS) ? 0 : -1;
  if (!savedSdFontFamilyName_.empty() && registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == savedSdFontFamilyName_) {
        currentFontIndex = i + 1;
        break;
      }
    }
  }

  GUI.drawList(
      renderer, Rect{safeArea.x, contentTop, safeArea.width, contentHeight}, static_cast<int>(fonts_.size()),
      selectedIndex_, [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
      [this, currentFontIndex](int index) -> std::string { return index == currentFontIndex ? tr(STR_SELECTED) : ""; },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
