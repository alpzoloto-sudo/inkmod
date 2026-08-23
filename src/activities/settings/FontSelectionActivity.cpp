#include "FontSelectionActivity.h"

#include <GfxRenderer.h>
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

  requestUpdate();
}

void FontSelectionActivity::onExit() {
  // previewFontId() may have swapped the real active SD reader font out for
  // a preview of a different family while browsing - SdCardFontManager only
  // keeps one family resident at a time, so restore the actual saved
  // selection now rather than leaving whatever was last previewed active.
  sdFontSystem.ensureLoaded(renderer);
  Activity::onExit();
}

int FontSelectionActivity::previewFontId() {
  if (selectedIndex_ == previewedIndex_) return previewFontId_;
  previewedIndex_ = selectedIndex_;

  if (fonts_.empty()) {
    previewFontId_ = 0;
    return previewFontId_;
  }

  const auto& font = fonts_[selectedIndex_];
  if (font.isBuiltin) {
    previewFontId_ = TEST_FONTS_12_FONT_ID;
  } else {
    // Fixed preview size rather than the user's configured reading size:
    // this screen exists to compare typefaces, not to show the exact final
    // reading layout, and a fixed size keeps line count/wrapping consistent
    // while scrolling through very differently-sized font families.
    previewFontId_ = sdFontSystem.loadPreviewFamily(font.name, renderer, /*targetPointSize=*/14);
  }
  return previewFontId_;
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
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });
}

void FontSelectionActivity::handleSelection() {
  if (fonts_.empty()) {
    finish();
    return;
  }

  const auto& font = fonts_[selectedIndex_];
  if (font.isBuiltin) {
    SETTINGS.fontFamily = InkMODSettings::TEST_FONTS;
    const uint8_t stored = InkMODSettings::getStoredReaderFontSize(InkMODSettings::SMALL);
    SETTINGS.fontSize = stored == UINT8_MAX ? 0 : stored;
    SETTINGS.sdFontFamilyName[0] = '\0';
    finish();
    return;
  }

  const uint8_t targetPointSize = currentFontPointSize(registry_);
  if (registry_) {
    const int sdIdx = font.settingIndex - InkMODSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx >= 0 && sdIdx < static_cast<int>(families.size())) {
      const std::vector<uint8_t> sizes = families[sdIdx].availableSizes();
      SETTINGS.fontSize = closestSizeIndex(sizes, targetPointSize);
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
    }
  }
  finish();
}

void FontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safeArea =
      UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);

  GUI.drawHeader(renderer, Rect{safeArea.x, metrics.topPadding, safeArea.width, metrics.headerHeight},
                 tr(STR_FONT_FAMILY));

  // Sample sentence in the current interface language - each is a standard
  // pangram-style sentence covering that language's full alphabet, so any
  // missing/unsupported glyphs in a candidate font show up immediately.
  const char* previewText = nullptr;
  switch (I18N.getLanguage()) {
    case Language::RU:
      previewText = "Съешь же ещё этих мягких французских булок, да выпей чаю.";
      break;
    case Language::UK:
      previewText = "Жебракують філософи при ґанку церкви в Гадячі, ще й шатро їхнє знаємо.";
      break;
    case Language::EN:
    default:
      previewText = "The quick brown fox jumps over the lazy dog.";
      break;
  }

  // Fixed-height preview band: always reserves the same two-line space
  // regardless of which font/text is shown, so switching the highlighted
  // font never shifts the list below it - unlike shrinking/growing the
  // preview to fit its content, which would otherwise shove every row down
  // by a different amount on every keypress.
  const int previewFontId = this->previewFontId();
  const int previewLineHeight = renderer.getLineHeight(previewFontId);
  const int previewTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int previewPadding = 6;
  const int previewHeight = previewLineHeight * 2 + previewPadding * 2;

  renderer.fillRect(safeArea.x, previewTop, safeArea.width, previewHeight, false);
  const auto previewLines =
      renderer.wrappedText(previewFontId, previewText, safeArea.width - 2 * previewPadding, 2, EpdFontFamily::REGULAR);
  int previewY = previewTop + previewPadding;
  for (const auto& line : previewLines) {
    renderer.drawText(previewFontId, safeArea.x + previewPadding, previewY, line.c_str());
    previewY += previewLineHeight;
  }
  renderer.drawLine(safeArea.x, previewTop + previewHeight, safeArea.x + safeArea.width, previewTop + previewHeight,
                    true);

  const int contentTop = previewTop + previewHeight + metrics.verticalSpacing;
  const int contentHeight = safeArea.y + safeArea.height - contentTop - metrics.verticalSpacing;

  // Determine which font index is currently active (to mark as "Selected").
  int currentFontIndex =
      (SETTINGS.sdFontFamilyName[0] == '\0' && SETTINGS.fontFamily == InkMODSettings::TEST_FONTS) ? 0 : -1;
  if (SETTINGS.sdFontFamilyName[0] != '\0' && registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == SETTINGS.sdFontFamilyName) {
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
