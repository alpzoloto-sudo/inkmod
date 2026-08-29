#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;       // true = word attaches to previous (no space before it)
  std::vector<bool> wordIsBionicSuffix;  // true = token is the regular tail of a bionic bold-prefix split
  std::vector<bool> wordIsGuideDot;      // true = token is a guide dot (U+00B7) inserted between words
  std::vector<uint8_t> wordBackgroundBlack;
  bool extraParagraphSpacing;
  bool forceParagraphIndents;
  bool hyphenationEnabled;
  bool bionicReadingEnabled;
  bool guideReadingEnabled;
  bool preserveNaturalWordSpaces;  // FB2: never compress justified spaces below their natural width
  BlockStyle blockStyle;
  bool hasRtlWord;
  // Once a paragraph has been drained in RAM windows, keep using a local/greedy
  // line breaker for the rest of that *same* paragraph. Unlike the legacy DP
  // breaker, greedy breaks are prefix-stable: a line already known to be full
  // can never move just because more words arrive in the next RAM chunk.
  // This makes internal streaming boundaries completely invisible to pagination.
  bool streamingParagraphMode = false;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedBionicSuffixScratch;
  std::vector<bool> reorderedGuideDotScratch;
  std::vector<uint8_t> reorderedBackgroundBlackScratch;
  std::vector<uint16_t> visualOrderScratch;

  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        bool paragraphContinuation = false);
  std::vector<size_t> computeGreedyLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                             std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                             bool paragraphContinuation = false);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  bool paragraphContinuation = false);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  bool splitPathologicalTokenAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                                     std::vector<uint16_t>& wordWidths);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId, bool paragraphContinuation = false);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool forceParagraphIndents = false,
                      const bool hyphenationEnabled = false, const bool bionicReadingEnabled = false,
                      const bool guideReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle(),
                      const bool preserveNaturalWordSpaces = false)
      : extraParagraphSpacing(extraParagraphSpacing),
        forceParagraphIndents(forceParagraphIndents),
        hyphenationEnabled(hyphenationEnabled),
        bionicReadingEnabled(bionicReadingEnabled),
        guideReadingEnabled(guideReadingEnabled),
        preserveNaturalWordSpaces(preserveNaturalWordSpaces),
        blockStyle(blockStyle),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               bool backgroundBlack = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void enableStreamingParagraphMode() { streamingParagraphMode = true; }
  bool isStreamingParagraphMode() const { return streamingParagraphMode; }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, size_t maxLines = SIZE_MAX,
                             size_t trailingLinesToKeep = 1, bool paragraphContinuation = false);
};
