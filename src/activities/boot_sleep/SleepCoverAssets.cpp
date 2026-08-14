#include "SleepCoverAssets.h"

#include <Epub.h>
#include <Fb2.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstdint>

#include "InkMODSettings.h"
#include "components/UITheme.h"
#include "components/themes/minimal/MinimalTheme.h"

namespace {

constexpr int kMinimalSleepCoverHeight = MinimalMetrics::homeCoverImageHeight;
constexpr int kMinimalSleepCoverWidth = MinimalMetrics::homeCoverImageWidth;

bool shouldPrepareFullCover() {
  return SETTINGS.sleepScreen == InkMODSettings::SLEEP_SCREEN_MODE::COVER ||
         SETTINGS.sleepScreen == InkMODSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM;
}

bool shouldPrepareMinimalCover() {
  return SETTINGS.sleepScreen == InkMODSettings::SLEEP_SCREEN_MODE::MINIMAL_SLEEP ||
         SETTINGS.sleepScreen == InkMODSettings::SLEEP_SCREEN_MODE::MINIMAL_STATS_SLEEP;
}

bool fileExists(const std::string& path) { return !path.empty() && Storage.exists(path.c_str()); }

bool isDirectFb2Path(const std::string& path) {
  return FsHelpers::checkFileExtension(path, ".fb2") || FsHelpers::checkFileExtension(path, ".fb2.zip") ||
         FsHelpers::checkFileExtension(path, "_fb2.zip");
}

// Build/use the FB2 lazy package and then deliberately ask Epub to generate
// its FULL cover BMP. Epub::readItemContentsToStream() recognizes the FB2
// package marker and materializes the original embedded binary on demand;
// no home-screen thumbnail participates in this path.
bool prepareFb2FullCover(const std::string& originalPath, const bool cropped) {
  Fb2 fb2(originalPath, "/.inkmod");
  if (!fb2.load()) return false;

  Epub package(fb2.getPackagePath(), "/.inkmod");
  if (!package.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true)) return false;
  return package.generateCoverBmp(cropped);
}

std::string fb2FullCoverPath(const std::string& originalPath, const bool cropped) {
  Fb2 fb2(originalPath, "/.inkmod");
  Epub package(fb2.getPackagePath(), "/.inkmod");
  return package.getCoverBmpPath(cropped);
}

}  // namespace

namespace SleepCoverAssets {

bool prepareEpub(const Epub& epub) {
  bool success = true;
  if (shouldPrepareFullCover()) {
    const bool cropped = SETTINGS.sleepScreenCoverMode == InkMODSettings::SLEEP_SCREEN_COVER_MODE::CROP;
    success = epub.generateCoverBmp(cropped) && success;
  }
  if (shouldPrepareMinimalCover()) {
    success = epub.generateAdaptiveThumbBmp(kMinimalSleepCoverWidth, kMinimalSleepCoverHeight) && success;
  }
  return success;
}

bool prepareXtc(const Xtc& xtc) {
  bool success = true;
  if (shouldPrepareFullCover()) {
    success = xtc.generateCoverBmp() && success;
  }
  if (shouldPrepareMinimalCover()) {
    success = xtc.generateThumbBmp(static_cast<uint16_t>(kMinimalSleepCoverWidth),
                                   static_cast<uint16_t>(kMinimalSleepCoverHeight)) &&
              success;
  }
  return success;
}

bool prepareTxt(const Txt& txt) {
  if (!shouldPrepareFullCover() && !shouldPrepareMinimalCover()) {
    return true;
  }
  return txt.generateCoverBmp();
}

bool prepareFullCoverForPath(const std::string& bookPath, const bool cropped) {
  if (bookPath.empty()) {
    return false;
  }

  if (isDirectFb2Path(bookPath)) {
    return prepareFb2FullCover(bookPath, cropped);
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.inkmod");
    if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
      return false;
    }
    return epub.generateCoverBmp(cropped);
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.inkmod");
    if (!xtc.load()) {
      return false;
    }
    return xtc.generateCoverBmp();
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.inkmod");
    return txt.generateCoverBmp();
  }
  return false;
}

std::string reusableCoverPathFor(const std::string& bookPath) {
  if (isDirectFb2Path(bookPath)) {
    Fb2 fb2(bookPath, "/.inkmod");
    return Epub(fb2.getPackagePath(), "/.inkmod").getThumbBmpPath();
  }
  if (FsHelpers::hasEpubExtension(bookPath)) {
    return Epub(bookPath, "/.inkmod").getThumbBmpPath();
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    return Xtc(bookPath, "/.inkmod").getThumbBmpPath();
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    return Txt(bookPath, "/.inkmod").getCoverBmpPath();
  }
  return {};
}

std::string cachedCoverPathFor(const std::string& bookPath, const bool cropped) {
  std::string coverPath;
  if (isDirectFb2Path(bookPath)) {
    coverPath = fb2FullCoverPath(bookPath, cropped);
  } else if (FsHelpers::hasEpubExtension(bookPath)) {
    coverPath = Epub(bookPath, "/.inkmod").getCoverBmpPath(cropped);
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    coverPath = Xtc(bookPath, "/.inkmod").getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    coverPath = Txt(bookPath, "/.inkmod").getCoverBmpPath();
  }

  return fileExists(coverPath) ? coverPath : std::string{};
}

std::string cachedMinimalCoverPathFor(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    const Epub epub(bookPath, "/.inkmod");
    const std::string coverPath = epub.getAdaptiveThumbBmpPath(kMinimalSleepCoverWidth, kMinimalSleepCoverHeight);
    return fileExists(coverPath) ? epub.getThumbBmpPath() : std::string{};
  }

  const std::string reusablePath = reusableCoverPathFor(bookPath);
  const std::string coverPath =
      UITheme::getCoverThumbPath(reusablePath, kMinimalSleepCoverWidth, kMinimalSleepCoverHeight);
  return fileExists(coverPath) ? reusablePath : std::string{};
}

}  // namespace SleepCoverAssets
