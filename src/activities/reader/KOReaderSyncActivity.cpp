#include "KOReaderSyncActivity.h"

#include "BootLog.h"
#include <GfxRenderer.h>
#include <Epub/Page.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdlib>

#include "InkMODSettings.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "Fb2.h"
#include "ChapterXPathResolver.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
bool syncSnippetWhitespace(const std::string& word) {
  if (word.empty()) return true;
  return std::all_of(word.begin(), word.end(), [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  });
}

std::string buildSyncTopSnippet(const Page& page) {
  std::string out;
  out.reserve(160);
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    if (!line.getBlock()) continue;
    for (const auto& word : line.getBlock()->getWords()) {
      if (syncSnippetWhitespace(word)) continue;
      if (!out.empty()) out.push_back(' ');
      if (out.size() + word.size() > 176) return out;
      out += word;
      if (out.size() >= 128) return out;
    }
  }
  return out;
}

void syncTimeWithNTP() {
  // Stop SNTP if already running (can't reconfigure while running)
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  // Configure SNTP
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  // Wait for time to sync (with timeout)
  int retry = 0;
  const int maxRetries = 50;  // 5 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry < maxRetries) {
    LOG_DBG("KOSync", "NTP time synced");
  } else {
    LOG_DBG("KOSync", "NTP sync timeout, using fallback");
  }
}

void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}
}  // namespace

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    BootLog::step("SYNC", "ensureEpubLoaded: epub is null, loading");
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.inkmod");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      BootLog::step("SYNC", "ensureEpubLoaded: epub->load() FAILED");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
    BootLog::step("SYNC", "ensureEpubLoaded: epub->load() done");
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(const InkMODPosition& position) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  const int pageCount = std::max(position.totalPages, position.pageNumber + 1);
  if (pageCount != position.totalPages) {
    LOG_DBG("KOSync", "Adjusted remote page count before save: page=%d count=%d -> %d", position.pageNumber,
            position.totalPages, pageCount);
  }
  if (!EpubReaderUtils::saveProgress(*epub, position.spineIndex, position.pageNumber, pageCount)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  BootLog::stepf("SYNC", "onWifiSelectionComplete(success=%d)", success ? 1 : 0);
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Sync time with NTP before making API requests
  BootLog::step("SYNC", "calling syncTimeWithNTP (bounded, max 5s)");
  syncTimeWithNTP();
  BootLog::step("SYNC", "syncTimeWithNTP returned");

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  BootLog::step("SYNC", "calling performSync()");
  performSync();
  BootLog::step("SYNC", "performSync() returned");
}

bool KOReaderSyncActivity::ensureDocumentHash() {
  if (!documentHash.empty()) return true;
  const std::string hashSourcePath = Fb2::resolveOriginalPath(epubPath);
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    documentHash = KOReaderDocumentId::calculateFromFilename(hashSourcePath);
  } else {
    documentHash = KOReaderDocumentId::calculate(hashSourcePath);
  }
  if (!documentHash.empty()) return true;
  {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    statusMessage = tr(STR_HASH_FAILED);
  }
  requestUpdate(true);
  return false;
}

void KOReaderSyncActivity::performSync() {
  BootLog::step("SYNC", "performSync: calculating document hash");
  if (!ensureDocumentHash()) return;
  BootLog::step("SYNC", "performSync: document hash done");

  LOG_DBG("KOSync", "Document hash: %s", documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Fetch progress screen could not be rendered synchronously; aborting sync");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  // Fetch remote progress
  BootLog::step("SYNC", "performSync: calling KOReaderSyncClient::getProgress() - network call, bounded ~15s per attempt");
  const auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  BootLog::stepf("SYNC", "performSync: getProgress() returned result=%d", static_cast<int>(result));

  if (result == KOReaderSyncClient::NOT_FOUND) {
    hasRemoteProgress = false;
    if (mode == Mode::AUTO) {
      LOG_INF("KOSync", "Quick sync: server has no progress; uploading local position");
      performUpload();
      return;
    }
    // Interactive mode: offer to upload.
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  LOG_INF("KOSync", "Remote progress: %.2f%% xpointer=%s device=%s",
          remoteProgress.percentage * 100.0f, remoteProgress.progress.c_str(), remoteProgress.device.c_str());

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  const bool fb2BackedRemote = Fb2::resolveOriginalPath(epubPath) != epubPath;
  if (fb2BackedRemote) {
    // KOReader/CREngine DocFragment[N] addresses the Nth section of the ORIGINAL
    // FB2. inkMOD may have split that section into several virtual spines, so
    // never treat N as a synthetic spine index.
    remotePosition = ProgressMapper::toInkMODFb2(epub, koPos, epub->getCachePath(), currentSpineIndex,
                                                 totalPagesInSpine);
  } else {
    remotePosition = ProgressMapper::toInkMOD(epub, koPos, currentSpineIndex, totalPagesInSpine);
  }

  // Canonical FB2 XPaths carry an exact character offset inside the source
  // paragraph. First narrow the target to the pages spanned by that paragraph,
  // then compare the *actual first rendered words* of nearby X4 pages against
  // the paragraph. This makes the selected X4 page correspond to the same
  // top-of-page text coordinate KOReader published, rather than just the same
  // chapter/paragraph.
  if (fb2BackedRemote && remotePosition.hasParagraphIndex && remotePosition.hasParagraphCharOffset) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    const uint16_t p = remotePosition.paragraphIndex;

    // The paragraph LUT stores the paragraph active at the TOP of each rendered
    // page.  The previous implementation treated getPageForParagraphIndex(p)
    // as the *end* of paragraph p, which shifts long paragraphs backwards by
    // several pages.  Find the real contiguous run of pages whose top paragraph
    // is p and compare the canonical text offset only inside that run.
    // Use the REAL page count from the section cache. remotePosition.totalPages
    // can legitimately be 1 here when the target FB2 virtual spine is not the
    // currently-open spine (the mapper only has an estimate in that case).
    // Using that estimate made every canonical point in such a chapter scan
    // only page 0, collapsing title/p[8]/p[12].455 all to "Page 1".
    const int mapperPageCount = remotePosition.totalPages;
    const int cachedPageCount = static_cast<int>(tempSection.pageCount);
    const int pageCount = cachedPageCount > 0
        ? cachedPageCount
        : std::max(mapperPageCount, remotePosition.pageNumber + 1);
    if (cachedPageCount > 0) remotePosition.totalPages = cachedPageCount;
    LOG_INF("KOSync", "FB2 exact refine page-count: mapper=%d cache=%d using=%d",
            mapperPageCount, cachedPageCount, pageCount);
    int first = -1;
    int last = -1;
    const auto hinted = tempSection.getPageForParagraphIndex(p);
    if (hinted.has_value()) {
      int probe = static_cast<int>(*hinted);
      // getPageForParagraphIndex() returns the first page whose LUT value is
      // >= p.  If p starts below the top of the previous page, include that
      // page as a candidate as well.
      if (probe > 0) {
        const auto prevP = tempSection.getParagraphIndexForPage(static_cast<uint16_t>(probe - 1));
        if (prevP.has_value() && *prevP == p) --probe;
      }
      while (probe > 0) {
        const auto prevP = tempSection.getParagraphIndexForPage(static_cast<uint16_t>(probe - 1));
        if (!prevP.has_value() || *prevP != p) break;
        --probe;
      }
      const auto probeP = tempSection.getParagraphIndexForPage(static_cast<uint16_t>(probe));
      if (probeP.has_value() && *probeP == p) {
        first = probe;
        last = probe;
        while (last + 1 < pageCount) {
          const auto nextP = tempSection.getParagraphIndexForPage(static_cast<uint16_t>(last + 1));
          if (!nextP.has_value() || *nextP != p) break;
          ++last;
        }
      } else if (probe > 0) {
        // Paragraph p can begin part-way down a page whose top still belongs to
        // p-1. Keep that page as a single fallback candidate.
        first = last = probe - 1;
      }
    }

    if (first >= 0 && last >= first) {
      int bestPage = first;
      int bestBeforeChar = -1;
      int nearestDistance = INT_MAX;
      int nearestPage = first;
      int matchedPages = 0;

      // Scan the complete paragraph run. A huge paragraph may span many X4
      // pages; limiting this to approx +/-2 was the source of the 3-4 page
      // misses seen in complex chapters.
      for (int candidate = first; candidate <= last; ++candidate) {
        tempSection.currentPage = candidate;
        auto page = tempSection.loadPageFromSectionFile();
        if (!page) continue;
        const std::string snippet = buildSyncTopSnippet(*page);
        if (snippet.empty()) continue;
        const int topChar = ChapterXPathResolver::findCharOffsetForTextSnippet(
            epub, remotePosition.spineIndex, p, snippet);
        if (topChar < 0) continue;
        ++matchedPages;
        const int distance = std::abs(topChar - static_cast<int>(remotePosition.paragraphCharOffset));
        if (distance < nearestDistance) {
          nearestDistance = distance;
          nearestPage = candidate;
        }
        // KOReader's xpointer represents the top-of-page text position. Choose
        // the latest X4 page whose first text is not after that coordinate.
        if (topChar <= static_cast<int>(remotePosition.paragraphCharOffset) && topChar > bestBeforeChar) {
          bestBeforeChar = topChar;
          bestPage = candidate;
        }
      }
      if (bestBeforeChar < 0 && nearestDistance != INT_MAX) bestPage = nearestPage;

      LOG_INF("KOSync", "FB2 exact-page refine v2: p=%u char=%u pages=%d..%d matched=%d -> %d topChar=%d nearest=%d",
              p, static_cast<unsigned>(remotePosition.paragraphCharOffset), first, last, matchedPages,
              bestPage, bestBeforeChar, nearestDistance == INT_MAX ? -1 : nearestDistance);
      if (matchedPages > 0) remotePosition.pageNumber = bestPage;
    }
  }

  // Canonical FB2 nodes without text content (notably <empty-line/>) still
  // map to a real flattened render paragraph, but they have no charOffset to
  // feed the exact text matcher above.  Resolve those directly through the
  // section paragraph LUT instead of leaving the mapper's coarse page estimate.
  if (fb2BackedRemote && remotePosition.hasParagraphIndex && !remotePosition.hasParagraphCharOffset) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    const auto page = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
    if (page.has_value()) {
      LOG_INF("KOSync", "FB2 non-text source block refine: p=%u -> page=%u/%u",
              remotePosition.paragraphIndex, static_cast<unsigned>(*page + 1),
              static_cast<unsigned>(tempSection.pageCount));
      remotePosition.pageNumber = static_cast<int>(*page);
      if (tempSection.pageCount > 0) remotePosition.totalPages = tempSection.pageCount;
    }
  }

  // Refine page using section cache LUTs: li index, anchor, or paragraph index.
  if (!fb2BackedRemote &&
      (remotePosition.hasLiIndex || remotePosition.xpathAnchorId[0] != '\0' || remotePosition.hasParagraphIndex)) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    bool refined = false;
    if (remotePosition.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(remotePosition.liIndex);
      if (liPage.has_value()) {
        LOG_DBG("KOSync", "Li index %u -> page %d (was %d)", remotePosition.liIndex, *liPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *liPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Li index %u not found in section LUT", remotePosition.liIndex);
      }
    }
    if (!refined && remotePosition.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(remotePosition.xpathAnchorId));
      if (anchorPage.has_value()) {
        LOG_DBG("KOSync", "Anchor '%s' -> page %d (was %d)", remotePosition.xpathAnchorId, *anchorPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *anchorPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Anchor '%s' not found in section cache", remotePosition.xpathAnchorId);
      }
    }
    if (!refined && remotePosition.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = std::max(remotePosition.pageNumber, static_cast<int>(*paragraphPage));
        if (nextParagraphPage.has_value()) {
          const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
          // Keep the percentage-derived page inside the paragraph's cached page range.
          // A one-page paragraph should not allow byte-percentage drift to jump to later paragraphs.
          if (lutSpan > 0 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
            refinedPage = static_cast<int>(*nextParagraphPage) - 1;
          }
        }
        char nextParaBuf[8];
        if (nextParagraphPage.has_value())
          snprintf(nextParaBuf, sizeof(nextParaBuf), "%d", *nextParagraphPage);
        else
          snprintf(nextParaBuf, sizeof(nextParaBuf), "none");
        LOG_DBG("KOSync", "Paragraph %u -> LUT page %d, nextPara page %s, intra page %d, using %d",
                remotePosition.paragraphIndex, *paragraphPage, nextParaBuf, remotePosition.pageNumber, refinedPage);
        remotePosition.pageNumber = refinedPage;
      } else {
        LOG_DBG("KOSync", "Paragraph %u not found in section LUT", remotePosition.paragraphIndex);
      }
    }
  }
  // Use ONE recommendation path for both interactive Sync Progress and Quick Sync.
  // Quick Sync must not have its own comparison rule: it simply executes the
  // exact option that the normal comparison screen would preselect.
  const auto chooseRecommendedSyncOption = [&]() -> int {
    // 0 = Apply remote progress, 1 = Upload local progress.
    return localProgress.percentage > remoteProgress.percentage ? 1 : 0;
  };

  selectedOption = chooseRecommendedSyncOption();

  if (mode == Mode::AUTO) {
    LOG_INF("KOSync",
            "Quick sync: normal Sync Progress recommends option=%d (local=%.2f%% remote=%.2f%%); executing",
            selectedOption, localProgress.percentage * 100.0f, remoteProgress.percentage * 100.0f);
    if (selectedOption == 0) {
      saveProgressAndReturn(remotePosition);
    } else {
      performUpload();
    }
    return;
  }

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  if (!ensureDocumentHash()) return;

  // Interactive sync has already reloaded Epub before the user can choose Upload.
  // Automatic sync may upload immediately after the comparison, so FB2 needs
  // the cached synthetic Epub loaded before generating its canonical source XPath.
  const bool fb2BackedBook = Fb2::resolveOriginalPath(epubPath) != epubPath;
  if (fb2BackedBook && !epub) {
    ensureEpubLoaded();
    if (!epub) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_SYNC_FAILED_MSG);
      }
      requestUpdate(true);
      return;
    }
  }

  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Upload progress screen could not be rendered synchronously; aborting upload");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  // FB2 is internally exposed as a synthetic EPUB package. Never publish that
  // synthetic XHTML XPath to KOReader: PocketBook/KOReader opens the original
  // FB2 through crengine and would treat the foreign path as invalid (often
  // jumping to the beginning). Publish a native-FB2-compatible shallow XPointer.
  const std::string originalPath = Fb2::resolveOriginalPath(epubPath);
  if (fb2BackedBook) {
    InkMODPosition fb2Pos{};
    fb2Pos.spineIndex = currentSpineIndex;
    fb2Pos.pageNumber = currentPage;
    fb2Pos.totalPages = totalPagesInSpine;
    // Re-open only the already-cached current page. The section LUT knows the
    // paragraph that is actually present at the TOP of this rendered page;
    // currentParagraphIndex may point at a later paragraph reached while the
    // page was laid out, which is especially wrong for long prose, subtitles
    // and verse blocks.
    Section localSection(epub, currentSpineIndex, renderer);
    const auto topP = localSection.getParagraphIndexForPage(static_cast<uint16_t>(currentPage));
    if (topP.has_value() && *topP > 0) {
      fb2Pos.paragraphIndex = *topP;
      fb2Pos.hasParagraphIndex = true;
      LOG_INF("KOSync", "FB2 upload top paragraph from page LUT: page=%d p=%u (readerP=%u)",
              currentPage, *topP, currentParagraphIndex.has_value() ? *currentParagraphIndex : 0);
    } else if (currentParagraphIndex.has_value()) {
      fb2Pos.paragraphIndex = *currentParagraphIndex;
      fb2Pos.hasParagraphIndex = true;
    }
    localSection.currentPage = currentPage;
    if (auto localPage = localSection.loadPageFromSectionFile()) {
      fb2Pos.topTextSnippet = buildSyncTopSnippet(*localPage);
    }
    int originalSectionOrdinal = currentSpineIndex + 1;
    if (!Fb2::getOriginalSectionOrdinal(epub->getCachePath(), currentSpineIndex, originalSectionOrdinal)) {
      LOG_DBG("KOSync", "FB2 source-section mapping unavailable for spine %d; using ordinal %d",
              currentSpineIndex, originalSectionOrdinal);
    }
    // Convert the paragraph inside inkMOD's virtual slice to a paragraph
    // ordinal inside the original FB2 source section. This makes chapter +
    // paragraph the primary coordinate; whole-book percentage is retained in
    // the sync record only for old-client compatibility/fallback.
    progress.progress = ProgressMapper::generateFb2SourceXPath(epub, epub->getCachePath(), fb2Pos,
                                                               originalSectionOrdinal);
    LOG_DBG("KOSync", "FB2 upload source XPointer: %s (spine=%d sourceSection=%d localP=%d)",
            progress.progress.c_str(), currentSpineIndex, originalSectionOrdinal,
            fb2Pos.hasParagraphIndex ? fb2Pos.paragraphIndex : 0);
  } else {
    progress.progress = localProgress.xpath;
  }
  progress.percentage = localProgress.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();
  LOG_INF("KOSync", "Upload progress: %.2f%% xpointer=%s", progress.percentage * 100.0f, progress.progress.c_str());

  const auto result = KOReaderSyncClient::updateProgress(progress);

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  wifiOff();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  if (mode == Mode::AUTO) {
    LOG_INF("KOSync", "Quick sync: upload complete, returning to reader");
    returnToReader();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  BootLog::step("SYNC", "KOReaderSyncActivity::onEnter() start");
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    BootLog::step("SYNC", "onEnter: no credentials, showing NO_CREDENTIALS");
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  sdFontSystem.releaseLoadedFont(renderer);
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    BootLog::step("SYNC", "onEnter: already WL_CONNECTED, calling onWifiSelectionComplete(true)");
    onWifiSelectionComplete(true);
    BootLog::step("SYNC", "onEnter: onWifiSelectionComplete(true) returned");
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  BootLog::step("SYNC", "onEnter: not connected, pushing WifiSelectionActivity");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
  BootLog::step("SYNC", "onEnter: startActivityForResult(WifiSelectionActivity) queued, onEnter returning");
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    wifiOff();
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_KOREADER_SYNC));

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Show comparison
    top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Remote chapter name requires Epub (loaded lazily in performSync before this state).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    // Local chapter name was pre-computed before Epub was released.
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 200, localPageStr);

    const int optionY = top + 230;
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_APPLY_REMOTE),
                      selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(screen.x, optionY + optionHeight - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY + optionHeight,
                      tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    const int messageWidth = screen.width - metrics.contentSidePadding * 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto messageLines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), messageWidth, 3);
    int messageY = top + 40;
    for (const auto& line : messageLines) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, messageY, line.c_str());
      messageY += lineHeight + 4;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition);
      } else if (selectedOption == 1) {
        // Upload local progress
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        const std::string hashSourcePath = Fb2::resolveOriginalPath(epubPath);
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(hashSourcePath);
        } else {
          documentHash = KOReaderDocumentId::calculate(hashSourcePath);
        }
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
