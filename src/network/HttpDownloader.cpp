#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <SecureHttpClient.h>
#include <esp_http_client.h>
#include <strings.h>

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#include "AppVersion.h"
#include "network/WifiPowerSaveGuard.h"

namespace {
// E-paper refreshes are slow (~500 ms on X4). Updating every 250 ms stalls
// the HTTP read loop, increases the chance of server-side disconnects, and
// makes changing counters ghost over each other. Keep progress useful but
// deliberately sparse while the socket is active.
constexpr size_t PROGRESS_UPDATE_BYTES = 256 * 1024;
constexpr uint32_t PROGRESS_UPDATE_MS = 3000;
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr int OPDS_STREAM_TIMEOUT_MS = 15000;
constexpr int HTTP_READ_POLL_TIMEOUT_MS = 5000;
constexpr uint32_t DOWNLOAD_IDLE_TIMEOUT_MS = 30000;
constexpr size_t DEFAULT_DOWNLOAD_BUFFER_SIZE = 2048;
constexpr uint8_t MAX_REDIRECTS = 5;

void logNetworkState(const char* phase) {
  LOG_DBG("HTTP", "%s: heap free=%u maxAlloc=%u wifi=%d rssi=%d", phase, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<int>(WiFi.status()), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
}

void logDownloadState(const char* phase, const size_t downloaded, const size_t total, const uint32_t idleMs) {
  LOG_ERR("HTTP", "%s after %zu/%zu bytes (idle=%lu ms, timeout=%lu ms)", phase, downloaded, total,
          static_cast<unsigned long>(idleMs), static_cast<unsigned long>(DOWNLOAD_IDLE_TIMEOUT_MS));
  logNetworkState(phase);
}

bool isRedirect(const int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_err_t captureLocationHeader(esp_http_client_event_t* evt) {
  auto* location = static_cast<std::string*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_HEADER && location != nullptr && evt->header_key != nullptr &&
      evt->header_value != nullptr && strcasecmp(evt->header_key, "Location") == 0) {
    location->assign(evt->header_value);
  }
  return ESP_OK;
}

struct ParsedUrl {
  bool https = false;
  std::string host;
  std::string path;
  uint16_t port = 80;
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;

  const std::string scheme = url.substr(0, schemeEnd);
  out.https = scheme == "https";
  if (!out.https && scheme != "http") return false;

  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  const std::string hostPort =
      url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
  out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  out.port = out.https ? 443 : 80;

  const size_t portSep = hostPort.rfind(':');
  if (portSep != std::string::npos) {
    out.host = hostPort.substr(0, portSep);
    const std::string portText = hostPort.substr(portSep + 1);
    if (portText.empty()) return false;
    uint32_t parsedPort = 0;
    for (const char c : portText) {
      if (c < '0' || c > '9') return false;
      parsedPort = parsedPort * 10 + static_cast<uint32_t>(c - '0');
      if (parsedPort > UINT16_MAX) return false;
    }
    if (parsedPort == 0) return false;
    out.port = static_cast<uint16_t>(parsedPort);
  } else {
    out.host = hostPort;
  }

  return !out.host.empty() && !out.path.empty();
}

bool sameOrigin(const ParsedUrl& a, const ParsedUrl& b) {
  return a.https == b.https && a.port == b.port && strcasecmp(a.host.c_str(), b.host.c_str()) == 0;
}

const char* schemeName(const ParsedUrl& url) { return url.https ? "https" : "http"; }

std::string buildRedirectUrl(const std::string& baseUrl, const std::string& location) {
  if (location.starts_with("http://") || location.starts_with("https://")) return location;

  ParsedUrl base;
  if (!parseUrl(baseUrl, base)) return location;

  std::string origin = base.https ? "https://" : "http://";
  origin += base.host;
  if ((base.https && base.port != 443) || (!base.https && base.port != 80)) {
    origin += ":";
    origin += std::to_string(base.port);
  }

  if (!location.empty() && location[0] == '/') return origin + location;

  const size_t lastSlash = base.path.rfind('/');
  const std::string parent = lastSlash == std::string::npos ? "/" : base.path.substr(0, lastSlash + 1);
  return origin + parent + location;
}

bool isCancelRequested(bool* cancelFlag, const HttpDownloader::CancelCallback& shouldCancel) {
  if (cancelFlag && *cancelFlag) return true;
  if (shouldCancel && shouldCancel()) {
    if (cancelFlag) *cancelFlag = true;
    return true;
  }
  return false;
}

class ProgressNotifier {
 public:
  ProgressNotifier(size_t total, HttpDownloader::ProgressCallback progress)
      : total_(total), progress_(std::move(progress)) {}

  void notify(size_t downloaded, bool force) {
    if (!progress_) return;

    const uint32_t now = millis();
    const bool completedKnownLength = total_ > 0 && downloaded == total_;
    if (force || completedKnownLength || downloaded - lastProgressBytes_ >= PROGRESS_UPDATE_BYTES ||
        now - lastProgressMs_ >= PROGRESS_UPDATE_MS) {
      lastProgressBytes_ = downloaded;
      lastProgressMs_ = now;
      // total_ == 0 means chunked/unknown Content-Length. Still notify the
      // UI so it can show an indeterminate progress bar and byte counter.
      progress_(downloaded, total_);
    }
  }

 private:
  size_t total_;
  size_t lastProgressBytes_ = 0;
  uint32_t lastProgressMs_ = 0;
  HttpDownloader::ProgressCallback progress_;
};

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  HttpDownloader::CancelCallback shouldCancel;
  size_t resumeOffset = 0;
  size_t downloaded = 0;
  size_t total = 0;
  bool rangeIgnored = false;
};


HttpDownloader::DownloadError runGetWolfSslLowMemory(const std::string& url, Sink& sink,
                                                      const int timeoutMs = OPDS_STREAM_TIMEOUT_MS,
                                                      const char* label = "FILE") {
#if defined(FREEINK_NET_WOLFSSL)
  // The X4 has no PSRAM.  Some servers present a certificate chain large enough
  // that system mbedTLS fails while parsing X.509 with -0x2880
  // (MBEDTLS_ERR_X509_ALLOC_FAILED), even though ~50 KB total heap is free.
  // SecureNet/wolfSSL is already part of inkMOD specifically for low-memory TLS
  // and has a much smaller contiguous-allocation requirement.  Use it only as a
  // zero-byte fallback for public book payloads.  Credentials are deliberately
  // not accepted by this fallback so we never send OPDS passwords over an
  // unverified transport.
  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(timeoutMs);
  http.setFollowRedirects(MAX_REDIRECTS);
  http.setAllowRedirectDowngrade(false);
  http.setReuse(false);
  // Prefer TLS 1.2 for the low-memory file path. On the X4, TLS 1.3 was able
  // to handshake but then wolfSSL_read() failed with MEMORY_E (-125) when the
  // first application record arrived. TLS 1.2 keeps a smaller live working set.
  http.setPreferTls12(true);
  http.setUserAgent(std::string("inkMOD-ESP32-") + INKMOD_VERSION);
  if (!http.begin(url)) {
    LOG_ERR("HTTP", "%s wolfSSL fallback rejected URL", label);
    return HttpDownloader::HTTP_ERROR;
  }
  if (sink.resumeOffset > 0) {
    char rangeHeader[40];
    snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%zu-", sink.resumeOffset);
    http.addHeader("Range", rangeHeader);
  }
  http.addHeader("Accept-Encoding", "identity");

  sink.downloaded = sink.resumeOffset;
  LOG_INF("HTTP", "%s wolfSSL low-memory fallback start: free=%u maxAlloc=%u", label,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  const int status = http.GET(
      [&](const uint8_t* data, const size_t len) {
        if (isCancelRequested(sink.cancelFlag, sink.shouldCancel)) return false;
        // Do not save HTML/error bodies as a .fb2.zip partial. The old code
        // wrote a 403 page first and only checked the status afterwards, which
        // created a fake partial file and triggered a pointless resume retry.
        const int responseStatus = http.getStatus();
        if (responseStatus != 200 && responseStatus != 206) return true;
        if (!sink.write(data, len)) return false;
        sink.downloaded += len;
        if (sink.progress) {
          const size_t contentLength = http.getContentLength();
          sink.total = contentLength > 0 ? sink.resumeOffset + contentLength : 0;
          sink.progress(sink.downloaded, sink.total);
        }
        return true;
      },
      [&]() { return isCancelRequested(sink.cancelFlag, sink.shouldCancel); });

  if (http.callbackAborted() || http.aborted()) {
    http.end();
    return HttpDownloader::ABORTED;
  }
  const size_t contentLength = http.getContentLength();
  sink.total = contentLength > 0 ? sink.resumeOffset + contentLength : 0;
  const bool resumeResponse = sink.resumeOffset > 0 && status == 206;
  if (sink.resumeOffset > 0 && !resumeResponse) {
    sink.rangeIgnored = true;
    http.end();
    return HttpDownloader::HTTP_ERROR;
  }
  if (status != 200 && status != 206) {
    LOG_ERR("HTTP", "%s wolfSSL fallback HTTP status=%d", label, status);
    http.end();
    return HttpDownloader::HTTP_ERROR;
  }
  if (!http.responseComplete()) {
    LOG_ERR("HTTP", "%s wolfSSL fallback body incomplete: bytes=%zu", label, sink.downloaded);
    http.end();
    return HttpDownloader::HTTP_ERROR;
  }
  http.end();
  LOG_INF("HTTP", "%s wolfSSL fallback done: bytes=%zu free=%u maxAlloc=%u", label,
          sink.downloaded, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return sink.downloaded > sink.resumeOffset ? HttpDownloader::OK : HttpDownloader::HTTP_ERROR;
#else
  (void)url; (void)sink; (void)timeoutMs; (void)label;
  return HttpDownloader::HTTP_ERROR;
#endif
}

void setRequestHeaders(esp_http_client_handle_t client, const std::string& username, const std::string& password,
                       size_t resumeOffset, bool sendAuthorization);
void logTlsError(esp_http_client_handle_t client, const char* phase);

// esp_http_client_fetch_headers() may receive the first body bytes together
// with the headers. ESP-IDF then reallocates an internal "decoded data"
// cache before the caller can start esp_http_client_read(). On the ESP32-C3
// that temporary cache can fail on fragmented TLS heap (seen with OPDS).
// Stream fetches use esp_http_client_perform() instead: HTTP_EVENT_ON_DATA
// delivers body chunks directly and bypasses that fetch-headers body cache.
struct StreamPerformContext {
  Sink* sink = nullptr;
  std::string* redirectLocation = nullptr;
  const char* label = "stream";
  bool writeFailed = false;
  bool aborted = false;
  size_t lastProgressBytes = 0;
  uint32_t lastProgressMs = 0;
};

esp_err_t streamPerformEvent(esp_http_client_event_t* evt) {
  auto* ctx = static_cast<StreamPerformContext*>(evt->user_data);
  if (!ctx) return ESP_OK;

  switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
      LOG_INF("HTTP", "%s connected: free=%u maxAlloc=%u", ctx->label, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      break;
    case HTTP_EVENT_ON_FINISH:
      LOG_INF("HTTP", "%s finish: free=%u maxAlloc=%u", ctx->label, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      break;
    case HTTP_EVENT_DISCONNECTED:
      LOG_INF("HTTP", "%s disconnected: free=%u maxAlloc=%u", ctx->label, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      break;
    default:
      break;
  }

  if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
    if (ctx->redirectLocation && strcasecmp(evt->header_key, "Location") == 0) {
      ctx->redirectLocation->assign(evt->header_value);
      return ESP_OK;
    }
    if (ctx->sink && strcasecmp(evt->header_key, "Content-Length") == 0) {
      const unsigned long long body = strtoull(evt->header_value, nullptr, 10);
      if (body > 0) ctx->sink->total = ctx->sink->resumeOffset + static_cast<size_t>(body);
    }
  }

  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0 && ctx->sink) {
    // Do not write redirect response bodies. The Location header is handled
    // after perform() returns and the next request starts cleanly.
    const int status = esp_http_client_get_status_code(evt->client);
    if (isRedirect(status)) return ESP_OK;
    if (ctx->sink->resumeOffset > 0 && status != 206) {
      ctx->sink->rangeIgnored = true;
      return ESP_OK;
    }

    if (isCancelRequested(ctx->sink->cancelFlag, ctx->sink->shouldCancel)) {
      ctx->aborted = true;
      return ESP_FAIL;
    }

    const size_t len = static_cast<size_t>(evt->data_len);
    if (!ctx->sink->write(reinterpret_cast<const uint8_t*>(evt->data), len)) {
      ctx->writeFailed = true;
      return ESP_FAIL;
    }
    ctx->sink->downloaded += len;

    if (ctx->sink->progress) {
      const uint32_t now = millis();
      const bool enoughBytes = ctx->sink->downloaded >= ctx->lastProgressBytes + PROGRESS_UPDATE_BYTES;
      const bool enoughTime = now - ctx->lastProgressMs >= PROGRESS_UPDATE_MS;
      const bool complete = ctx->sink->total > 0 && ctx->sink->downloaded >= ctx->sink->total;
      if (ctx->lastProgressBytes == 0 || enoughBytes || enoughTime || complete) {
        ctx->lastProgressBytes = ctx->sink->downloaded;
        ctx->lastProgressMs = now;
        ctx->sink->progress(ctx->sink->downloaded, ctx->sink->total);
      }
    }
  }
  return ESP_OK;
}

HttpDownloader::DownloadError runGetStreamPerform(const std::string& url, const std::string& username,
                                                   const std::string& password, Sink& sink,
                                                   const int timeoutMs = OPDS_STREAM_TIMEOUT_MS,
                                                   const char* label = "OPDS") {
  std::string currentUrl = url;

  ParsedUrl credentialOrigin;
  const bool hasCredentials = !username.empty() && !password.empty() && parseUrl(url, credentialOrigin);

  for (uint8_t hop = 0; hop < MAX_REDIRECTS; ++hop) {
    ParsedUrl currentOrigin;
    const bool currentParsed = parseUrl(currentUrl, currentOrigin);
    const bool sendAuthorization = hasCredentials && currentParsed && sameOrigin(currentOrigin, credentialOrigin);
    std::string redirectLocation;

    StreamPerformContext ctx;
    ctx.sink = &sink;
    ctx.redirectLocation = &redirectLocation;
    ctx.label = label;

    esp_http_client_config_t config = {};
    config.url = currentUrl.c_str();
    // Small buffers leave more contiguous heap for TLS on the no-PSRAM C3.
    // Both OPDS feeds and book files are streamed directly to their sink.
    config.buffer_size = 1536;
    config.buffer_size_tx = 768;
    config.timeout_ms = timeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = false;
    config.disable_auto_redirect = true;
    config.event_handler = streamPerformEvent;
    config.user_data = &ctx;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "Stream client init failed");
      logNetworkState("Stream client init failure");
      return HttpDownloader::HTTP_ERROR;
    }

    setRequestHeaders(client, username, password, sink.resumeOffset, sendAuthorization);
    // Avoid ESP-IDF's decoded-body staging buffers on the no-PSRAM C3.
    // OPDS XML and book archives are already compact enough; writing the raw
    // response directly to SD is both safer and cheaper in contiguous RAM.
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    LOG_INF("HTTP", "%s stream start: free=%u maxAlloc=%u", label, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    sink.downloaded = sink.resumeOffset;
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    const int64_t contentLength = esp_http_client_get_content_length(client);

    if (ctx.aborted) {
      LOG_INF("HTTP", "%s stream aborted after %zu bytes", label, sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    if (ctx.writeFailed) {
      LOG_ERR("HTTP", "Streaming sink aborted after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "Streaming perform failed: %s", esp_err_to_name(err));
      logTlsError(client, "Streaming perform failure");
      logNetworkState("Streaming perform failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    if (isRedirect(status)) {
      if (redirectLocation.empty()) {
        LOG_ERR("HTTP", "Stream redirect missing Location header");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      const std::string redirectUrl = buildRedirectUrl(currentUrl, redirectLocation);
      ParsedUrl redirect;
      if (!parseUrl(redirectUrl, redirect)) {
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (currentParsed && currentOrigin.https && !redirect.https) {
        LOG_ERR("HTTP", "Rejected HTTPS downgrade redirect");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (hasCredentials && !sameOrigin(redirect, credentialOrigin)) {
        LOG_ERR("HTTP", "Rejected credentialed stream redirect to different origin");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      currentUrl = redirectUrl;
      esp_http_client_cleanup(client);
      continue;
    }

    const bool resumeResponse = sink.resumeOffset > 0 && status == 206;
    if (sink.resumeOffset > 0 && !resumeResponse) {
      // The server ignored Range. Tell downloadToFile() to restart cleanly
      // instead of appending a full response to the partial file.
      sink.rangeIgnored = true;
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (status != 200 && status != 206) {
      LOG_ERR("HTTP", "Unexpected stream status: %d", status);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    if (contentLength > 0) sink.total = sink.resumeOffset + static_cast<size_t>(contentLength);
    if (sink.progress) sink.progress(sink.downloaded, sink.total);
    esp_http_client_cleanup(client);
    LOG_INF("HTTP", "%s stream done: bytes=%zu free=%u maxAlloc=%u", label, sink.downloaded, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return sink.downloaded > sink.resumeOffset ? HttpDownloader::OK : HttpDownloader::HTTP_ERROR;
  }

  return HttpDownloader::HTTP_ERROR;
}

void setRequestHeaders(esp_http_client_handle_t client, const std::string& username, const std::string& password,
                       size_t resumeOffset, bool sendAuthorization) {
  esp_http_client_set_header(client, "User-Agent", "inkMOD-ESP32-" INKMOD_VERSION);
  esp_http_client_set_header(client, "Connection", "close");
  if (resumeOffset > 0) {
    char rangeHeader[40];
    snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%zu-", resumeOffset);
    esp_http_client_set_header(client, "Range", rangeHeader);
    LOG_DBG("HTTP", "Resuming download at byte %zu", resumeOffset);
  }
  if (sendAuthorization) {
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }
}

void logTlsError(esp_http_client_handle_t client, const char* phase) {
  int tlsError = 0;
  int tlsFlags = 0;
  const esp_err_t err = esp_http_client_get_and_clear_last_tls_error(client, &tlsError, &tlsFlags);
  if (err != ESP_OK || tlsError != 0 || tlsFlags != 0) {
    const int tlsCode = tlsError < 0 ? -tlsError : tlsError;
    LOG_ERR("HTTP", "%s TLS error: err=%s mbedtls=0x%x flags=0x%x", phase, esp_err_to_name(err), tlsCode, tlsFlags);
  }
}

HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink, const size_t bufferSize) {
  std::string currentUrl = url;

  ParsedUrl credentialOrigin;
  const bool hasCredentials = !username.empty() && !password.empty() && parseUrl(url, credentialOrigin);

  for (uint8_t hop = 0; hop < MAX_REDIRECTS; ++hop) {
    ParsedUrl currentOrigin;
    const bool currentParsed = parseUrl(currentUrl, currentOrigin);
    const bool sendAuthorization = hasCredentials && currentParsed && sameOrigin(currentOrigin, credentialOrigin);
    std::string redirectLocation;

    esp_http_client_config_t config = {};
    config.url = currentUrl.c_str();
    config.buffer_size = HTTP_RX_BUF;
    config.buffer_size_tx = HTTP_TX_BUF;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = false;
    config.event_handler = captureLocationHeader;
    config.user_data = &redirectLocation;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "Client init failed");
      logNetworkState("Client init failure");
      return HttpDownloader::HTTP_ERROR;
    }

    setRequestHeaders(client, username, password, sink.resumeOffset, sendAuthorization);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "Open failed: %s", esp_err_to_name(err));
      logTlsError(client, "Open failure");
      logNetworkState("Open failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    int64_t responseLength = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (responseLength < 0) {
      LOG_ERR("HTTP", "Fetch headers failed: %lld", static_cast<long long>(responseLength));
      logNetworkState("Fetch headers failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    if (isRedirect(status)) {
      if (redirectLocation.empty()) {
        LOG_ERR("HTTP", "Redirect missing Location header");
        logNetworkState("Redirect missing Location");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }

      const std::string redirectUrl = buildRedirectUrl(currentUrl, redirectLocation);
      ParsedUrl redirect;
      if (!parseUrl(redirectUrl, redirect)) {
        LOG_ERR("HTTP", "Rejected redirect with unsupported Location");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (currentParsed && currentOrigin.https && !redirect.https) {
        LOG_ERR("HTTP", "Rejected HTTPS downgrade redirect to %s", redirect.host.c_str());
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (hasCredentials && !sameOrigin(redirect, credentialOrigin)) {
        LOG_ERR("HTTP", "Rejected credentialed redirect to different origin: %s://%s:%u", schemeName(redirect),
                redirect.host.c_str(), redirect.port);
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      currentUrl = redirectUrl;
      LOG_DBG("HTTP", "Redirecting to: %s", redirect.host.c_str());
      esp_http_client_cleanup(client);
      continue;
    }

    const bool isResumeResponse = sink.resumeOffset > 0 && status == 206;
    if (status != 200 && !isResumeResponse) {
      LOG_ERR("HTTP", "Unexpected status: %d", status);
      logNetworkState("Unexpected status");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (sink.resumeOffset > 0 && !isResumeResponse) {
      LOG_DBG("HTTP", "Server ignored range request; restarting download");
      sink.rangeIgnored = true;
      sink.resumeOffset = 0;
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    const size_t bodyLength = responseLength > 0 ? static_cast<size_t>(responseLength) : 0;
    sink.total = bodyLength > 0 ? sink.resumeOffset + bodyLength : 0;
    sink.downloaded = sink.resumeOffset;
    if (sink.total > 0) {
      LOG_DBG("HTTP", "Content-Length: %zu", sink.total);
    } else {
      LOG_DBG("HTTP", "Content-Length: unknown");
    }
#ifdef ESP_ERR_HTTP_EAGAIN
    err = esp_http_client_set_timeout_ms(client, HTTP_READ_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "Failed to set read timeout: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
#endif

    auto buffer = makeUniqueNoThrow<char[]>(bufferSize);
    if (!buffer) {
      LOG_ERR("HTTP", "Failed to allocate %zu byte download buffer", bufferSize);
      logNetworkState("Download buffer allocation failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    ProgressNotifier progressNotifier(sink.total, std::move(sink.progress));
    LOG_DBG("HTTP", "Reading body: buffer=%zu bytes", bufferSize);
#ifdef ESP_ERR_HTTP_EAGAIN
    uint32_t lastReadMs = millis();
#endif
    while (true) {
      if (isCancelRequested(sink.cancelFlag, sink.shouldCancel)) {
        esp_http_client_cleanup(client);
        return HttpDownloader::ABORTED;
      }

      const int bytesRead = esp_http_client_read(client, buffer.get(), bufferSize);
      if (bytesRead < 0) {
#ifdef ESP_ERR_HTTP_EAGAIN
        if (bytesRead == -ESP_ERR_HTTP_EAGAIN) {
          const uint32_t idleMs = millis() - lastReadMs;
          if (idleMs >= DOWNLOAD_IDLE_TIMEOUT_MS) {
            logDownloadState("Read timed out", sink.downloaded, sink.total, idleMs);
            esp_http_client_cleanup(client);
            return HttpDownloader::HTTP_ERROR;
          }
          delay(1);
          continue;
        }
#endif
        LOG_ERR("HTTP", "Read error after %zu/%zu bytes", sink.downloaded, sink.total);
        logNetworkState("Read error");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (bytesRead == 0) break;

      if (!sink.write(reinterpret_cast<const uint8_t*>(buffer.get()), static_cast<size_t>(bytesRead))) {
        LOG_ERR("HTTP", "Write failed after %zu/%zu bytes", sink.downloaded, sink.total);
        logNetworkState("Write failure");
        esp_http_client_cleanup(client);
        return HttpDownloader::FILE_ERROR;
      }

      sink.downloaded += static_cast<size_t>(bytesRead);
#ifdef ESP_ERR_HTTP_EAGAIN
      lastReadMs = millis();
#endif
      if (sink.total > 0 && sink.total <= PROGRESS_UPDATE_BYTES) {
        LOG_DBG("HTTP", "Read progress: %zu/%zu bytes", sink.downloaded, sink.total);
      }
      progressNotifier.notify(sink.downloaded, false);
      if (sink.total > 0 && sink.downloaded >= sink.total) break;
      delay(0);
    }

    const bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    progressNotifier.notify(sink.downloaded, true);
    if (!complete) {
      LOG_ERR("HTTP", "Incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      logNetworkState("Incomplete transfer");
      return HttpDownloader::HTTP_ERROR;
    }

    return HttpDownloader::OK;
  }

  LOG_ERR("HTTP", "Redirect limit exceeded");
  logNetworkState("Redirect limit exceeded");
  return HttpDownloader::HTTP_ERROR;
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetStreamPerform(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  outContent.clear();
  return fetchUrl(
      url,
      [&outContent](const uint8_t* data, size_t len) {
        outContent.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      username, password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  if (!onData) {
    LOG_ERR("HTTP", "Fetch failed: missing data callback");
    return false;
  }

  Sink sink;
  sink.write = onData;
  // OPDS/text streaming must avoid esp_http_client_fetch_headers(): on the
  // ESP32-C3 it may allocate an internal decoded-data buffer after TLS has
  // already fragmented the heap. The perform/event path forwards body chunks
  // directly to the SD sink and keeps peak contiguous-RAM demand much lower.
  return runGetStreamPerform(url, username, password, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             DownloadOptions options) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  const size_t bufferSize = options.bufferSize > 0 ? options.bufferSize : DEFAULT_DOWNLOAD_BUFFER_SIZE;
  size_t resumeOffset = 0;
  if (options.resumePartial && Storage.exists(destPath.c_str())) {
    FsFile existingFile;
    if (Storage.openFileForRead("HTTP", destPath.c_str(), existingFile)) {
      resumeOffset = existingFile.fileSize();
      existingFile.close();
    }
  }

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());
  LOG_DBG("HTTP", "Timeout: %d ms buffer=%zu bytes", HTTP_TIMEOUT_MS, bufferSize);

  if (resumeOffset == 0 && Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.shouldCancel = std::move(options.shouldCancel);
  sink.resumeOffset = resumeOffset;

  FsFile file;
  bool fileOpen = false;
  auto openOutputFile = [&]() {
    if (fileOpen) return true;
    if (sink.resumeOffset > 0) {
      file = Storage.open(destPath.c_str(), O_WRONLY | O_APPEND);
    } else {
      fileOpen = Storage.openFileForWrite("HTTP", destPath.c_str(), file);
      if (!fileOpen) {
        LOG_ERR("HTTP", "Failed to open file for writing");
        return false;
      }
    }
    fileOpen = file;
    if (!fileOpen) {
      LOG_ERR("HTTP", "Failed to open file for writing");
    }
    return fileOpen;
  };

  sink.write = [&](const uint8_t* data, size_t len) { return openOutputFile() && file.write(data, len) == len; };

  // Large public HTTPS payloads on the X4 need a different TLS strategy from
  // tiny OPDS feeds. If the largest contiguous block is already below 48 KB,
  // skip system mbedTLS completely: a failed mbedTLS setup fragments the heap
  // further, and our logs showed wolfSSL then entering with only ~24 KB maxAlloc.
  // Go straight to the low-memory TLS 1.2-first path while the heap is still
  // fresh. Credentialed OPDS URLs stay on verified system TLS.
  const bool publicHttps = url.rfind("https://", 0) == 0 && username.empty() && password.empty();
  const bool lowContiguousHeap = ESP.getMaxAllocHeap() < 48 * 1024;
  DownloadError result;
  if (publicHttps && lowContiguousHeap) {
    LOG_INF("HTTP", "FILE choosing direct low-memory TLS path: free=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    result = runGetWolfSslLowMemory(url, sink, HTTP_TIMEOUT_MS, "FILE");
  } else {
    // Use the same event-driven body path as OPDS feeds. It avoids
    // esp_http_client_fetch_headers() staging body bytes in a temporary decoded
    // buffer while TLS already owns most contiguous RAM on ESP32-C3.
    result = runGetStreamPerform(url, username, password, sink, HTTP_TIMEOUT_MS, "FILE");
  }

  // A zero-byte HTTPS failure with a healthy Wi-Fi link is commonly the C3
  // X.509 allocation failure (-0x2880) seen on iKnigi.  mbedTLS needs a larger
  // contiguous block than the browser can leave after Wi-Fi is up.  Fall back
  // to inkMOD's already-linked low-memory wolfSSL transport.  Never do this for
  // credentialed URLs because SecureNet currently has no CA bundle verifier.
  if (result == HTTP_ERROR && sink.downloaded == sink.resumeOffset &&
      publicHttps && !lowContiguousHeap) {
    delay(40);
    result = runGetWolfSslLowMemory(url, sink, HTTP_TIMEOUT_MS, "FILE");
  }

  if (sink.rangeIgnored) {
    if (fileOpen) {
      file.close();
      fileOpen = false;
    }
    Storage.remove(destPath.c_str());
    sink.rangeIgnored = false;
    sink.resumeOffset = 0;
    sink.downloaded = 0;
    sink.total = 0;
    sink.write = [&](const uint8_t* data, size_t len) { return openOutputFile() && file.write(data, len) == len; };
    if (publicHttps && ESP.getMaxAllocHeap() < 48 * 1024) {
      result = runGetWolfSslLowMemory(url, sink, HTTP_TIMEOUT_MS, "FILE");
    } else {
      result = runGetStreamPerform(url, username, password, sink, HTTP_TIMEOUT_MS, "FILE");
      if (result == HTTP_ERROR && sink.downloaded == 0 && publicHttps) {
        delay(40);
        result = runGetWolfSslLowMemory(url, sink, HTTP_TIMEOUT_MS, "FILE");
      }
    }
  }

  if (fileOpen) {
    file.flush();
    file.close();
  }

  if (result != OK) {
    LOG_ERR("HTTP", "Transfer failed: error=%d downloaded=%zu expected=%zu preservePartial=%d resumePartial=%d",
            static_cast<int>(result), sink.downloaded, sink.total, options.preservePartial, options.resumePartial);
    if (result == ABORTED || !options.preservePartial) {
      Storage.remove(destPath.c_str());
    }
    return result;
  }

  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    if (!options.preservePartial) {
      Storage.remove(destPath.c_str());
    }
    return HTTP_ERROR;
  }

  if (sink.total > 0 && sink.downloaded != sink.total) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", sink.downloaded, sink.total);
    if (!options.preservePartial) {
      Storage.remove(destPath.c_str());
    }
    return HTTP_ERROR;
  }

  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}
