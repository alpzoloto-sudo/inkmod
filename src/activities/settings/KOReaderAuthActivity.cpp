#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::vector<std::string> wrapCenteredMessage(const GfxRenderer& renderer, const std::string& text, const int maxWidth,
                                             const size_t maxLines = 3) {
  std::vector<std::string> lines;
  if (text.empty() || maxWidth <= 0 || maxLines == 0) return lines;

  size_t pos = 0;
  while (pos < text.size() && lines.size() < maxLines) {
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size()) break;

    size_t bestEnd = pos;
    size_t scan = pos;
    while (scan < text.size()) {
      size_t wordEnd = text.find(' ', scan);
      if (wordEnd == std::string::npos) wordEnd = text.size();
      const std::string candidate = text.substr(pos, wordEnd - pos);
      if (renderer.getTextWidth(UI_10_FONT_ID, candidate.c_str()) > maxWidth) break;
      bestEnd = wordEnd;
      if (wordEnd == text.size()) break;
      scan = wordEnd + 1;
    }

    if (bestEnd == pos) {
      // A single unbroken token (URL/error code) can be wider than the screen.
      // Fall back to the renderer's UTF-8-safe truncation rather than drawing
      // outside the panel.
      std::string remaining = text.substr(pos);
      lines.push_back(renderer.truncatedText(UI_10_FONT_ID, remaining.c_str(), maxWidth));
      pos = text.size();
      break;
    }

    lines.push_back(text.substr(pos, bestEnd - pos));
    pos = bestEnd;
  }

  if (pos < text.size() && !lines.empty()) {
    // The message needs more lines than the compact error area allows. Keep
    // the last visible line inside the screen and make the truncation clear.
    const std::string suffix = "...";
    const int suffixWidth = renderer.getTextWidth(UI_10_FONT_ID, suffix.c_str());
    const int available = std::max(1, maxWidth - suffixWidth);
    lines.back() = renderer.truncatedText(UI_10_FONT_ID, lines.back().c_str(), available) + suffix;
  }
  return lines;
}
}  // namespace

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = tr(STR_AUTHENTICATING);
  }
  requestUpdate();

  performAuthentication();
}

void KOReaderAuthActivity::performAuthentication() {
  const auto result = KOReaderSyncClient::authenticate();

  {
    RenderLock lock(*this);
    if (result == KOReaderSyncClient::OK) {
      state = SUCCESS;
      statusMessage = tr(STR_AUTH_SUCCESS);
    } else {
      state = FAILED;
      errorMessage = KOReaderSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void KOReaderAuthActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderAuthActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void KOReaderAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOREADER_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_SUCCESS), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_SYNC_READY));
  } else if (state == FAILED) {
    constexpr int sideMargin = 24;
    constexpr int messageGap = 8;
    const int maxTextWidth = std::max(1, pageWidth - sideMargin * 2);
    const auto lines = wrapCenteredMessage(renderer, errorMessage, maxTextWidth);
    const int totalMessageHeight = static_cast<int>(lines.size()) * height;
    const int titleY = top - totalMessageHeight / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, titleY, tr(STR_AUTH_FAILED), true, EpdFontFamily::BOLD);
    int lineY = titleY + height + messageGap;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, lineY, line.c_str());
      lineY += height;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void KOReaderAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}
