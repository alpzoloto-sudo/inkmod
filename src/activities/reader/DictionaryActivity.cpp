#include "DictionaryActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

bool isAsciiSpace(const char c) { return c == ' ' || c == '\t' || c == '\r'; }

bool decodeUtf8Codepoint(const std::string& text, size_t& offset, uint32_t& codepoint) {
  if (offset >= text.size()) return false;
  const size_t start = offset;
  const uint8_t first = static_cast<uint8_t>(text[offset++]);
  if (first < 0x80) {
    codepoint = first;
    return true;
  }

  uint8_t continuationCount = 0;
  if ((first & 0xE0) == 0xC0) {
    codepoint = first & 0x1F;
    continuationCount = 1;
  } else if ((first & 0xF0) == 0xE0) {
    codepoint = first & 0x0F;
    continuationCount = 2;
  } else if ((first & 0xF8) == 0xF0) {
    codepoint = first & 0x07;
    continuationCount = 3;
  } else {
    codepoint = 0xFFFD;
    return true;
  }

  if (offset + continuationCount > text.size()) {
    offset = start + 1;
    codepoint = 0xFFFD;
    return true;
  }
  for (uint8_t i = 0; i < continuationCount; ++i) {
    const uint8_t next = static_cast<uint8_t>(text[offset]);
    if ((next & 0xC0) != 0x80) {
      offset = start + 1;
      codepoint = 0xFFFD;
      return true;
    }
    ++offset;
    codepoint = (codepoint << 6) | (next & 0x3F);
  }
  return true;
}

bool isDictionaryLetter(const uint32_t codepoint) {
  return (codepoint >= '0' && codepoint <= '9') || (codepoint >= 'A' && codepoint <= 'Z') ||
         (codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 0x00C0 && codepoint <= 0x02AF) ||
         (codepoint >= 0x0400 && codepoint <= 0x052F);
}

bool isHyphen(const uint32_t codepoint) {
  return codepoint == '-' || codepoint == 0x058A || codepoint == 0x2010 || codepoint == 0x2011 ||
         codepoint == 0x2E17 || codepoint == 0xFE63 || codepoint == 0xFF0D;
}

bool copyUtf8Range(const std::string& source, const size_t start, const size_t end, char* output,
                   const size_t outputSize) {
  if (!output || outputSize == 0 || start > end || end > source.size()) return false;
  size_t copyLength = std::min(end - start, outputSize - 1);
  if (start + copyLength < end) {
    while (copyLength > 0 && (static_cast<uint8_t>(source[start + copyLength]) & 0xC0) == 0x80) --copyLength;
  }
  memcpy(output, source.data() + start, copyLength);
  output[copyLength] = '\0';
  return copyLength > 0;
}

bool appendUtf8Range(char* output, const size_t outputSize, const std::string& source, const size_t start,
                     const size_t end) {
  if (!output || outputSize == 0 || start > end || end > source.size()) return false;
  const size_t used = strlen(output);
  if (used >= outputSize - 1) return false;
  size_t copyLength = std::min(end - start, outputSize - used - 1);
  if (start + copyLength < end) {
    while (copyLength > 0 && (static_cast<uint8_t>(source[start + copyLength]) & 0xC0) == 0x80) --copyLength;
  }
  memcpy(output + used, source.data() + start, copyLength);
  output[used + copyLength] = '\0';
  return copyLength == end - start;
}

void removeLastUtf8Codepoint(char* text, size_t& length) {
  if (length == 0) return;
  --length;
  while (length > 0 && (static_cast<uint8_t>(text[length]) & 0xC0) == 0x80) --length;
  text[length] = '\0';
}

}  // namespace

void DictionaryActivity::onEnter() {
  Activity::onEnter();
  dictionaries_.scan();
  if (!page_ || !selectFirstWord()) {
    mode_ = Mode::NoWords;
  } else if (dictionaries_.count() == 0) {
    mode_ = Mode::NoDictionaries;
  }
  requestUpdate();
}

bool DictionaryActivity::isSelectableWord(const int elementIndex, const size_t wordIndex) const {
  if (!page_ || elementIndex < 0 || elementIndex >= static_cast<int>(page_->elements.size())) return false;
  const auto& element = page_->elements[elementIndex];
  if (!element || element->getTag() != TAG_PageLine) return false;
  const auto& line = static_cast<const PageLine&>(*element);
  const auto& block = line.getBlock();
  if (!block || wordIndex >= block->getWords().size()) return false;
  char normalized[128];
  return Dictionary::Store::normalizeWord(block->getWords()[wordIndex].c_str(), normalized, sizeof(normalized)) > 0;
}

bool DictionaryActivity::getSelectionFragment(const WordLocation& location, SelectionFragment& fragment) const {
  if (!isSelectableWord(location.elementIndex, location.wordIndex)) return false;
  const auto& line = static_cast<const PageLine&>(*page_->elements[location.elementIndex]);
  const auto& word = line.getBlock()->getWords()[location.wordIndex];

  size_t offset = 0;
  size_t firstLetter = word.size();
  size_t lastLetterEnd = 0;
  while (offset < word.size()) {
    const size_t codepointStart = offset;
    uint32_t codepoint = 0;
    if (!decodeUtf8Codepoint(word, offset, codepoint)) break;
    if (isDictionaryLetter(codepoint)) {
      if (firstLetter == word.size()) firstLetter = codepointStart;
      lastLetterEnd = offset;
    }
  }
  if (firstLetter == word.size() || lastLetterEnd <= firstLetter) return false;

  bool trailingHyphen = false;
  offset = lastLetterEnd;
  while (offset < word.size()) {
    uint32_t codepoint = 0;
    if (!decodeUtf8Codepoint(word, offset, codepoint)) break;
    if (isHyphen(codepoint)) {
      trailingHyphen = true;
      break;
    }
  }

  fragment.location = location;
  fragment.startByte = firstLetter;
  fragment.endByte = lastLetterEnd;
  fragment.trailingHyphen = trailingHyphen;
  return true;
}

bool DictionaryActivity::firstSelectableWord(const int elementIndex, WordLocation& location) const {
  if (!page_ || elementIndex < 0 || elementIndex >= static_cast<int>(page_->elements.size())) return false;
  const auto& element = page_->elements[elementIndex];
  if (!element || element->getTag() != TAG_PageLine) return false;
  const auto& line = static_cast<const PageLine&>(*element);
  if (!line.getBlock()) return false;
  for (size_t wordIndex = 0; wordIndex < line.getBlock()->getWords().size(); ++wordIndex) {
    if (isSelectableWord(elementIndex, wordIndex)) {
      location = {elementIndex, wordIndex};
      return true;
    }
  }
  return false;
}

bool DictionaryActivity::lastSelectableWord(const int elementIndex, WordLocation& location) const {
  if (!page_ || elementIndex < 0 || elementIndex >= static_cast<int>(page_->elements.size())) return false;
  const auto& element = page_->elements[elementIndex];
  if (!element || element->getTag() != TAG_PageLine) return false;
  const auto& line = static_cast<const PageLine&>(*element);
  if (!line.getBlock()) return false;
  const auto wordCount = line.getBlock()->getWords().size();
  for (size_t wordIndex = wordCount; wordIndex > 0; --wordIndex) {
    if (isSelectableWord(elementIndex, wordIndex - 1)) {
      location = {elementIndex, wordIndex - 1};
      return true;
    }
  }
  return false;
}

int DictionaryActivity::adjacentLineElement(const int elementIndex, const int direction) const {
  if (!page_ || direction == 0) return -1;
  for (int index = elementIndex + direction; index >= 0 && index < static_cast<int>(page_->elements.size());
       index += direction) {
    const auto& element = page_->elements[index];
    if (element && element->getTag() == TAG_PageLine) return index;
  }
  return -1;
}

bool DictionaryActivity::linesAreVisuallyAdjacent(const int firstElement, const int secondElement) const {
  if (!page_ || firstElement < 0 || secondElement < 0 || firstElement >= static_cast<int>(page_->elements.size()) ||
      secondElement >= static_cast<int>(page_->elements.size())) {
    return false;
  }
  const int delta = page_->elements[secondElement]->yPos - page_->elements[firstElement]->yPos;
  const int lineHeight = std::max(1, renderer.getLineHeight(fontId_));
  return delta > 0 && delta <= lineHeight + lineHeight / 2;
}

bool DictionaryActivity::resolveSelectionSpan(SelectionSpan& span) const {
  span.count = 0;
  WordLocation start{selectedElement_, selectedWord_};
  SelectionFragment currentFragment;
  if (!getSelectionFragment(start, currentFragment)) return false;

  for (uint8_t depth = 1; depth < MAX_SELECTION_FRAGMENTS; ++depth) {
    WordLocation firstOnLine;
    if (!firstSelectableWord(start.elementIndex, firstOnLine) || firstOnLine.wordIndex != start.wordIndex) break;
    const int previousLine = adjacentLineElement(start.elementIndex, -1);
    if (previousLine < 0 || !linesAreVisuallyAdjacent(previousLine, start.elementIndex)) break;
    WordLocation previous;
    SelectionFragment previousFragment;
    if (!lastSelectableWord(previousLine, previous) || !getSelectionFragment(previous, previousFragment) ||
        !previousFragment.trailingHyphen) {
      break;
    }
    start = previous;
  }

  WordLocation cursor = start;
  while (span.count < MAX_SELECTION_FRAGMENTS) {
    SelectionFragment fragment;
    if (!getSelectionFragment(cursor, fragment)) break;
    span.fragments[span.count++] = fragment;
    if (!fragment.trailingHyphen) break;

    WordLocation lastOnLine;
    if (!lastSelectableWord(cursor.elementIndex, lastOnLine) || lastOnLine.wordIndex != cursor.wordIndex) break;
    const int nextLine = adjacentLineElement(cursor.elementIndex, 1);
    if (nextLine < 0 || !linesAreVisuallyAdjacent(cursor.elementIndex, nextLine)) break;
    WordLocation next;
    if (!firstSelectableWord(nextLine, next)) break;
    cursor = next;
  }
  return span.count > 0;
}

bool DictionaryActivity::buildSelectedLookupWords() {
  selectedLookupWord_[0] = '\0';
  alternateLookupWord_[0] = '\0';
  SelectionSpan span;
  if (!resolveSelectionSpan(span)) return false;

  bool complete = true;
  for (uint8_t index = 0; index < span.count; ++index) {
    const auto& fragment = span.fragments[index];
    const auto& line = static_cast<const PageLine&>(*page_->elements[fragment.location.elementIndex]);
    const auto& word = line.getBlock()->getWords()[fragment.location.wordIndex];
    if (index > 0) {
      const size_t used = strlen(alternateLookupWord_);
      if (used + 1 >= sizeof(alternateLookupWord_)) {
        complete = false;
      } else {
        alternateLookupWord_[used] = '-';
        alternateLookupWord_[used + 1] = '\0';
      }
    }
    complete = appendUtf8Range(selectedLookupWord_, sizeof(selectedLookupWord_), word, fragment.startByte,
                               fragment.endByte) &&
               complete;
    complete = appendUtf8Range(alternateLookupWord_, sizeof(alternateLookupWord_), word, fragment.startByte,
                               fragment.endByte) &&
               complete;
  }
  if (!complete || selectedLookupWord_[0] == '\0') return false;
  if (span.count < 2 || strcmp(selectedLookupWord_, alternateLookupWord_) == 0) alternateLookupWord_[0] = '\0';
  return true;
}

bool DictionaryActivity::selectFirstWord() {
  if (!page_) return false;
  for (size_t elementIndex = 0; elementIndex < page_->elements.size(); ++elementIndex) {
    const auto& element = page_->elements[elementIndex];
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    if (!line.getBlock()) continue;
    for (size_t wordIndex = 0; wordIndex < line.getBlock()->getWords().size(); ++wordIndex) {
      if (isSelectableWord(static_cast<int>(elementIndex), wordIndex)) {
        selectedElement_ = static_cast<int>(elementIndex);
        selectedWord_ = wordIndex;
        return true;
      }
    }
  }
  return false;
}

void DictionaryActivity::moveHorizontal(const int direction) {
  if (!page_ || selectedElement_ < 0 || direction == 0) return;
  SelectionSpan span;
  const bool hasSpan = resolveSelectionSpan(span);
  const auto edge = hasSpan ? span.fragments[direction > 0 ? span.count - 1 : 0].location
                            : WordLocation{selectedElement_, selectedWord_};
  int elementIndex = edge.elementIndex;
  int wordIndex = static_cast<int>(edge.wordIndex) + direction;
  const int elementCount = static_cast<int>(page_->elements.size());

  while (elementIndex >= 0 && elementIndex < elementCount) {
    const auto& element = page_->elements[elementIndex];
    if (element && element->getTag() == TAG_PageLine) {
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = line.getBlock();
      if (block) {
        const int wordCount = static_cast<int>(block->getWords().size());
        while (wordIndex >= 0 && wordIndex < wordCount) {
          if (isSelectableWord(elementIndex, static_cast<size_t>(wordIndex))) {
            selectedElement_ = elementIndex;
            selectedWord_ = static_cast<size_t>(wordIndex);
            requestUpdate();
            return;
          }
          wordIndex += direction;
        }
      }
    }
    elementIndex += direction;
    if (elementIndex < 0 || elementIndex >= elementCount) break;
    const auto& next = page_->elements[elementIndex];
    if (next && next->getTag() == TAG_PageLine) {
      const auto& nextLine = static_cast<const PageLine&>(*next);
      const auto& nextBlock = nextLine.getBlock();
      wordIndex = direction > 0 ? 0 : (nextBlock ? static_cast<int>(nextBlock->getWords().size()) - 1 : -1);
    }
  }
}

void DictionaryActivity::moveVertical(const int direction) {
  if (!page_ || selectedElement_ < 0 || direction == 0) return;
  const auto& currentLine = static_cast<const PageLine&>(*page_->elements[selectedElement_]);
  const auto& currentBlock = currentLine.getBlock();
  if (!currentBlock || selectedWord_ >= currentBlock->getWordXPositions().size()) return;
  const int wantedX = currentLine.xPos + currentBlock->getWordXPositions()[selectedWord_];

  int targetElement = selectedElement_ + direction;
  while (targetElement >= 0 && targetElement < static_cast<int>(page_->elements.size())) {
    const auto& element = page_->elements[targetElement];
    if (element && element->getTag() == TAG_PageLine) {
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = line.getBlock();
      if (block) {
        int bestDistance = INT_MAX;
        size_t bestWord = 0;
        bool found = false;
        for (size_t wordIndex = 0; wordIndex < block->getWords().size(); ++wordIndex) {
          if (!isSelectableWord(targetElement, wordIndex) || wordIndex >= block->getWordXPositions().size()) continue;
          const int x = line.xPos + block->getWordXPositions()[wordIndex];
          const int distance = std::abs(x - wantedX);
          if (!found || distance < bestDistance) {
            found = true;
            bestDistance = distance;
            bestWord = wordIndex;
          }
        }
        if (found) {
          selectedElement_ = targetElement;
          selectedWord_ = bestWord;
          requestUpdate();
          return;
        }
      }
    }
    targetElement += direction;
  }
}

bool DictionaryActivity::loadDictionaryArticle(const uint8_t dictionaryIndex) {
  if (!page_ || selectedElement_ < 0 || dictionaryIndex >= dictionaries_.count()) return false;
  const auto& selectedLine = static_cast<const PageLine&>(*page_->elements[selectedElement_]);
  const auto& selectedBlock = selectedLine.getBlock();
  if (!selectedBlock || selectedWord_ >= selectedBlock->getWords().size()) return false;

  // Always query the complete selected span first. Looking up the rendered
  // first fragment before the joined word can return a valid but unrelated
  // short entry (for example "ку-" -> "ку" or "реше-" -> "реш") even
  // though both line fragments are highlighted on screen.
  if (!buildSelectedLookupWords()) {
    snprintf(selectedLookupWord_, sizeof(selectedLookupWord_), "%s",
             selectedBlock->getWords()[selectedWord_].c_str());
    alternateLookupWord_[0] = '\0';
  }

  article_[0] = '\0';
  matchedWord_[0] = '\0';
  bool found = dictionaries_.lookup(dictionaryIndex, selectedLookupWord_, matchedWord_, sizeof(matchedWord_), article_,
                                    sizeof(article_));
  if (!found && alternateLookupWord_[0] != '\0') {
    found = dictionaries_.lookup(dictionaryIndex, alternateLookupWord_, matchedWord_, sizeof(matchedWord_), article_,
                                 sizeof(article_));
  }
  if (!found) {
    snprintf(matchedWord_, sizeof(matchedWord_), "%s", selectedLookupWord_);
    snprintf(article_, sizeof(article_), "%s", tr(STR_DICTIONARY_WORD_NOT_FOUND));
  }
  activeDictionary_ = dictionaryIndex;
  articleLength_ = static_cast<uint16_t>(std::min<size_t>(strlen(article_), MAX_ARTICLE_BYTES));
  paginateArticle();
  return found;
}

void DictionaryActivity::lookupSelectedWord() {
  if (dictionaries_.count() == 0) {
    mode_ = Mode::NoDictionaries;
    requestUpdate();
    return;
  }

  bool found = false;
  for (uint8_t offset = 0; offset < dictionaries_.count(); ++offset) {
    const uint8_t index = static_cast<uint8_t>((activeDictionary_ + offset) % dictionaries_.count());
    if (loadDictionaryArticle(index)) {
      found = true;
      break;
    }
  }
  if (!found) loadDictionaryArticle(activeDictionary_ < dictionaries_.count() ? activeDictionary_ : 0);
  mode_ = Mode::Article;
  requestUpdate();
}

void DictionaryActivity::changeDictionary(const int direction) {
  if (dictionaries_.count() < 2 || direction == 0) return;
  const int count = dictionaries_.count();
  const int next = (static_cast<int>(activeDictionary_) + direction + count) % count;
  loadDictionaryArticle(static_cast<uint8_t>(next));
  requestUpdate();
}

int DictionaryActivity::articleFontId() const {
  // Use the active reader font, not the limited built-in UI font. Besides
  // matching the book's selected size and family, SD fonts prepared with the
  // comprehensive range contain IPA characters such as U+02C8 and U+0268
  // that occur in Wiktionary pronunciation fields.
  return fontId_;
}

void DictionaryActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mode_ == Mode::Article) {
      mode_ = Mode::SelectWord;
      requestUpdate();
    } else {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  const bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
                       mappedInput.wasReleased(MappedInputManager::Button::Power);
  if (confirm) {
    if (mode_ == Mode::SelectWord) {
      lookupSelectedWord();
    } else if (mode_ == Mode::Article) {
      // Back already returns to word selection, so use the otherwise
      // redundant confirm button to expose dictionary switching on the four
      // front buttons. Side Up/Down remain available as shortcuts.
      if (dictionaries_.count() > 1) {
        changeDictionary(1);
      } else {
        mode_ = Mode::SelectWord;
        requestUpdate();
      }
    }
    return;
  }

  if (mode_ == Mode::SelectWord) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) moveHorizontal(-1);
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) moveHorizontal(1);
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) moveVertical(-1);
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) moveVertical(1);
    return;
  }

  if (mode_ == Mode::Article) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) && currentArticlePage_ > 0) {
      --currentArticlePage_;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) &&
        currentArticlePage_ + 1 < articlePageCount_) {
      ++currentArticlePage_;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) changeDictionary(-1);
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) changeDictionary(1);
  }
}

void DictionaryActivity::getPopupLayout(int& x, int& y, int& width, int& height, int& contentWidth,
                                        int& maxLines) const {
  int safeTop = 0;
  int safeRight = 0;
  int safeBottom = 0;
  int safeLeft = 0;
  renderer.getOrientedViewableTRBL(&safeTop, &safeRight, &safeBottom, &safeLeft);
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int availableWidth = std::max(160, screenWidth - safeLeft - safeRight - 20);
  const int availableHeight = std::max(220, screenHeight - safeTop - safeBottom - 50);
  width = std::min(600, availableWidth);
  height = std::min(600, availableHeight);
  x = (screenWidth - width) / 2;
  y = (screenHeight - height) / 2;
  constexpr int horizontalPadding = 14;
  constexpr int headerHeight = 42;
  constexpr int footerHeight = 62;
  constexpr int verticalPadding = 10;
  contentWidth = width - horizontalPadding * 2;
  const int contentHeight = height - headerHeight - footerHeight - verticalPadding * 2;
  maxLines = std::max(1, contentHeight / std::max(1, renderer.getLineHeight(articleFontId())));
}

size_t DictionaryActivity::buildArticleLine(size_t offset, const int maxWidth, bool& hasLine) {
  hasLine = false;
  lineBuffer_[0] = '\0';
  if (offset >= articleLength_) return offset;

  while (offset < articleLength_ && isAsciiSpace(article_[offset])) ++offset;
  if (offset < articleLength_ && article_[offset] == '\n') {
    hasLine = true;
    return offset + 1;
  }

  size_t lineLength = 0;
  while (offset < articleLength_) {
    if (article_[offset] == '\n') {
      ++offset;
      break;
    }
    while (offset < articleLength_ && isAsciiSpace(article_[offset])) ++offset;
    if (offset >= articleLength_ || article_[offset] == '\n') continue;

    const size_t wordStart = offset;
    while (offset < articleLength_ && !isAsciiSpace(article_[offset]) && article_[offset] != '\n') ++offset;
    const size_t wordLength = offset - wordStart;
    if (wordLength == 0) continue;

    const size_t separator = lineLength > 0 ? 1 : 0;
    const size_t copyLength = std::min(wordLength, sizeof(lineBuffer_) - lineLength - separator - 1);
    const size_t previousLength = lineLength;
    if (separator) lineBuffer_[lineLength++] = ' ';
    memcpy(lineBuffer_ + lineLength, article_ + wordStart, copyLength);
    lineLength += copyLength;
    lineBuffer_[lineLength] = '\0';

    if (renderer.getTextWidth(articleFontId(), lineBuffer_) <= maxWidth) {
      hasLine = true;
      continue;
    }

    if (previousLength > 0) {
      lineLength = previousLength;
      lineBuffer_[lineLength] = '\0';
      hasLine = true;
      return wordStart;
    }

    while (lineLength > 0 && renderer.getTextWidth(articleFontId(), lineBuffer_) > maxWidth) {
      removeLastUtf8Codepoint(lineBuffer_, lineLength);
    }
    hasLine = true;
    return offset;
  }
  return offset;
}

void DictionaryActivity::paginateArticle() {
  int popupX = 0;
  int popupY = 0;
  int popupWidth = 0;
  int popupHeight = 0;
  int contentWidth = 0;
  int maxLines = 0;
  getPopupLayout(popupX, popupY, popupWidth, popupHeight, contentWidth, maxLines);

  articlePageOffsets_[0] = 0;
  articlePageCount_ = 1;
  currentArticlePage_ = 0;
  size_t offset = 0;
  while (offset < articleLength_ && articlePageCount_ < MAX_ARTICLE_PAGES) {
    const size_t pageStart = offset;
    for (int line = 0; line < maxLines && offset < articleLength_; ++line) {
      bool hasLine = false;
      const size_t next = buildArticleLine(offset, contentWidth, hasLine);
      if (next <= offset) break;
      offset = next;
    }
    if (offset <= pageStart) break;
    if (offset < articleLength_) articlePageOffsets_[articlePageCount_++] = static_cast<uint16_t>(offset);
  }
  articlePageOffsets_[articlePageCount_] = articleLength_;
}

void DictionaryActivity::drawSelectionPage(const bool showHints) {
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  if (page_) {
    page_->render(renderer, fontId_, marginLeft_, marginTop_, ReaderUtils::readerForegroundBlack());
  }

  SelectionSpan span;
  if (page_ && resolveSelectionSpan(span)) {
    for (uint8_t index = 0; index < span.count; ++index) {
      const auto& fragment = span.fragments[index];
      const auto& line = static_cast<const PageLine&>(*page_->elements[fragment.location.elementIndex]);
      const auto& block = line.getBlock();
      if (!block || fragment.location.wordIndex >= block->getWords().size() ||
          fragment.location.wordIndex >= block->getWordXPositions().size() ||
          fragment.location.wordIndex >= block->getWordStyles().size()) {
        continue;
      }
      const auto& word = block->getWords()[fragment.location.wordIndex];
      const auto style = block->getWordStyles()[fragment.location.wordIndex];
      selectionPrefixBuffer_[0] = '\0';
      selectionTextBuffer_[0] = '\0';
      if (fragment.startByte > 0) {
        copyUtf8Range(word, 0, fragment.startByte, selectionPrefixBuffer_, sizeof(selectionPrefixBuffer_));
      }
      if (!copyUtf8Range(word, fragment.startByte, fragment.endByte, selectionTextBuffer_,
                         sizeof(selectionTextBuffer_))) {
        continue;
      }
      const int prefixWidth = selectionPrefixBuffer_[0] != '\0'
                                  ? renderer.getTextAdvanceX(fontId_, selectionPrefixBuffer_, style)
                                  : 0;
      const int x = marginLeft_ + line.xPos + block->getWordXPositions()[fragment.location.wordIndex] + prefixWidth;
      const int y = marginTop_ + line.yPos;
      const int selectedWidth = std::max(4, renderer.getTextAdvanceX(fontId_, selectionTextBuffer_, style));
      const int selectedHeight = std::max(4, renderer.getLineHeight(fontId_));
      const bool foreground = ReaderUtils::readerForegroundBlack();
      renderer.fillRect(x - 2, y - 2, selectedWidth + 4, selectedHeight + 4, foreground);
      renderer.drawText(fontId_, x, y, selectionTextBuffer_, !foreground, style);
    }
  }

  if (showHints) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  }
}

void DictionaryActivity::drawArticlePopup() {
  drawSelectionPage(false);

  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int contentWidth = 0;
  int maxLines = 0;
  getPopupLayout(x, y, width, height, contentWidth, maxLines);
  constexpr int padding = 14;
  constexpr int headerHeight = 42;
  constexpr int footerHeight = 62;
  const int bodyFontId = articleFontId();
  const int lineHeight = std::max(1, renderer.getLineHeight(bodyFontId));

  renderer.fillRect(x, y, width, height, false);
  renderer.drawRect(x, y, width, height, 2, true);

  const auto* dictionary = dictionaries_.entry(activeDictionary_);
  char title[192];
  snprintf(title, sizeof(title), "%s — %s", matchedWord_, dictionary ? dictionary->name : tr(STR_DICTIONARY));
  const std::string fittedTitle = renderer.truncatedText(UI_10_FONT_ID, title, contentWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x + padding, y + 8, fittedTitle.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawLine(x + padding, y + headerHeight, x + width - padding, y + headerHeight, true);

  size_t offset = articlePageOffsets_[currentArticlePage_];
  const size_t pageEnd = articlePageOffsets_[currentArticlePage_ + 1];
  int textY = y + headerHeight + 10;
  for (int line = 0; line < maxLines && offset < pageEnd; ++line) {
    bool hasLine = false;
    const size_t next = buildArticleLine(offset, contentWidth, hasLine);
    if (next <= offset) break;
    if (hasLine && lineBuffer_[0] != '\0') renderer.drawText(bodyFontId, x + padding, textY, lineBuffer_);
    textY += lineHeight;
    offset = next;
  }

  const int footerTop = y + height - footerHeight;
  renderer.drawLine(x + padding, footerTop, x + width - padding, footerTop, true);
  char pageLabel[20];
  snprintf(pageLabel, sizeof(pageLabel), "%u/%u", currentArticlePage_ + 1, articlePageCount_);
  constexpr int pageBandHeight = 25;
  const int pageLabelY = footerTop + std::max(0, (pageBandHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2);
  renderer.drawCenteredText(SMALL_FONT_ID, pageLabelY, pageLabel);

  const char* dictionaryLabel = dictionaries_.count() > 1 ? tr(STR_DICTIONARY) : "";
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), dictionaryLabel, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  const char* buttonLabels[4] = {labels.btn1, labels.btn2, labels.btn3, labels.btn4};
  const int buttonY = footerTop + pageBandHeight;
  const int buttonWidth = (width - padding * 2) / 4;
  const int buttonHeight = std::max(24, height - (buttonY - y) - 6);
  for (int i = 0; i < 4; ++i) {
    const int buttonX = x + padding + i * buttonWidth;
    renderer.drawRect(buttonX, buttonY, buttonWidth, buttonHeight, true);
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, buttonLabels[i], buttonWidth - 6);
    const int labelX = buttonX + (buttonWidth - renderer.getTextWidth(SMALL_FONT_ID, label.c_str())) / 2;
    renderer.drawText(SMALL_FONT_ID, labelX, buttonY + 3, label.c_str());
  }
}

void DictionaryActivity::drawEmptyState(const StrId title, const StrId body) {
  renderer.clearScreen();
  const int screenWidth = renderer.getScreenWidth();
  const int margin = 28;
  const int contentWidth = screenWidth - margin * 2;
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, I18N.get(title), contentWidth, 2, EpdFontFamily::BOLD);
  int y = std::max(30, renderer.getScreenHeight() / 4);
  for (const auto& line : titleLines) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }
  y += 20;
  const auto bodyLines = renderer.wrappedText(UI_10_FONT_ID, I18N.get(body), contentWidth, 8);
  for (const auto& line : bodyLines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void DictionaryActivity::drawCurrentMode() {
  switch (mode_) {
    case Mode::SelectWord:
      drawSelectionPage(true);
      break;
    case Mode::Article:
      drawArticlePopup();
      break;
    case Mode::NoDictionaries:
      drawEmptyState(StrId::STR_DICTIONARY, StrId::STR_DICTIONARY_NO_FILES);
      break;
    case Mode::NoWords:
      drawEmptyState(StrId::STR_DICTIONARY, StrId::STR_DICTIONARY_NO_WORDS);
      break;
  }
}

void DictionaryActivity::render(RenderLock&&) {
  // Reader pages rendered with an SD-card font need the same two-pass glyph
  // prewarm as the normal reader. Without it, entering word selection after
  // the menu redraws every uncached glyph as the replacement diamond.
  if (page_ && (mode_ == Mode::SelectWord || mode_ == Mode::Article)) {
    if (auto* fontCache = renderer.getFontCacheManager()) {
      auto prewarmScope = fontCache->createPrewarmScope();
      page_->renderText(renderer, fontId_, marginLeft_, marginTop_, ReaderUtils::readerForegroundBlack());

      // The article body uses the same SD-card font as the book. Scan only
      // the visible article page so uncommon dictionary glyphs (notably IPA)
      // are loaded before the real draw without copying the whole 8 KiB
      // article into the X4's bounded prewarm buffer.
      if (mode_ == Mode::Article) {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int contentWidth = 0;
        int maxLines = 0;
        getPopupLayout(x, y, width, height, contentWidth, maxLines);
        size_t offset = articlePageOffsets_[currentArticlePage_];
        const size_t pageEnd = articlePageOffsets_[currentArticlePage_ + 1];
        for (int line = 0; line < maxLines && offset < pageEnd; ++line) {
          bool hasLine = false;
          const size_t next = buildArticleLine(offset, contentWidth, hasLine);
          if (next <= offset) break;
          if (hasLine && lineBuffer_[0] != '\0') renderer.drawText(articleFontId(), 0, 0, lineBuffer_);
          offset = next;
        }
      }

      prewarmScope.endScanAndPrewarm();
      drawCurrentMode();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
  }

  drawCurrentMode();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
