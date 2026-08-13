#include "ClippingSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <utility>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

ClippingSelectionActivity::ClippingSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     Section& section, const int fontId,
                                                     const int marginLeft, const int marginTop)
    : Activity("ClippingSelection", renderer, mappedInput),
      section_(section),
      fontId_(fontId),
      marginLeft_(marginLeft),
      marginTop_(marginTop) {}

void ClippingSelectionActivity::onEnter() {
  Activity::onEnter();
  originalPage_ = std::clamp(section_.currentPage, 0, std::max(0, static_cast<int>(section_.pageCount) - 1));
  currentPage_ = originalPage_;
  loadPage(currentPage_);
  cursor_ = nearestCenterWord();
  anchorPage_ = currentPage_;
  anchorWord_ = cursor_;
  requestUpdate(true);
}

void ClippingSelectionActivity::onExit() {
  // Selection is only a modal view; leave the parent reader on its original
  // page until the result handler explicitly decides where to go.
  section_.currentPage = originalPage_;
  Activity::onExit();
}

bool ClippingSelectionActivity::loadPage(const int pageNumber, const bool selectLastWord) {
  if (pageNumber < 0 || pageNumber >= static_cast<int>(section_.pageCount)) return false;
  const int previousSectionPage = section_.currentPage;
  section_.currentPage = pageNumber;
  auto next = section_.loadPageFromSectionFile();
  section_.currentPage = previousSectionPage;
  if (!next) return false;

  page_ = std::move(next);
  words_ = ClippingUtils::collectWords(*page_);
  currentPage_ = pageNumber;
  if (words_.empty()) {
    cursor_ = 0;
  } else {
    cursor_ = selectLastWord ? words_.size() - 1 : 0;
  }
  return true;
}

size_t ClippingSelectionActivity::nearestCenterWord() const {
  if (!page_ || words_.empty()) return 0;
  const int centerX = renderer.getScreenWidth() / 2;
  const int centerY = renderer.getScreenHeight() / 2;
  long best = LONG_MAX;
  size_t bestIndex = 0;

  for (size_t i = 0; i < words_.size(); ++i) {
    const auto& ref = words_[i];
    const auto& line = static_cast<const PageLine&>(*page_->elements[ref.elementIndex]);
    const auto& block = line.getBlock();
    if (!block || ref.wordIndex >= block->getWordXPositions().size()) continue;
    const long x = marginLeft_ + line.xPos + block->getWordXPositions()[ref.wordIndex];
    const long y = marginTop_ + line.yPos;
    const long dx = x - centerX;
    const long dy = y - centerY;
    const long distance = dx * dx + dy * dy;
    if (distance < best) {
      best = distance;
      bestIndex = i;
    }
  }
  return bestIndex;
}

void ClippingSelectionActivity::moveHorizontal(const int delta) {
  if (words_.empty() || delta == 0) return;

  if (delta > 0 && cursor_ + 1 >= words_.size()) {
    if (currentPage_ + 1 < static_cast<int>(section_.pageCount) && loadPage(currentPage_ + 1, false)) {
      requestUpdate();
    }
    return;
  }
  if (delta < 0 && cursor_ == 0) {
    if (currentPage_ > 0 && loadPage(currentPage_ - 1, true)) {
      requestUpdate();
    }
    return;
  }

  cursor_ = static_cast<size_t>(static_cast<int>(cursor_) + delta);
  requestUpdate();
}

void ClippingSelectionActivity::jumpPage(const int direction) {
  if (direction == 0) return;
  const int target = currentPage_ + direction;
  if (target < 0 || target >= static_cast<int>(section_.pageCount)) return;
  if (loadPage(target, direction < 0)) requestUpdate();
}

void ClippingSelectionActivity::moveVertical(const int direction) {
  if (!page_ || words_.empty() || direction == 0) return;
  const auto& currentRef = words_[cursor_];
  const auto& currentLine = static_cast<const PageLine&>(*page_->elements[currentRef.elementIndex]);
  const auto& currentBlock = currentLine.getBlock();
  if (!currentBlock || currentRef.wordIndex >= currentBlock->getWordXPositions().size()) return;

  const int wantedX = currentLine.xPos + currentBlock->getWordXPositions()[currentRef.wordIndex];
  const int currentY = currentLine.yPos;
  int bestDy = INT_MAX;
  int bestDx = INT_MAX;
  size_t best = cursor_;

  for (size_t i = 0; i < words_.size(); ++i) {
    const auto& ref = words_[i];
    const auto& line = static_cast<const PageLine&>(*page_->elements[ref.elementIndex]);
    const int dy = line.yPos - currentY;
    if ((direction < 0 && dy >= 0) || (direction > 0 && dy <= 0)) continue;
    const int absDy = std::abs(dy);
    const auto& block = line.getBlock();
    if (!block || ref.wordIndex >= block->getWordXPositions().size()) continue;
    const int dx = std::abs((line.xPos + block->getWordXPositions()[ref.wordIndex]) - wantedX);
    if (absDy < bestDy || (absDy == bestDy && dx < bestDx)) {
      bestDy = absDy;
      bestDx = dx;
      best = i;
    }
  }

  if (best != cursor_) {
    cursor_ = best;
    requestUpdate();
  }
}

void ClippingSelectionActivity::orderedRange(int& startPage, size_t& startWord,
                                             int& endPage, size_t& endWord) const {
  const bool forward = currentPage_ > anchorPage_ ||
                       (currentPage_ == anchorPage_ && cursor_ >= anchorWord_);
  if (forward) {
    startPage = anchorPage_;
    startWord = anchorWord_;
    endPage = currentPage_;
    endWord = cursor_;
  } else {
    startPage = currentPage_;
    startWord = cursor_;
    endPage = anchorPage_;
    endWord = anchorWord_;
  }
}

void ClippingSelectionActivity::appendPageText(std::string& out, const int pageNumber,
                                               const size_t startWord, const size_t endWord) {
  const int previousSectionPage = section_.currentPage;
  section_.currentPage = pageNumber;
  auto page = section_.loadPageFromSectionFile();
  section_.currentPage = previousSectionPage;
  if (!page) return;

  const auto words = ClippingUtils::collectWords(*page);
  if (words.empty()) return;
  const size_t safeStart = std::min(startWord, words.size() - 1);
  const size_t safeEnd = std::min(endWord, words.size() - 1);
  char fragment[513] = {};
  if (!ClippingUtils::extractText(*page, words, safeStart, safeEnd, fragment, sizeof(fragment))) return;

  if (!out.empty() && out.size() < 512) out.push_back('\n');
  const size_t remaining = 512 - std::min<size_t>(out.size(), 512);
  if (remaining > 0) out.append(fragment, std::min(strlen(fragment), remaining));
}

void ClippingSelectionActivity::saveSelection() {
  if (!page_ || words_.empty()) return;

  int startPage = 0;
  int endPage = 0;
  size_t startWord = 0;
  size_t endWord = 0;
  orderedRange(startPage, startWord, endPage, endWord);

  ClippingSelectionResult result;
  result.startPageNumber = static_cast<uint16_t>(startPage);
  result.endPageNumber = static_cast<uint16_t>(endPage);
  result.startWordIndex = static_cast<uint16_t>(std::min<size_t>(startWord, UINT16_MAX));
  result.endWordIndex = static_cast<uint16_t>(std::min<size_t>(endWord, UINT16_MAX));

  std::string text;
  text.reserve(512);
  for (int pageNumber = startPage; pageNumber <= endPage && text.size() < 512; ++pageNumber) {
    const int previousSectionPage = section_.currentPage;
    section_.currentPage = pageNumber;
    auto page = section_.loadPageFromSectionFile();
    section_.currentPage = previousSectionPage;
    if (!page) continue;
    const auto pageWords = ClippingUtils::collectWords(*page);
    if (pageWords.empty()) continue;

    const size_t first = pageNumber == startPage ? startWord : 0;
    const size_t last = pageNumber == endPage ? endWord : pageWords.size() - 1;
    char fragment[513] = {};
    if (ClippingUtils::extractText(*page, pageWords, std::min(first, pageWords.size() - 1),
                                   std::min(last, pageWords.size() - 1), fragment, sizeof(fragment))) {
      if (!text.empty() && text.size() < 512) text.push_back('\n');
      const size_t remaining = 512 - std::min<size_t>(text.size(), 512);
      if (remaining > 0) text.append(fragment, std::min(strlen(fragment), remaining));
    }
  }

  if (text.empty()) return;
  result.text = std::move(text);
  setResult(std::move(result));
  finish();
}

void ClippingSelectionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    if (words_.empty()) return;
    if (!selecting_) {
      selecting_ = true;
      anchorPage_ = currentPage_;
      anchorWord_ = cursor_;
      requestUpdate();
    } else {
      saveSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (mappedInput.getHeldTime() >= PAGE_JUMP_HOLD_MS) jumpPage(-1);
    else moveHorizontal(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (mappedInput.getHeldTime() >= PAGE_JUMP_HOLD_MS) jumpPage(1);
    else moveHorizontal(1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) moveVertical(-1);
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) moveVertical(1);
}

void ClippingSelectionActivity::render(RenderLock&&) {
  // IMPORTANT: a Page loaded from the section cache only contains layout/text
  // references. For an SD-card font the glyph bitmaps may have been evicted
  // after leaving the reader menu. Rendering immediately in that state makes
  // every uncached Cyrillic/Unicode glyph appear as the replacement diamond.
  //
  // Use the same two-pass prewarm path as DictionaryActivity:
  //   1) scan the visible page text with the *same* active reader font id;
  //   2) prewarm those glyphs;
  //   3) render the real page and selection overlay.
  auto drawSelectionScreen = [this]() {
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());

    if (page_) {
      page_->render(renderer, fontId_, marginLeft_, marginTop_, ReaderUtils::readerForegroundBlack());
    }

    if (page_ && !words_.empty()) {
      if (selecting_) {
        int startPage = 0;
        int endPage = 0;
        size_t startWord = 0;
        size_t endWord = 0;
        orderedRange(startPage, startWord, endPage, endWord);

        if (currentPage_ >= startPage && currentPage_ <= endPage) {
          size_t first = 0;
          size_t last = words_.size() - 1;
          if (currentPage_ == startPage) first = std::min(startWord, words_.size() - 1);
          if (currentPage_ == endPage) last = std::min(endWord, words_.size() - 1);
          if (first > last) std::swap(first, last);

          for (size_t i = first; i <= last; ++i) {
            ClippingUtils::drawWordHighlight(renderer, *page_, words_[i], fontId_, marginLeft_, marginTop_,
                                             ReaderUtils::readerForegroundBlack());
          }
        }
      } else {
        ClippingUtils::drawWordHighlight(renderer, *page_, words_[cursor_], fontId_, marginLeft_, marginTop_,
                                         ReaderUtils::readerForegroundBlack(), true);
      }
    }

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int hintsTop = renderer.getScreenHeight() - metrics.buttonHintsHeight;

    // Reserve a solid reader-colour band so book text never collides with the
    // selection controls.
    renderer.fillRect(0, hintsTop, renderer.getScreenWidth(), metrics.buttonHintsHeight,
                      ReaderUtils::readerDarkModeEnabled());

    char pageLabel[48];
    snprintf(pageLabel, sizeof(pageLabel), "%d/%u  удерж. ←/→ = страница",
             currentPage_ + 1, static_cast<unsigned>(section_.pageCount));
    const int labelY = std::max(0, hintsTop - renderer.getLineHeight(SMALL_FONT_ID) - 4);
    renderer.fillRect(0, labelY - 2, renderer.getScreenWidth(), renderer.getLineHeight(SMALL_FONT_ID) + 4,
                      ReaderUtils::readerDarkModeEnabled());
    renderer.drawCenteredText(SMALL_FONT_ID, labelY, pageLabel, ReaderUtils::readerForegroundBlack());

    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), selecting_ ? tr(STR_CLIPPING_DONE) : tr(STR_CLIPPING_START),
                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  };

  if (page_) {
    if (auto* fontCache = renderer.getFontCacheManager()) {
      auto prewarmScope = fontCache->createPrewarmScope();

      // Scan every text glyph used by this rendered page with the exact font
      // selected by the parent reader. No fallback font and no new font
      // context are introduced by clipping mode.
      page_->renderText(renderer, fontId_, marginLeft_, marginTop_,
                        ReaderUtils::readerForegroundBlack());

      prewarmScope.endScanAndPrewarm();
      drawSelectionScreen();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
  }

  // Built-in fonts do not require prewarming.
  drawSelectionScreen();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

