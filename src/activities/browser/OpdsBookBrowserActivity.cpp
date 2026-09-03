#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <cstring>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "InkMODHumor.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr size_t OPDS_DOWNLOAD_BUFFER_SIZE = 1024;
constexpr size_t OPDS_FEED_IO_BUFFER_SIZE = 1024;
constexpr char OPDS_DOWNLOAD_DIR[] = "/My downloads";

// Force std::string capacities belonging to an OPDS entry back to the heap.
// Assigning OpdsEntry{} is not enough on all libstdc++ builds because string
// capacity can be retained.  On an ESP32-C3 that turns page-to-page browsing
// into permanent heap fragmentation.
void releaseOpdsEntryHeap(OpdsEntry& entry) {
  std::string().swap(entry.title);
  std::string().swap(entry.author);
  std::string().swap(entry.description);
  std::string().swap(entry.href);
  std::string().swap(entry.id);
  std::string().swap(entry.navigationHref);
  for (auto& acq : entry.acquisitions) std::string().swap(acq.href);
  entry.type = OpdsEntryType::NAVIGATION;
  entry.acquisitionCount = 0;
}

// A catalogue list never needs the annotation or acquisition URLs.  Keep only
// the fields required to paint/select the row.  The complete record is read
// lazily from SD when the user opens that one entry.
void slimOpdsEntryForList(OpdsEntry& entry) {
  std::string().swap(entry.description);
  std::string().swap(entry.id);
  std::string().swap(entry.navigationHref);
  if (entry.type == OpdsEntryType::BOOK) std::string().swap(entry.href);
  for (auto& acq : entry.acquisitions) std::string().swap(acq.href);
}
constexpr char OPDS_CACHE_DIR[] = "/.inkmod/opds";
constexpr unsigned long OPDS_EXIT_HOLD_MS = 1200;
constexpr int OPDS_DESCRIPTION_PAGE_LINES = 7;
// FORMAT_SELECTION uses a dynamic description page height.  Keep the last
// rendered values here so input navigation advances by exactly one visible
// page instead of the old hard-coded seven lines.
static int gOpdsDescriptionPageLines = OPDS_DESCRIPTION_PAGE_LINES;
static int gOpdsDescriptionMaxStart = 0;

constexpr uint32_t OPDS_WIFI_CONNECT_TIMEOUT_MS = 15000;

bool opdsWifiReady() {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

bool ensureOpdsDownloadWifi(bool forceReconnect = false) {
  if (!forceReconnect && opdsWifiReady()) return true;

  // A prepared OPDS catalog can be browsed offline, so the normal OPDS entry
  // path deliberately does not force Wi-Fi on. A book download is different:
  // bring up the last saved network here, before mbedTLS allocates anything.
  // This avoids the old sequence where the first download failed with DNS,
  // and WifiSelectionActivity connected only after the failed attempt.
  WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_ERR("OPDS", "Book download needs WiFi but there is no saved network");
    return false;
  }

  const auto* cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_ERR("OPDS", "Saved WiFi network has no credential: %s", lastSsid.c_str());
    return false;
  }

  const std::string ssid = cred->ssid;
  const std::string password = cred->password;

  if (forceReconnect) {
    LOG_INF("OPDS", "Resetting WiFi before book download retry");
    WiFi.disconnect(false);
    delay(120);
  }

  WiFi.mode(WIFI_STA);
  LOG_INF("OPDS", "Connecting WiFi for book download: ssid=%s free=%u maxAlloc=%u", ssid.c_str(),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  WiFi.begin(ssid.c_str(), password.c_str());

  const uint32_t started = millis();
  while (millis() - started < OPDS_WIFI_CONNECT_TIMEOUT_MS) {
    if (opdsWifiReady()) {
      LOG_INF("OPDS", "Book download WiFi ready: ip=%s rssi=%d free=%u maxAlloc=%u",
              WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      // Give lwIP/DNS a moment after GOT_IP, but do not start NTP or another
      // activity: both cost heap immediately before the TLS handshake.
      delay(150);
      return true;
    }
    delay(40);
  }

  LOG_ERR("OPDS", "WiFi connect timeout before book download: status=%d", static_cast<int>(WiFi.status()));
  return false;
}

std::string buildBookFilenameBase(const OpdsEntry& book, const OpdsFilenameFormat format) {
  if (book.author.empty()) return book.title;
  if (book.title.empty()) return book.author;
  if (format == OpdsFilenameFormat::TITLE_AUTHOR) return book.title + " - " + book.author;
  return book.author + " - " + book.title;
}

const char* acquisitionLabel(const OpdsBookFormat format) {
  switch (format) {
    case OpdsBookFormat::EPUB:
      return "EPUB";
    case OpdsBookFormat::FB2:
      return "FB2";
    case OpdsBookFormat::FB2_ZIP:
      return "FB2.ZIP";
  }
  return "BOOK";
}

const char* acquisitionExtension(const OpdsBookFormat format) {
  switch (format) {
    case OpdsBookFormat::EPUB:
      return ".epub";
    case OpdsBookFormat::FB2:
      return ".fb2";
    case OpdsBookFormat::FB2_ZIP:
      return ".fb2.zip";
  }
  return ".book";
}

enum class DownloadedBookKind : uint8_t { UNKNOWN, ZIP, FB2_XML };

DownloadedBookKind sniffDownloadedBook(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("OPDS", path.c_str(), file)) return DownloadedBookKind::UNKNOWN;

  uint8_t buf[512] = {};
  const int count = file.read(buf, sizeof(buf));
  file.close();
  if (count < 4) return DownloadedBookKind::UNKNOWN;

  // ZIP local header / empty archive / spanned archive signatures.
  if (buf[0] == 'P' && buf[1] == 'K' &&
      ((buf[2] == 0x03 && buf[3] == 0x04) || (buf[2] == 0x05 && buf[3] == 0x06) ||
       (buf[2] == 0x07 && buf[3] == 0x08))) {
    return DownloadedBookKind::ZIP;
  }

  // Raw FB2 may start with BOM/whitespace/XML declaration. Search the small
  // prefix for the FictionBook root instead of requiring it at byte zero.
  char ascii[513] = {};
  const int n = count < 512 ? count : 512;
  for (int i = 0; i < n; ++i) {
    unsigned char c = buf[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
    ascii[i] = static_cast<char>(c);
  }
  if (strstr(ascii, "<fictionbook") != nullptr || strstr(ascii, ":fictionbook") != nullptr) {
    return DownloadedBookKind::FB2_XML;
  }
  return DownloadedBookKind::UNKNOWN;
}


uint32_t opdsUrlHash(const std::string& value) {
  // Deterministic FNV-1a keeps cache names tiny and avoids allocating a copy
  // of long OPDS URLs. Collisions are harmless: the file is refreshed before
  // each parse and is only a transport spool/cache, never authoritative data.
  uint32_t hash = 2166136261u;
  for (const unsigned char c : value) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}

std::string opdsFeedCachePath(const std::string& url) {
  char path[48];
  snprintf(path, sizeof(path), "%s/feed_%08lx.xml", OPDS_CACHE_DIR,
           static_cast<unsigned long>(opdsUrlHash(url)));
  return path;
}

bool spoolOpdsFeedToSd(const std::string& url, const std::string& username, const std::string& password,
                       const std::string& cachePath) {
  Storage.mkdir(OPDS_CACHE_DIR, true);
  const std::string tempPath = cachePath + ".tmp";
  Storage.remove(tempPath.c_str());

  FsFile file;
  if (!Storage.openFileForWrite("OPDS", tempPath.c_str(), file)) {
    LOG_ERR("OPDS", "Could not create feed spool: %s", tempPath.c_str());
    return false;
  }

  bool writeOk = true;
  size_t bytes = 0;
  const bool fetched = HttpDownloader::fetchUrl(
      url,
      [&file, &writeOk, &bytes](const uint8_t* data, const size_t len) {
        if (!writeOk || !data || len == 0) return writeOk;
        const size_t written = file.write(data, len);
        if (written != len) {
          writeOk = false;
          return false;
        }
        bytes += written;
        return true;
      },
      username, password);
  file.flush();
  file.close();

  if (!fetched || !writeOk || bytes == 0) {
    Storage.remove(tempPath.c_str());
    LOG_ERR("OPDS", "Feed spool failed: fetched=%d write=%d bytes=%zu", fetched, writeOk, bytes);
    return false;
  }

  Storage.remove(cachePath.c_str());
  if (!Storage.rename(tempPath.c_str(), cachePath.c_str())) {
    LOG_ERR("OPDS", "Could not commit feed spool: %s", cachePath.c_str());
    Storage.remove(tempPath.c_str());
    return false;
  }

  LOG_INF("OPDS", "Feed cached on SD: %s bytes=%zu free=%u maxAlloc=%u", cachePath.c_str(), bytes,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

bool parseOpdsFeedFromSd(const std::string& cachePath, OpdsParser& parser) {
  FsFile file;
  if (!Storage.openFileForRead("OPDS", cachePath.c_str(), file)) return false;

  uint8_t buffer[OPDS_FEED_IO_BUFFER_SIZE];
  bool ok = true;
  while (file.available()) {
    const int read = file.read(buffer, sizeof(buffer));
    if (read < 0) {
      ok = false;
      break;
    }
    if (read == 0) break;
    if (parser.write(buffer, static_cast<size_t>(read)) != static_cast<size_t>(read) || parser.error()) {
      ok = false;
      break;
    }
    // If the fixed display window is full the parser intentionally stops
    // consuming XML. No need to read the rest of the cached feed from SD.
    if (parser.wasTruncated()) break;
  }
  file.close();
  parser.flush();
  return ok && !parser.error();
}

std::string replaceBookExtension(const std::string& path, const char* newExtension) {
  const size_t slash = path.find_last_of('/');
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) dot = path.size();

  // Treat .fb2.zip as one logical extension.
  if (dot < path.size() && path.compare(dot, std::string::npos, ".zip") == 0) {
    const size_t prevDot = path.find_last_of('.', dot == 0 ? 0 : dot - 1);
    if (prevDot != std::string::npos && (slash == std::string::npos || prevDot > slash) &&
        path.compare(prevDot, dot - prevDot, ".fb2") == 0) {
      dot = prevDot;
    }
  }
  return path.substr(0, dot) + newExtension;
}


bool hasEmptyQueryValue(const std::string& url, const char* key) {
  const std::string needle = std::string(key) + "=";
  const size_t pos = url.find(needle);
  if (pos == std::string::npos) return false;
  const size_t valueStart = pos + needle.size();
  return valueStart >= url.size() || url[valueStart] == '&' || url[valueStart] == '#';
}

std::string queryValue(const std::string& url, const char* key) {
  const std::string needle = std::string(key) + "=";
  const size_t pos = url.find(needle);
  if (pos == std::string::npos) return {};
  const size_t start = pos + needle.size();
  size_t end = url.find_first_of("&#", start);
  if (end == std::string::npos) end = url.size();
  return end > start ? url.substr(start, end - start) : std::string{};
}

std::string trailingNumericToken(const std::string& value) {
  if (value.empty()) return {};
  size_t end = value.size();
  while (end > 0 && (value[end - 1] < '0' || value[end - 1] > '9')) --end;
  if (end == 0) return {};
  size_t start = end;
  while (start > 0 && value[start - 1] >= '0' && value[start - 1] <= '9') --start;
  // Avoid treating tiny counters/page numbers as a book id.
  return (end - start >= 4) ? value.substr(start, end - start) : std::string{};
}

std::string repairAcquisitionHref(const OpdsEntry& book, const std::string& href) {
  if (!hasEmptyQueryValue(href, "hub_id")) return href;

  std::string hubId = queryValue(book.navigationHref, "hub_id");
  if (hubId.empty()) hubId = queryValue(book.navigationHref, "id");
  if (hubId.empty()) hubId = queryValue(book.navigationHref, "book_id");
  if (hubId.empty()) hubId = queryValue(book.id, "hub_id");
  if (hubId.empty()) hubId = queryValue(book.id, "id");
  if (hubId.empty()) hubId = trailingNumericToken(book.id);
  if (hubId.empty()) hubId = trailingNumericToken(book.navigationHref);
  if (hubId.empty()) return {};

  std::string repaired = href;
  const std::string needle = "hub_id=";
  const size_t pos = repaired.find(needle);
  if (pos == std::string::npos) return {};
  repaired.insert(pos + needle.size(), hubId);
  return repaired;
}

bool looksLikeFlibusta(const std::string& url) {
  return url.find("flibusta") != std::string::npos;
}

std::string resolveOpdsUrl(const std::string& baseUrl, const std::string& ref) {
  if (ref.empty()) return baseUrl;
  if (ref.find("://") != std::string::npos) return ref;

  const std::string absoluteBase = UrlUtils::ensureProtocol(baseUrl);
  const size_t schemePos = absoluteBase.find("://");
  const std::string scheme = schemePos == std::string::npos ? "http:" : absoluteBase.substr(0, schemePos + 1);

  // RFC-style network-path reference, e.g. //cdn.example.org/book.epub
  if (ref.rfind("//", 0) == 0) return scheme + ref;

  const std::string host = UrlUtils::extractHost(absoluteBase);
  if (!ref.empty() && ref[0] == '/') return host + ref;

  std::string base = absoluteBase;
  const size_t fragmentPos = base.find('#');
  if (fragmentPos != std::string::npos) base.resize(fragmentPos);
  const size_t queryPos = base.find('?');
  if (queryPos != std::string::npos) base.resize(queryPos);

  // Query-only references keep the current path.
  if (!ref.empty() && ref[0] == '?') return base + ref;

  const size_t hostEnd = host.size();
  std::string path = base.size() > hostEnd ? base.substr(hostEnd) : std::string("/");
  if (path.empty()) path = "/";
  const size_t lastSlash = path.rfind('/');
  std::string merged = (lastSlash == std::string::npos ? std::string("/") : path.substr(0, lastSlash + 1)) + ref;

  // Collapse ./ and ../ path segments without touching the authority.
  std::vector<std::string> segments;
  size_t pos = 0;
  std::string suffix;
  const size_t suffixPos = merged.find_first_of("?#");
  if (suffixPos != std::string::npos) {
    suffix = merged.substr(suffixPos);
    merged.resize(suffixPos);
  }
  while (pos <= merged.size()) {
    const size_t slash = merged.find('/', pos);
    const size_t end = slash == std::string::npos ? merged.size() : slash;
    const std::string segment = merged.substr(pos, end - pos);
    if (segment.empty() || segment == ".") {
      // skip
    } else if (segment == "..") {
      if (!segments.empty()) segments.pop_back();
    } else {
      segments.push_back(segment);
    }
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }

  std::string normalized = "/";
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i) normalized += '/';
    normalized += segments[i];
  }
  return host + normalized + suffix;
}
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  sdFontSystem.releaseLoadedFont(renderer);

  state = BrowserState::CHECK_WIFI;
  entryCount = 0;
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  longBackTriggered = false;
  formatAcquisitionCount = 0;
  formatSelectionIndex = 0;
  descriptionScroll = 0;
  infoDescriptionScroll = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  if (!ensureEntryBuffer()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  clearEntries();
  navigationHistory.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsBookBrowserActivity::loop() {
  // Long Back is a global OPDS escape hatch. It deliberately works from
  // catalog lists, format/details screens and errors, so deeply nested feeds
  // never require dozens of Back presses to leave OPDS.
  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    if (!longBackTriggered && mappedInput.getHeldTime() >= OPDS_EXIT_HOLD_MS) {
      longBackTriggered = true;
      mappedInput.suppressNextBackRelease();
      navigationHistory.clear();
      clearEntries();
      onGoHome();
      return;
    }
  } else {
    longBackTriggered = false;
  }

  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) return;

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::FORMAT_SELECTION) {
    if (!selectedEntryValid || formatAcquisitionCount == 0) {
      state = BrowserState::BROWSING;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const size_t acquisitionIndex = formatAcquisitionIndexes[formatSelectionIndex];
      downloadBook(selectedEntry, acquisitionIndex);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = BrowserState::BROWSING;
      descriptionScroll = 0;
      requestUpdate();
      return;
    }
    // Front left/right choose the actual file extension.
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      formatSelectionIndex = static_cast<uint8_t>(ButtonNavigator::previousIndex(formatSelectionIndex,
                                                                                 formatAcquisitionCount));
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      formatSelectionIndex = static_cast<uint8_t>(ButtonNavigator::nextIndex(formatSelectionIndex,
                                                                             formatAcquisitionCount));
      requestUpdate();
    }
    // Side buttons scroll the description without changing the selected format.
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      const int pageLines = std::max(1, gOpdsDescriptionPageLines);
      // The final page may start at a non-page-aligned offset (maxStart).
      // In that case go back to the previous true page boundary.
      if (descriptionScroll == gOpdsDescriptionMaxStart &&
          descriptionScroll > 0 && (descriptionScroll % pageLines) != 0) {
        descriptionScroll = ((descriptionScroll - 1) / pageLines) * pageLines;
      } else {
        descriptionScroll = std::max(0, descriptionScroll - pageLines);
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      const int pageLines = std::max(1, gOpdsDescriptionPageLines);
      descriptionScroll = std::min(gOpdsDescriptionMaxStart, descriptionScroll + pageLines);
      requestUpdate();
    }
    return;
  }

  if (state == BrowserState::INFO_VIEW) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = BrowserState::BROWSING;
      infoDescriptionScroll = 0;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      infoDescriptionScroll = std::max(0, infoDescriptionScroll - OPDS_DESCRIPTION_PAGE_LINES);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      infoDescriptionScroll += OPDS_DESCRIPTION_PAGE_LINES;
      requestUpdate();
    }
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        showLoadingBeforeFetch();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (entryCount > 0) {
        const OpdsEntry* entryPtr = visibleEntry(static_cast<size_t>(selectorIndex));
        if (!entryPtr) return;
        const auto& entry = *entryPtr;
        if (entry.type == OpdsEntryType::BOOK) {
          selectBookFormat(static_cast<size_t>(selectorIndex));
        } else if (entry.type == OpdsEntryType::SEARCH) {
          launchSearch();
        } else if (entry.type == OpdsEntryType::REFRESH) {
          showLoadingBeforeFetch();
          fetchFeed(currentPath, true);
        } else if (entry.type == OpdsEntryType::INFO) {
          showInfoEntry(static_cast<size_t>(selectorIndex));
        } else {
          navigateToEntry(entry);
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }

    if (entryCount > 0) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entryCount);
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entryCount);
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entryCount);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entryCount);
        requestUpdate();
      });
    }
  }
}

bool OpdsBookBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
    case BrowserState::FORMAT_SELECTION:
    case BrowserState::INFO_VIEW:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Show server name in header if available, otherwise generic title
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerTitle, true, EpdFontFamily::BOLD);

  if (state == BrowserState::FORMAT_SELECTION && selectedEntryValid && formatAcquisitionCount > 0) {
    const auto& book = selectedEntry;
    const auto& metrics = UITheme::getInstance().getMetrics();

    // Fully adaptive portrait card.  Previous versions used Y coordinates that
    // were effectively tuned for a ~480 px-high canvas, while X4 portrait is
    // ~800 px high.  That is why the text/format block sat in the upper half and
    // left a huge blank area above the hardware-button hints.
    //
    // Anchor the footer to the REAL screen height, reserve the button-hint
    // strip, and let title/author/annotation consume the whole remaining area.
    const int side = std::max(18, static_cast<int>(metrics.contentSidePadding));
    const int textWidth = pageWidth - side * 2;
    const int hintsTop = pageHeight - metrics.buttonHintsHeight;
    const int helpLine2Y = hintsTop - 26;
    const int helpLine1Y = helpLine2Y - 24;
    const int formatValueY = helpLine1Y - 40;
    const int formatLabelY = formatValueY - 28;
    const int footerTop = formatLabelY - 16;
    const int contentTop = 46;
    const int contentBottom = std::max(contentTop + 80, footerTop);
    const int contentHeight = std::max(80, contentBottom - contentTop);

    const int titleFont = UI_10_FONT_ID;
    const int titleLineH = std::max(22, renderer.getLineHeight(titleFont) + 2);
    const int authorFont = SMALL_FONT_ID;
    const int authorLineH = std::max(20, renderer.getLineHeight(authorFont) + 2);

    auto titleLines = renderer.wrappedText(titleFont, book.title.c_str(), textWidth, 4, EpdFontFamily::BOLD);
    if (titleLines.empty()) titleLines.push_back(book.title);

    std::vector<std::string> authorLines;
    if (!book.author.empty()) {
      authorLines = renderer.wrappedText(authorFont, book.author.c_str(), textWidth, 2);
      if (authorLines.empty()) authorLines.push_back(book.author);
    }

    const char* desc = book.description.empty() ? "Описание в каталоге отсутствует" : book.description.c_str();

    // Start with a comfortable body font.  Only shrink when a genuinely long
    // annotation would otherwise waste most of the page on pagination.
    int descFont = UI_12_FONT_ID;
    int descLineH = std::max(25, renderer.getLineHeight(descFont) + 3);
    auto descLines = renderer.wrappedText(descFont, desc, textWidth, 160);
    if (descLines.empty()) descLines.push_back(desc);

    const int titleH = static_cast<int>(titleLines.size()) * titleLineH;
    const int authorH = static_cast<int>(authorLines.size()) * authorLineH;
    const int fixedMetaH = titleH + authorH + (authorLines.empty() ? 8 : 12);
    int availableDescH = std::max(descLineH, contentHeight - fixedMetaH);
    int maxDescLines = std::max(1, availableDescH / descLineH);

    if (static_cast<int>(descLines.size()) > maxDescLines + 3) {
      descFont = UI_10_FONT_ID;
      descLineH = std::max(22, renderer.getLineHeight(descFont) + 2);
      descLines = renderer.wrappedText(descFont, desc, textWidth, 160);
      if (descLines.empty()) descLines.push_back(desc);
      availableDescH = std::max(descLineH, contentHeight - fixedMetaH);
      maxDescLines = std::max(1, availableDescH / descLineH);
    }

    const bool paged = static_cast<int>(descLines.size()) > maxDescLines;
    const int maxStart = std::max(0, static_cast<int>(descLines.size()) - maxDescLines);
    gOpdsDescriptionPageLines = std::max(1, maxDescLines);
    gOpdsDescriptionMaxStart = maxStart;
    descriptionScroll = std::max(0, std::min(descriptionScroll, maxStart));
    const int visibleDesc = std::min(maxDescLines, static_cast<int>(descLines.size()) - descriptionScroll);
    const int descH = std::max(0, visibleDesc) * descLineH;

    // Centre the COMPLETE metadata block when it is short.  For long text the
    // block naturally fills from contentTop to contentBottom.  This is the key
    // difference from the old code which centred only the description inside a
    // tiny hard-coded 43..292 rectangle.
    const int blockH = titleH + authorH + (authorLines.empty() ? 8 : 12) + descH;
    int y = contentTop;
    if (!paged && blockH < contentHeight) y += (contentHeight - blockH) / 2;

    for (const auto& line : titleLines) {
      renderer.drawCenteredText(titleFont, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += titleLineH;
    }
    for (const auto& line : authorLines) {
      renderer.drawCenteredText(authorFont, y, line.c_str());
      y += authorLineH;
    }
    y += authorLines.empty() ? 8 : 12;

    for (int i = descriptionScroll;
         i < static_cast<int>(descLines.size()) && i < descriptionScroll + maxDescLines; ++i) {
      renderer.drawText(descFont, side, y, descLines[i].c_str());
      y += descLineH;
    }

    if (paged) {
      char page[24];
      const int pageCount = (static_cast<int>(descLines.size()) + maxDescLines - 1) / maxDescLines;
      const int pageNo = (descriptionScroll >= maxStart && maxStart > 0)
                             ? pageCount
                             : std::min(pageCount, descriptionScroll / maxDescLines + 1);
      snprintf(page, sizeof(page), "%d/%d", pageNo, pageCount);
      renderer.drawCenteredText(SMALL_FONT_ID, contentBottom + 1, page);
    }

    renderer.drawCenteredText(SMALL_FONT_ID, formatLabelY, "Формат файла");
    const uint8_t acq = formatAcquisitionIndexes[formatSelectionIndex];
    char selected[48];
    snprintf(selected, sizeof(selected), "<  %s  >", acquisitionLabel(book.acquisitions[acq].format));
    renderer.drawCenteredText(UI_12_FONT_ID, formatValueY, selected, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, helpLine1Y, "Боковые: описание   Передние: формат");
    renderer.drawCenteredText(SMALL_FONT_ID, helpLine2Y, "Зажать Назад: выйти из OPDS");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::INFO_VIEW && selectedEntryValid) {
    const auto& info = selectedEntry;
    renderer.drawCenteredText(UI_10_FONT_ID, 50, info.title.c_str(), true, EpdFontFamily::BOLD);
    if (!info.author.empty()) renderer.drawCenteredText(SMALL_FONT_ID, 76, info.author.c_str());
    const char* desc = info.description.empty() ? "В этом разделе нет дополнительной информации" : info.description.c_str();
    const auto lines = renderer.wrappedText(SMALL_FONT_ID, desc, pageWidth - 44, 48);
    const int maxStart = std::max(0, static_cast<int>(lines.size()) - 12);
    infoDescriptionScroll = std::min(infoDescriptionScroll, maxStart);
    int y = 108;
    for (int i = infoDescriptionScroll; i < static_cast<int>(lines.size()) && i < infoDescriptionScroll + 12; ++i) {
      renderer.drawText(SMALL_FONT_ID, 22, y, lines[i].c_str());
      y += 22;
    }
    renderer.drawCenteredText(SMALL_FONT_ID, 398, "Боковые: листать   Зажать Назад: выйти из OPDS");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 62, tr(STR_DOWNLOADING));

    // Show coarse live progress so it is obvious that the transfer is alive.
    // Updates are deliberately throttled in the callback to avoid spending
    // half a second refreshing e-paper for every network chunk.
    const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 56, 2);
    int titleY = pageHeight / 2 - 34;
    for (const auto& line : titleLines) {
      renderer.drawCenteredText(UI_10_FONT_ID, titleY, line.c_str());
      titleY += 24;
    }

    if (downloadTotal > 0) {
      const Rect barRect{50, pageHeight / 2 + 20, pageWidth - 100, 20};
      GUI.drawProgressBar(renderer, barRect, downloadProgress, downloadTotal);
      // BaseTheme::drawProgressBar already paints the percentage at
      // bar.y + bar.height + 15.  The old OPDS code painted a second line at
      // almost the same Y, producing the visible "32%" collision.
      char progressText[56];
      snprintf(progressText, sizeof(progressText), "Скачано: %zu KB / %zu KB", downloadProgress / 1024,
               downloadTotal / 1024);
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 82, progressText);
    } else {
      char progressText[48];
      snprintf(progressText, sizeof(progressText), "Скачано: %zu KB", downloadProgress / 1024);
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 42, progressText);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const char* confirmLabel = tr(STR_OPEN);
  if (entryCount > 0) {
    const OpdsEntry* selected = visibleEntry(static_cast<size_t>(selectorIndex));
    if (selected && selected->type == OpdsEntryType::BOOK) confirmLabel = tr(STR_DOWNLOAD);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entryCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 24, tr(STR_NO_ENTRIES));
    InkMODHumor::drawBounded(renderer, InkMODHumor::Moment::EmptyCatalog, 0, pageHeight / 2 + 12, pageWidth,
                             std::max(0, pageHeight / 2 - 88), SMALL_FONT_ID, true);
  } else {
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

    // OPDS lists are redrawn often while scrolling. Building two std::string
    // temporaries for every visible row was enough to fragment the C3 heap
    // after a large Flibusta feed. Keep row formatting entirely on the stack;
    // the framebuffer clips text at the right edge anyway.
    for (size_t i = pageStartIndex; i < entryCount && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
      const OpdsEntry* entryPtr = visibleEntry(i);
      if (!entryPtr) continue;
      const auto& entry = *entryPtr;
      char displayText[448];
      if (entry.type == OpdsEntryType::SEARCH) {
        snprintf(displayText, sizeof(displayText), "? %s", entry.title.c_str());
      } else if (entry.type == OpdsEntryType::REFRESH) {
        snprintf(displayText, sizeof(displayText), "* %s", entry.title.c_str());
      } else if (entry.type == OpdsEntryType::NAVIGATION) {
        snprintf(displayText, sizeof(displayText), "> %s", entry.title.c_str());
      } else if (entry.type == OpdsEntryType::INFO) {
        snprintf(displayText, sizeof(displayText), "i %s", entry.title.c_str());
      } else if (entry.type == OpdsEntryType::BOOK) {
        char formatText[40] = {0};
        size_t used = 0;
        for (uint8_t f = 0; f < entry.acquisitionCount; ++f) {
          const char* label = acquisitionLabel(entry.acquisitions[f].format);
          const int written = snprintf(formatText + used, sizeof(formatText) - used, "%s%s", f ? "/" : "", label);
          if (written <= 0 || static_cast<size_t>(written) >= sizeof(formatText) - used) break;
          used += static_cast<size_t>(written);
        }
        if (!entry.author.empty()) {
          snprintf(displayText, sizeof(displayText), "v %s - %s [%s]", entry.title.c_str(), entry.author.c_str(),
                   formatText);
        } else {
          snprintf(displayText, sizeof(displayText), "v %s [%s]", entry.title.c_str(), formatText);
        }
      } else if (!entry.author.empty()) {
        snprintf(displayText, sizeof(displayText), "%s - %s", entry.title.c_str(), entry.author.c_str());
      } else {
        snprintf(displayText, sizeof(displayText), "%s", entry.title.c_str());
      }
      renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, displayText,
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::showLoadingBeforeFetch() {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("OPDS", "Loading screen could not be rendered before feed fetch");
    requestUpdate(true);
  }
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path, const bool forceRefresh) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  clearEntries();
  const std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  const uint32_t urlHash = opdsUrlHash(url);
  char baseBuf[64];
  snprintf(baseBuf, sizeof(baseBuf), "%s/cache_%08lx", OPDS_CACHE_DIR, static_cast<unsigned long>(urlHash));
  const std::string cacheBase = baseBuf;
  const std::string xmlCachePath = opdsFeedCachePath(url);
  Storage.mkdir(OPDS_CACHE_DIR, true);

  LOG_DBG("OPDS", "Opening catalog: %s", url.c_str());
  LOG_INF("OPDS", "Feed start heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Cache-first: once a feed has been prepared, browsing it no longer needs
  // the network or XML parser. A future explicit refresh can rebuild the same
  // cache atomically without changing the UI/read path.
  if (!forceRefresh && cacheStore.openRead(cacheBase) && cacheStore.meta().urlHash == urlHash && cacheStore.meta().url == url) {
    LOG_INF("OPDS", "Using prepared SD catalog: entries=%zu", cacheStore.meta().entryCount);
    applyCacheMeta();
    selectorIndex = 0;
    state = entryCount == 0 ? BrowserState::ERROR : BrowserState::BROWSING;
    if (entryCount == 0) errorMessage = tr(STR_NO_ENTRIES);
    requestUpdate();
    return;
  }
  cacheStore.closeRead();

  // A cache miss really needs the network.  Cache-first browsing is allowed
  // with Wi-Fi off, but previously an uncached child feed tried DNS first and
  // immediately threw the user onto the error screen; auto-connect happened
  // only afterwards.  Connect here before touching TLS.
  if (WiFi.status() != WL_CONNECTED && !ensureOpdsDownloadWifi(false)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }

  // TLS and Expat never overlap. First spool the raw Atom XML to SD, then
  // destroy the network connection and convert XML -> compact binary records.
  if (!spoolOpdsFeedToSd(url, server.username, server.password, xmlCachePath)) {
    if (!Storage.exists(xmlCachePath.c_str())) {
      if (cacheStore.openRead(cacheBase) && cacheStore.meta().urlHash == urlHash && cacheStore.meta().url == url) {
        LOG_INF("OPDS", "Refresh failed; keeping prepared catalog: entries=%zu", cacheStore.meta().entryCount);
        statusMessage = "Каталог не обновлён — открыт кэш";
        applyCacheMeta();
        selectorIndex = 0;
        state = entryCount == 0 ? BrowserState::ERROR : BrowserState::BROWSING;
        requestUpdate();
        return;
      }
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
    LOG_INF("OPDS", "Using previous raw SD feed after network failure: %s", xmlCachePath.c_str());
  }

  if (!cacheStore.beginWrite(cacheBase, url, urlHash)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

  LOG_INF("OPDS", "Building binary cache from SD XML: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  OpdsParser parser(&OpdsCacheStore::parserSink, &cacheStore);
  if (!parseOpdsFeedFromSd(xmlCachePath, parser) || !parser) {
    cacheStore.abortWrite();
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    LOG_ERR("OPDS", "SD feed -> cache failed: reason=%d free=%u maxAlloc=%u",
            static_cast<int>(parser.getErrorReason()), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    requestUpdate();
    return;
  }

  if (!cacheStore.finishWrite(parser.getSearchTemplate(), parser.getNextPageUrl(), parser.getPrevPageUrl())) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

  // Raw XML was only a transport spool. The compact cache is authoritative for
  // browsing and survives re-entry; dropping XML saves SD space and prevents a
  // later code path from accidentally reparsing it.
  Storage.remove(xmlCachePath.c_str());

  if (!cacheStore.openRead(cacheBase)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  LOG_INF("OPDS", "Binary catalog ready: entries=%zu free=%u maxAlloc=%u", cacheStore.meta().entryCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  applyCacheMeta();
  selectorIndex = 0;
  state = entryCount == 0 ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entryCount == 0) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

bool OpdsBookBrowserActivity::ensureEntryBuffer() { return true; }

void OpdsBookBrowserActivity::clearEntries() {
  cacheStore.closeRead();
  for (auto& entry : pageEntries) releaseOpdsEntryHeap(entry);
  pageStart = static_cast<size_t>(-1);
  pageEntryCount = 0;
  cachedEntryCount = 0;
  entryCount = 0;
  hasSearchRow = hasPrevRow = hasNextRow = hasRefreshRow = false;
  std::string().swap(prevPageUrl);
  std::string().swap(nextPageUrl);
  releaseOpdsEntryHeap(selectedEntry);
  selectedEntryValid = false;
  formatAcquisitionCount = 0;
  formatSelectionIndex = 0;
  descriptionScroll = 0;
}

bool OpdsBookBrowserActivity::appendEntry(OpdsEntry&&) { return false; }

void OpdsBookBrowserActivity::applyCacheMeta() {
  const auto& meta = cacheStore.meta();
  cachedEntryCount = meta.entryCount;
  if (!meta.searchTemplate.empty()) searchTemplate = meta.searchTemplate;
  if (searchTemplate.empty() && looksLikeFlibusta(server.url)) {
    searchTemplate = UrlUtils::buildUrl(server.url,
                                        "/opds/search?searchType=books&searchTerm={searchTerms}&pageNumber=0");
  }
  prevPageUrl = meta.prevPageUrl;
  nextPageUrl = meta.nextPageUrl;
  const bool isRootFeed = navigationHistory.empty() && currentPath.empty();
  hasSearchRow = isRootFeed && !searchTemplate.empty();
  hasPrevRow = !prevPageUrl.empty();
  hasNextRow = !nextPageUrl.empty();
  hasRefreshRow = isRootFeed;
  entryCount = cachedEntryCount + (hasSearchRow ? 1 : 0) + (hasPrevRow ? 1 : 0) + (hasNextRow ? 1 : 0) + (hasRefreshRow ? 1 : 0);
  pageStart = static_cast<size_t>(-1);
  pageEntryCount = 0;
}

bool OpdsBookBrowserActivity::loadEntry(const size_t index, OpdsEntry& out) {
  if (index >= entryCount) return false;
  size_t cursor = 0;
  if (hasPrevRow) {
    if (index == cursor) {
      out = OpdsEntry{};
      out.type = OpdsEntryType::NAVIGATION;
      out.title = tr(STR_PREV_PAGE);
      out.href = prevPageUrl;
      return true;
    }
    ++cursor;
  }
  if (hasSearchRow) {
    if (index == cursor) {
      out = OpdsEntry{};
      out.type = OpdsEntryType::SEARCH;
      out.title = tr(STR_SEARCH);
      return true;
    }
    ++cursor;
  }
  if (hasRefreshRow) {
    if (index == cursor) {
      out = OpdsEntry{};
      out.type = OpdsEntryType::REFRESH;
      out.title = "Обновить каталог";
      return true;
    }
    ++cursor;
  }
  if (index < cursor + cachedEntryCount) return cacheStore.readEntry(index - cursor, out);
  cursor += cachedEntryCount;
  if (hasNextRow && index == cursor) {
    out = OpdsEntry{};
    out.type = OpdsEntryType::NAVIGATION;
    out.title = tr(STR_NEXT_PAGE);
    out.href = nextPageUrl;
    return true;
  }
  return false;
}

bool OpdsBookBrowserActivity::loadPageFor(const size_t index) {
  if (index >= entryCount) return false;
  const size_t wantedStart = (index / PAGE_ITEMS) * PAGE_ITEMS;
  if (pageStart == wantedStart && index < pageStart + pageEntryCount) return true;
  for (auto& entry : pageEntries) releaseOpdsEntryHeap(entry);
  pageStart = wantedStart;
  pageEntryCount = 0;
  const size_t count = std::min<size_t>(PAGE_ITEMS, entryCount - wantedStart);
  for (size_t i = 0; i < count; ++i) {
    if (!loadEntry(wantedStart + i, pageEntries[i])) {
      for (auto& entry : pageEntries) releaseOpdsEntryHeap(entry);
      pageStart = static_cast<size_t>(-1);
      pageEntryCount = 0;
      return false;
    }
    slimOpdsEntryForList(pageEntries[i]);
    ++pageEntryCount;
  }
  return true;
}

const OpdsEntry* OpdsBookBrowserActivity::visibleEntry(const size_t index) {
  if (!loadPageFor(index) || index < pageStart || index >= pageStart + pageEntryCount) return nullptr;
  return &pageEntries[index - pageStart];
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  clearEntries();
  selectorIndex = 0;
  showLoadingBeforeFetch();
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    clearEntries();
    selectorIndex = 0;
    showLoadingBeforeFetch();
    fetchFeed(currentPath);
  }
}

void OpdsBookBrowserActivity::selectBookFormat(const size_t entryIndex) {
  if (entryIndex >= entryCount || !loadEntry(entryIndex, selectedEntry)) return;
  selectedEntryValid = true;
  const auto& book = selectedEntry;
  if (book.type != OpdsEntryType::BOOK || book.acquisitionCount == 0) return;

  formatAcquisitionCount = 0;
  for (uint8_t i = 0; i < book.acquisitionCount && formatAcquisitionCount < MAX_OPDS_ACQUISITIONS; ++i) {
    const std::string usableHref = repairAcquisitionHref(book, book.acquisitions[i].href);
    if (usableHref.empty()) continue;
    formatAcquisitionIndexes[formatAcquisitionCount++] = i;
  }

  if (formatAcquisitionCount == 0) {
    if (!book.navigationHref.empty()) {
      OpdsEntry navigationEntry;
      navigationEntry.href = book.navigationHref;
      navigateToEntry(navigationEntry);
      return;
    }
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  formatEntryIndex = entryIndex;
  formatSelectionIndex = 0;
  descriptionScroll = 0;
  state = BrowserState::FORMAT_SELECTION;
  requestUpdate(true);
}

void OpdsBookBrowserActivity::showInfoEntry(const size_t entryIndex) {
  if (entryIndex >= entryCount || !loadEntry(entryIndex, selectedEntry)) return;
  selectedEntryValid = true;
  infoEntryIndex = entryIndex;
  infoDescriptionScroll = 0;
  state = BrowserState::INFO_VIEW;
  requestUpdate(true);
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book, const size_t acquisitionIndex) {
  // Copy everything needed before clearing the feed. Keeping 20-24 title,
  // author and href strings alive while mbedTLS starts another HTTPS transfer
  // leaves too little contiguous heap on the C3 and can trigger
  // HTTP_CLIENT: Failed to allocate memory for storing decoded data.
  if (acquisitionIndex >= book.acquisitionCount) return;
  auto acquisition = book.acquisitions[acquisitionIndex];

  const std::string repairedHref = repairAcquisitionHref(book, acquisition.href);
  if (repairedHref.empty()) {
    LOG_ERR("OPDS", "Unusable acquisition URL (missing hub_id and no repair source): %s", acquisition.href.c_str());
    if (!book.navigationHref.empty()) {
      OpdsEntry navigationEntry;
      navigationEntry.href = book.navigationHref;
      navigateToEntry(navigationEntry);
      return;
    }
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }
  if (repairedHref != acquisition.href) {
    LOG_INF("OPDS", "Repaired acquisition hub_id from entry metadata");
    acquisition.href = repairedHref;
  }

  statusMessage = book.title;
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  const std::string downloadUrl = resolveOpdsUrl(feedUrl, acquisition.href);
  const std::string legacyDownloadUrl = UrlUtils::buildUrl(feedUrl, acquisition.href);
  LOG_INF("OPDS", "Download resolve: format=%s href=%s rfc=%s legacy=%s", acquisitionLabel(acquisition.format),
          acquisition.href.c_str(), downloadUrl.c_str(), legacyDownloadUrl.c_str());
  Storage.mkdir(OPDS_DOWNLOAD_DIR, true);
  std::string filename = std::string(OPDS_DOWNLOAD_DIR) + "/" +
                         StringUtils::sanitizeFilename(buildBookFilenameBase(book, server.filenameFormat)) +
                         acquisitionExtension(acquisition.format);

  state = BrowserState::DOWNLOADING;
  downloadProgress = downloadTotal = 0;
  lastDownloadUiBytes = 0;
  lastDownloadUiMs = millis();
  clearEntries();
  LOG_INF("OPDS", "Book page released before TLS: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  requestUpdate(true);

  // Catalogs are intentionally cache-first/offline, but downloading a book
  // obviously requires a live network. Connect here while the OPDS page/cache
  // has already been released, leaving the largest possible contiguous heap
  // for the TLS handshake.
  if (!ensureOpdsDownloadWifi(false)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate(true);
    return;
  }

  LOG_INF("OPDS", "Book download start heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  bool cancelRequested = false;
  auto pollCancel = [this, &cancelRequested] {
    if (cancelRequested) {
      return true;
    }
    mappedInput.update();
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelRequested = true;
    }
    return cancelRequested;
  };
  auto progressCallback = [this](const size_t downloaded, const size_t total) {
    downloadProgress = downloaded;
    downloadTotal = total;
    const uint32_t now = millis();
    const bool firstVisible = lastDownloadUiBytes == 0 && downloaded > 0;
    const bool enoughBytes = downloaded >= lastDownloadUiBytes + 512 * 1024;
    const bool enoughTime = now - lastDownloadUiMs >= 5000;
    const bool completed = total > 0 && downloaded >= total;
    if (firstVisible || enoughBytes || enoughTime || completed) {
      lastDownloadUiBytes = downloaded;
      lastDownloadUiMs = now;
      requestUpdate(true);
    }
  };

  // Keep a partial OPDS file on transient socket errors. Some catalog servers
  // use chunked downloads and occasionally close a long transfer mid-body.
  // Retrying the same canonical URL with Range is much safer than throwing
  // away hundreds of kilobytes and trying a made-up legacy URL.
  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.shouldCancel = pollCancel;
  downloadOptions.bufferSize = OPDS_DOWNLOAD_BUFFER_SIZE;
  downloadOptions.preservePartial = true;

  auto result = HttpDownloader::downloadToFile(downloadUrl, filename, progressCallback, &cancelRequested,
                                                server.username, server.password, downloadOptions);

  // If TLS/DNS failed before the first byte, there is nothing useful to
  // resume. Reset the Wi-Fi socket state and retry exactly once. This covers
  // stale DNS/lwIP state after long offline cache browsing without turning a
  // bad server into an endless retry loop.
  if (result == HttpDownloader::HTTP_ERROR && !cancelRequested) {
    FsFile zeroCheck;
    size_t bytesOnDisk = 0;
    if (Storage.openFileForRead("OPDS", filename.c_str(), zeroCheck)) {
      bytesOnDisk = zeroCheck.fileSize();
      zeroCheck.close();
    }
    if (bytesOnDisk == 0) {
      LOG_INF("OPDS", "Zero-byte network failure; reconnecting WiFi and retrying once");
      Storage.remove(filename.c_str());
      if (ensureOpdsDownloadWifi(true)) {
        downloadProgress = downloadTotal = 0;
        lastDownloadUiBytes = 0;
        lastDownloadUiMs = millis();
        HttpDownloader::DownloadOptions reconnectOptions;
        reconnectOptions.shouldCancel = pollCancel;
        reconnectOptions.bufferSize = OPDS_DOWNLOAD_BUFFER_SIZE;
        reconnectOptions.preservePartial = true;
        result = HttpDownloader::downloadToFile(downloadUrl, filename, progressCallback, &cancelRequested,
                                                server.username, server.password, reconnectOptions);
      }
    }
  }

  // A number of OPDS servers (including some PHP download endpoints) use
  // chunked transfer and close long sockets occasionally. Range-resume is not
  // safe for those endpoints and can make ESP-IDF allocate another decoded
  // response buffer. Retry the canonical URL once from byte zero instead.
  if (result != HttpDownloader::OK && result != HttpDownloader::ABORTED && !cancelRequested) {
    FsFile partial;
    size_t partialSize = 0;
    if (Storage.openFileForRead("OPDS", filename.c_str(), partial)) {
      partialSize = partial.fileSize();
      partial.close();
    }
    if (partialSize > 0) {
      LOG_INF("OPDS", "Retrying interrupted download from start: partial=%zu", partialSize);
      Storage.remove(filename.c_str());
      delay(400);
      downloadProgress = downloadTotal = 0;
      lastDownloadUiBytes = 0;
      lastDownloadUiMs = millis();

      HttpDownloader::DownloadOptions retryOptions;
      retryOptions.shouldCancel = pollCancel;
      retryOptions.bufferSize = OPDS_DOWNLOAD_BUFFER_SIZE;
      retryOptions.preservePartial = false;
      result = HttpDownloader::downloadToFile(downloadUrl, filename, progressCallback, &cancelRequested,
                                              server.username, server.password, retryOptions);
    }
  }

  // Legacy relative resolution is only meaningful for truly relative hrefs.
  // A protocol-relative URL beginning with // already names its host; turning
  // it into https://host//host/... is always wrong (exactly what iKnigi logs
  // showed), so never use the legacy fallback for that case.
  const bool protocolRelative = acquisition.href.rfind("//", 0) == 0;
  if (result != HttpDownloader::OK && result != HttpDownloader::ABORTED && !cancelRequested &&
      !protocolRelative && legacyDownloadUrl != downloadUrl) {
    LOG_INF("OPDS", "Download retry with legacy relative URL: %s", legacyDownloadUrl.c_str());
    Storage.remove(filename.c_str());
    downloadProgress = downloadTotal = 0;
    lastDownloadUiBytes = 0;
    lastDownloadUiMs = millis();
    HttpDownloader::DownloadOptions legacyOptions;
    legacyOptions.shouldCancel = pollCancel;
    legacyOptions.bufferSize = OPDS_DOWNLOAD_BUFFER_SIZE;
    result = HttpDownloader::downloadToFile(legacyDownloadUrl, filename, progressCallback, &cancelRequested,
                                            server.username, server.password, legacyOptions);
  }

  LOG_INF("OPDS", "Book download end heap: free=%u maxAlloc=%u result=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<int>(result));
  if (result == HttpDownloader::OK) {
    if (!Storage.exists(filename.c_str())) {
      LOG_ERR("OPDS", "Downloader returned OK but file is missing: %s", filename.c_str());
      result = HttpDownloader::FILE_ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
    } else {
      // Do not trust OPDS MIME blindly. Some servers advertise FB2+ZIP but
      // return raw FB2 XML. Detect the actual bytes and fix the extension so
      // FileBrowser opens the downloaded book with the correct engine.
      const DownloadedBookKind actualKind = sniffDownloadedBook(filename);
      std::string correctedFilename = filename;
      if (actualKind == DownloadedBookKind::FB2_XML) {
        correctedFilename = replaceBookExtension(filename, ".fb2");
      } else if (actualKind == DownloadedBookKind::ZIP && acquisition.format == OpdsBookFormat::FB2) {
        correctedFilename = replaceBookExtension(filename, ".fb2.zip");
      } else if (actualKind == DownloadedBookKind::UNKNOWN) {
        LOG_ERR("OPDS", "Downloaded payload is neither ZIP nor FB2 XML: %s", filename.c_str());
        Storage.remove(filename.c_str());
        result = HttpDownloader::FILE_ERROR;
        errorMessage = tr(STR_DOWNLOAD_FAILED);
        state = BrowserState::ERROR;
        requestUpdateAndWait();
        return;
      }

      if (correctedFilename != filename) {
        if (Storage.exists(correctedFilename.c_str())) Storage.remove(correctedFilename.c_str());
        if (!Storage.rename(filename.c_str(), correctedFilename.c_str())) {
          LOG_ERR("OPDS", "Failed to correct downloaded extension: %s -> %s", filename.c_str(),
                  correctedFilename.c_str());
          Storage.remove(filename.c_str());
          result = HttpDownloader::FILE_ERROR;
          errorMessage = tr(STR_DOWNLOAD_FAILED);
          state = BrowserState::ERROR;
          requestUpdateAndWait();
          return;
        }
        LOG_INF("OPDS", "Corrected downloaded format: %s -> %s", filename.c_str(), correctedFilename.c_str());
        filename = correctedFilename;
      }

      size_t savedSize = 0;
      FsFile savedFile;
      if (Storage.openFileForRead("OPDS", filename.c_str(), savedFile)) {
        savedSize = savedFile.fileSize();
        savedFile.close();
      }
      LOG_INF("OPDS", "Saved book: %s size=%zu", filename.c_str(), savedSize);
      clearBookCache(filename);

      // Give the user an unambiguous confirmation instead of instantly
      // snapping back to the catalog and making a successful transfer look
      // like nothing happened.
      state = BrowserState::LOADING;
      char savedText[64];
      snprintf(savedText, sizeof(savedText), "Сохранено: %zu KB", savedSize / 1024);
      statusMessage = savedText;
      requestUpdateAndWait();
      delay(1600);
    }
  } else if (result == HttpDownloader::ABORTED) {
    LOG_DBG("OPDS", "Download cancelled");
    mappedInput.suppressNextBackRelease();
  } else {
    // Resume attempts are finished; do not leave a truncated archive looking
    // like a valid book in the file browser.
    Storage.remove(filename.c_str());
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    state = BrowserState::ERROR;
    requestUpdateAndWait();
    // Keep the error on screen instead of immediately snapping back to the
    // catalogue. Confirm retries the feed; Back leaves it.
    return;
  }

  // The feed was deliberately released before TLS allocation. Reload it now
  // so browsing continues with a clean heap instead of retaining a fragmented
  // in-memory catalogue throughout the download.
  showLoadingBeforeFetch();
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  // OpenSearch optional pagination placeholders must not leak into the URL.
  for (const char* pageToken : {"{startPage?}", "{startIndex?}"}) {
    size_t tokenPos;
    while ((tokenPos = url.find(pageToken)) != std::string::npos) url.replace(tokenPos, strlen(pageToken), "0");
  }

  // Flibusta's search endpoint accepts searchType=books and pageNumber.
  // Older OPDS roots expose only searchTerm={searchTerms}, so complete the
  // query here to make the keyboard search deterministic and paginatable.
  if (looksLikeFlibusta(url)) {
    if (url.find("searchType=") == std::string::npos) url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("searchType=books");
    if (url.find("pageNumber=") == std::string::npos) url += "&pageNumber=0";
  }

  navigationHistory.push_back(currentPath);  // <-- add this
  currentPath = url;                         // <-- add this

  showLoadingBeforeFetch();
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  // Prepared OPDS catalogs are usable completely offline. Check the SD cache
  // before forcing the user through Wi-Fi selection.
  const std::string url = currentPath.find("http") == 0 ? currentPath : UrlUtils::buildUrl(server.url, currentPath);
  char baseBuf[64];
  snprintf(baseBuf, sizeof(baseBuf), "%s/cache_%08lx", OPDS_CACHE_DIR,
           static_cast<unsigned long>(opdsUrlHash(url)));
  const std::string metaPath = std::string(baseBuf) + ".meta.json";
  if (Storage.exists(metaPath.c_str())) {
    showLoadingBeforeFetch();
    fetchFeed(currentPath);
    return;
  }

  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    showLoadingBeforeFetch();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    showLoadingBeforeFetch();
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
