#include "BookInfoActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::string filenameOf(const std::string& p) {
  const auto pos = p.find_last_of('/');
  return pos == std::string::npos ? p : p.substr(pos + 1);
}
std::string humanSize(uint64_t bytes) {
  char buf[24];
  if (bytes >= 1048576ULL) snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
  else snprintf(buf, sizeof(buf), "%llu KB", static_cast<unsigned long long>(bytes / 1024ULL));
  return buf;
}
std::string formatOf(const std::string& p) {
  if (FsHelpers::hasEpubExtension(p)) return "EPUB";
  if (FsHelpers::checkFileExtension(p, ".fb2")) return "FB2";
  if (FsHelpers::checkFileExtension(p, ".zip")) return "ZIP / FB2.ZIP";
  if (FsHelpers::hasXtcExtension(p)) return "XTC / XTCH";
  if (FsHelpers::hasTxtExtension(p)) return "TXT";
  if (FsHelpers::hasMarkdownExtension(p)) return "Markdown";
  if (FsHelpers::hasPngExtension(p)) return "PNG";
  if (FsHelpers::hasBmpExtension(p)) return "BMP";
  return "-";
}

bool isBookLike(const std::string& p) {
  return FsHelpers::hasEpubExtension(p) || FsHelpers::checkFileExtension(p, ".fb2") ||
         FsHelpers::checkFileExtension(p, ".zip") || FsHelpers::hasXtcExtension(p) ||
         FsHelpers::hasTxtExtension(p) || FsHelpers::hasMarkdownExtension(p);
}
}

void BookInfoActivity::onEnter() { Activity::onEnter(); requestUpdate(true); }
void BookInfoActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) finish();
}
void BookInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  if (isBookLike(path_)) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageW, metrics.headerHeight}, tr(STR_BOOK_INFO));
  } else {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageW, metrics.headerHeight}, tr(STR_FILE_INFO));
  }

  uint64_t fileSize = 0;
  auto file = Storage.open(path_.c_str());
  if (file) { fileSize = file.size(); file.close(); }

  std::vector<std::pair<std::string,std::string>> rows;
  rows.push_back({tr(STR_BOOK_INFO_NAME), filenameOf(path_)});
  rows.push_back({tr(STR_BOOK_INFO_FORMAT), formatOf(path_)});
  rows.push_back({tr(STR_BOOK_INFO_SIZE), fileSize ? humanSize(fileSize) : "-"});
  if (FsHelpers::hasEpubExtension(path_)) {
    rows.push_back({tr(STR_BOOK_INFO_CACHE), Epub::hasCache(path_, "/.inkmod") ? tr(STR_YES) : tr(STR_NO)});
  }
  if (isBookLike(path_)) {
    const bool heavy = (FsHelpers::checkFileExtension(path_, ".fb2") && fileSize >= 20ULL * 1024ULL * 1024ULL) ||
                       (FsHelpers::hasEpubExtension(path_) && fileSize >= 20ULL * 1024ULL * 1024ULL);
    rows.push_back({tr(STR_BOOK_INFO_PROFILE), heavy ? tr(STR_BOOK_INFO_HEAVY) : tr(STR_BOOK_INFO_NORMAL)});
  }

  const int left = metrics.contentSidePadding;
  const int right = pageW - metrics.contentSidePadding;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  for (const auto& row : rows) {
    if (y + lineH >= pageH - metrics.buttonHintsHeight - metrics.verticalSpacing) break;
    renderer.drawText(UI_10_FONT_ID, left, y, row.first.c_str());
    const auto valueLines = renderer.wrappedText(UI_10_FONT_ID, row.second.c_str(), std::max(80, pageW/2), 2);
    int vy=y;
    for (const auto& line : valueLines) {
      const int w=renderer.getTextWidth(UI_10_FONT_ID,line.c_str());
      renderer.drawText(UI_10_FONT_ID, std::max(left, right-w), vy, line.c_str());
      vy += lineH;
    }
    y = std::max(y+lineH+metrics.verticalSpacing, vy+metrics.verticalSpacing);
  }
  const auto labels=mappedInput.mapLabels(tr(STR_BACK),"","","");
  GUI.drawButtonHints(renderer, labels.btn1,labels.btn2,labels.btn3,labels.btn4,true);
  renderer.displayBuffer();
}
