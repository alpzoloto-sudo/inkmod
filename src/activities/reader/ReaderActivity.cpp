#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <cstdio>

#include "InkMODSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

static bool isImagePreviewFile(const std::string& path) {
  return FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path);
}

bool ReaderActivity::shouldShowLoadingPopup(const std::string& path) {
  if (isXtcFile(path) || isTxtFile(path) || isImagePreviewFile(path)) {
    return true;
  }

  // A browser-prepared EPUB/FB2 package already has book.bin before it has
  // ever been opened on the device. The old fast-cache shortcut therefore
  // suppressed the loading popup, even though the reader still had first-open
  // work to do (section/layout/cache validation). On e-ink that looked exactly
  // like a freeze.
  //
  // Keep the no-extra-refresh behaviour for books that have genuinely been
  // opened before, but show feedback whenever there is no saved reader
  // progress yet. Web preparation intentionally does not create progress.bin.
  Epub epub(path, "/.inkmod");
  const bool hasMetadataCache = Epub::hasCache(path, "/.inkmod");
  if (!hasMetadataCache) {
    return true;
  }

  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  const std::string progressBackupPath = progressPath + ".bak";
  const bool hasReaderProgress = Storage.exists(progressPath.c_str()) || Storage.exists(progressBackupPath.c_str());
  return !hasReaderProgress;
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path, const std::function<void(uint8_t)>& onProgress) {
  char breadcrumb[48];
  snprintf(breadcrumb, sizeof(breadcrumb), "Open EPUB %.34s", path.c_str());
  HalSystem::recordBreadcrumb(breadcrumb);
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  // Stability path for X3/X4:
  // open the original EPUB directly with the proven Epub/ERS cache pipeline.
  //
  // The optional pre-splitter (epubsplit_*) is deliberately bypassed here.
  // In release builds, some large EPUBs could reboot the ESP32-C3 during
  // EpubChapterSplitter::resolveReadPath() before the normal EPUB reader was
  // even created. Developer logs confirmed that opening the same original
  // EPUB directly proceeds through BMC/ERS and builds sections successfully.
  //
  // Keep EpubChapterSplitter sources in the project for future work/tests;
  // this change only removes it from the runtime open path.
  const std::string& readPath = path;

  auto epub = makeUniqueNoThrow<Epub>(readPath, "/.inkmod");
  if (!epub) {
    LOG_ERR("READER", "OOM: could not allocate EPUB reader");
    return nullptr;
  }
  if (epub->load(true, SETTINGS.embeddedStyle == 0, onProgress)) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  char breadcrumb[48];
  snprintf(breadcrumb, sizeof(breadcrumb), "Open XTC %.35s", path.c_str());
  HalSystem::recordBreadcrumb(breadcrumb);
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.inkmod");
  if (!xtc) {
    LOG_ERR("READER", "OOM: could not allocate XTC reader");
    return nullptr;
  }
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  char breadcrumb[48];
  snprintf(breadcrumb, sizeof(breadcrumb), "Open TXT %.35s", path.c_str());
  HalSystem::recordBreadcrumb(breadcrumb);
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, "/.inkmod");
  if (!txt) {
    LOG_ERR("READER", "OOM: could not allocate TXT reader");
    return nullptr;
  }
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc)));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt)));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (suppressInitialBackRelease) {
    mappedInput.suppressNextBackRelease();
  }

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  const bool showLoadingPopup = shouldShowLoadingPopup(initialBookPath);
  Rect loadingPopupRect{};
  std::function<void(uint8_t)> reportProgress;
  if (showLoadingPopup) {
    loadingPopupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    GUI.fillPopupProgress(renderer, loadingPopupRect, 0);
    reportProgress = [this, loadingPopupRect](const uint8_t progress) {
      GUI.fillPopupProgress(renderer, loadingPopupRect, progress);
    };
  }

  if (isImagePreviewFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
    return;
  }

  sdFontSystem.ensureLoaded(renderer);

  currentBookPath = initialBookPath;
  if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    auto epub = loadEpub(initialBookPath, reportProgress);
    if (!epub) {
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }
