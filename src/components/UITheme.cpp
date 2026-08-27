#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "fontIds.h"

namespace {
constexpr char kWidthPlaceholder[] = "[WIDTH]";
constexpr char kHeightPlaceholder[] = "[HEIGHT]";
constexpr size_t kWidthPlaceholderLength = sizeof(kWidthPlaceholder) - 1;
constexpr size_t kHeightPlaceholderLength = sizeof(kHeightPlaceholder) - 1;

std::string addBmpSuffix(const std::string& path, const char* suffix) {
  const size_t extPos = path.rfind(".bmp");
  if (extPos == std::string::npos) {
    return path + suffix;
  }
  std::string suffixedPath = path;
  suffixedPath.insert(extPos, suffix);
  return suffixedPath;
}
}  // namespace

UITheme UITheme::instance;

UITheme::UITheme() {
  const auto themeType = static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  const auto themeType = static_cast<InkMODSettings::UI_THEME>(SETTINGS.uiTheme);
  // The singleton is constructed before settings are loaded from SD, and setup()
  // calls reload() afterwards. When the persisted theme equals the already-active
  // default, recreating the polymorphic theme only adds a heap free+allocation at
  // every boot. A real theme change still goes through setTheme() below.
  if (currentTheme && currentType == themeType) return;
  setTheme(themeType);
}

void UITheme::setTheme(InkMODSettings::UI_THEME type) {
  switch (type) {
    case InkMODSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      currentType = type;
      break;
    case InkMODSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      currentType = type;
      break;
    case InkMODSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = &RoundedRaffMetrics::values;
      currentType = type;
      break;
    case InkMODSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      currentType = type;
      break;
    case InkMODSettings::UI_THEME::LYRA_CAROUSEL:
      LOG_DBG("UI", "Using Lyra Carousel theme");
      currentTheme = std::make_unique<LyraCarouselTheme>();
      currentMetrics = &LyraCarouselMetrics::values;
      currentType = type;
      break;
    case InkMODSettings::UI_THEME::MINIMAL:
      LOG_DBG("UI", "Using Minimal theme");
      currentTheme = std::make_unique<MinimalTheme>();
      currentMetrics = &MinimalMetrics::values;
      currentType = type;
      break;
    case InkMODSettings::UI_THEME::DASHBOARD:
      LOG_DBG("UI", "Using Dashboard theme");
      currentTheme = std::make_unique<DashboardTheme>();
      currentMetrics = &DashboardMetrics::values;
      currentType = type;
      break;
    default:
      LOG_ERR("UI", "Unknown theme %d, falling back to Classic", static_cast<int>(type));
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      currentType = InkMODSettings::UI_THEME::CLASSIC;
      break;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int rowHeight = metrics.listRowHeight;
  if (hasSubtitle) {
    const int subtitleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    rowHeight = std::max(metrics.listWithSubtitleRowHeight, titleLineHeight + subtitleLineHeight + 14);
  }
  return std::max(1, availableHeight / rowHeight);
}

Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  (void)hasSideButtonHints;
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += currentMetrics->buttonHintsHeight;
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += currentMetrics->buttonHintsHeight;
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(const std::string& coverBmpPath, int coverHeight) {
  if (coverHeight <= 0) {
    return "";
  }
  const int coverWidth = static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
  return getCoverThumbPath(coverBmpPath, coverWidth, coverHeight);
}

std::string UITheme::getCoverThumbPath(const std::string& coverBmpPath, int width, int height) {
  if (width <= 0 || height <= 0) {
    return "";
  }
  const size_t initialWidthPos = coverBmpPath.find(kWidthPlaceholder, 0);
  const size_t initialHeightPos = coverBmpPath.find(kHeightPlaceholder, 0);
  const bool hasWidthPlaceholder = initialWidthPos != std::string::npos;
  const bool hasHeightPlaceholder = initialHeightPos != std::string::npos;

  if (!hasWidthPlaceholder && !hasHeightPlaceholder) {
    return coverBmpPath;
  }
  if ((hasWidthPlaceholder &&
       coverBmpPath.find(kWidthPlaceholder, initialWidthPos + kWidthPlaceholderLength) != std::string::npos) ||
      (hasHeightPlaceholder &&
       coverBmpPath.find(kHeightPlaceholder, initialHeightPos + kHeightPlaceholderLength) != std::string::npos)) {
    return "";
  }
  if (!hasHeightPlaceholder) {
    return "";
  }

  std::string thumbPath = coverBmpPath;
  size_t widthPos = thumbPath.find(kWidthPlaceholder, 0);
  if (widthPos != std::string::npos) {
    thumbPath.replace(widthPos, kWidthPlaceholderLength, std::to_string(width));
  }
  size_t pos = thumbPath.find(kHeightPlaceholder, 0);
  if (pos != std::string::npos) {
    if (hasWidthPlaceholder) {
      thumbPath.replace(pos, kHeightPlaceholderLength, std::to_string(height));
    } else {
      std::string legacyPath = thumbPath;
      legacyPath.replace(pos, kHeightPlaceholderLength, std::to_string(height));
      thumbPath.replace(pos, kHeightPlaceholderLength, std::to_string(width) + "x" + std::to_string(height));
      if (!Storage.exists(thumbPath.c_str()) && Storage.exists(legacyPath.c_str())) {
        return legacyPath;
      }
    }
  }
  return thumbPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
      FsHelpers::checkFileExtension(filename, ".fb2") || FsHelpers::checkFileExtension(filename, ".fb2.zip")) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  const bool showStatusBar = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
                             SETTINGS.statusBarTitle != InkMODSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                             SETTINGS.statusBarTimeLeft != InkMODSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE ||
                             SETTINGS.statusBarBattery;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != InkMODSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != InkMODSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}
