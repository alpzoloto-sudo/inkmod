#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ClippingStore.h"

namespace ClippingUtils {

struct WordRef {
  int elementIndex = -1;
  size_t wordIndex = 0;
};

inline std::vector<WordRef> collectWords(const Page& page) {
  std::vector<WordRef> words;
  words.reserve(96);
  for (size_t elementIndex = 0; elementIndex < page.elements.size(); ++elementIndex) {
    const auto& element = page.elements[elementIndex];
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block) continue;
    for (size_t wordIndex = 0; wordIndex < block->getWords().size(); ++wordIndex) {
      if (!block->getWords()[wordIndex].empty()) words.push_back({static_cast<int>(elementIndex), wordIndex});
    }
  }
  return words;
}

inline bool extractText(const Page& page, const std::vector<WordRef>& words, size_t start, size_t end,
                        char* out, size_t outSize) {
  if (!out || outSize == 0 || words.empty()) return false;
  if (start > end) std::swap(start, end);
  if (end >= words.size()) return false;
  out[0] = '\0';
  size_t used = 0;

  int previousElement = -1;
  for (size_t i = start; i <= end; ++i) {
    const auto& ref = words[i];
    if (ref.elementIndex < 0 || ref.elementIndex >= static_cast<int>(page.elements.size())) continue;
    const auto& line = static_cast<const PageLine&>(*page.elements[ref.elementIndex]);
    const auto& block = line.getBlock();
    if (!block || ref.wordIndex >= block->getWords().size()) continue;
    const std::string& word = block->getWords()[ref.wordIndex];

    const char separator = (used > 0 && previousElement != ref.elementIndex) ? '\n' : ' ';
    if (used > 0) {
      if (used + 1 >= outSize) break;
      out[used++] = separator;
    }
    size_t copy = std::min(word.size(), outSize - used - 1);
    while (copy > 0 && copy < word.size() &&
           (static_cast<uint8_t>(word[copy]) & 0xC0) == 0x80) {
      --copy;
    }
    if (copy == 0) break;
    memcpy(out + used, word.data(), copy);
    used += copy;
    out[used] = '\0';
    previousElement = ref.elementIndex;
    if (used + 1 >= outSize) break;
  }
  return used > 0;
}

inline void drawWordHighlight(GfxRenderer& renderer, const Page& page, const WordRef& ref, int fontId,
                              int marginLeft, int marginTop, bool foregroundBlack, bool cursorOnly = false) {
  if (ref.elementIndex < 0 || ref.elementIndex >= static_cast<int>(page.elements.size())) return;
  const auto& element = page.elements[ref.elementIndex];
  if (!element || element->getTag() != TAG_PageLine) return;
  const auto& line = static_cast<const PageLine&>(*element);
  const auto& block = line.getBlock();
  if (!block || ref.wordIndex >= block->getWords().size() ||
      ref.wordIndex >= block->getWordXPositions().size() ||
      ref.wordIndex >= block->getWordStyles().size()) return;

  const auto& word = block->getWords()[ref.wordIndex];
  const auto style = block->getWordStyles()[ref.wordIndex];
  const int x = marginLeft + line.xPos + block->getWordXPositions()[ref.wordIndex];
  const int y = marginTop + line.yPos;
  const int width = std::max(4, renderer.getTextAdvanceX(fontId, word.c_str(), style));
  const int height = std::max(4, renderer.getLineHeight(fontId));

  if (cursorOnly) {
    renderer.drawRect(x - 2, y - 2, width + 4, height + 4, 1, foregroundBlack);
    return;
  }
  renderer.fillRect(x - 2, y - 2, width + 4, height + 4, foregroundBlack);
  renderer.drawText(fontId, x, y, word.c_str(), !foregroundBlack, style);
}

inline void drawSavedHighlights(GfxRenderer& renderer, const Page& page, const std::vector<Clipping>& clippings,
                                uint16_t spineIndex, uint16_t pageNumber, int fontId, int marginLeft, int marginTop,
                                bool foregroundBlack) {
  if (clippings.empty()) return;
  const auto words = collectWords(page);
  if (words.empty()) return;

  for (const auto& clipping : clippings) {
    if (clipping.spineIndex != spineIndex) continue;
    const uint16_t firstPage = std::min(clipping.pageNumber, clipping.endPageNumber);
    const uint16_t lastPage = std::max(clipping.pageNumber, clipping.endPageNumber);
    if (pageNumber < firstPage || pageNumber > lastPage) continue;

    size_t startWord = 0;
    size_t endWord = words.size() - 1;

    if (pageNumber == clipping.pageNumber) {
      startWord = std::min<size_t>(clipping.startWordIndex, words.size() - 1);
    }
    if (pageNumber == clipping.endPageNumber) {
      endWord = std::min<size_t>(clipping.endWordIndex, words.size() - 1);
    }

    // Defensive handling for a future reverse-selection record.
    if (firstPage == lastPage && startWord > endWord) std::swap(startWord, endWord);

    for (size_t i = startWord; i <= endWord; ++i) {
      drawWordHighlight(renderer, page, words[i], fontId, marginLeft, marginTop, foregroundBlack);
    }
  }
}

}  // namespace ClippingUtils
