#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "activities/Activity.h"
#include "dictionary/DictionaryStore.h"

class DictionaryActivity final : public Activity {
 public:
  DictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page, int fontId,
                     int marginTop, int marginRight, int marginBottom, int marginLeft)
      : Activity("Dictionary", renderer, mappedInput),
        page_(std::move(page)),
        fontId_(fontId),
        marginTop_(marginTop),
        marginRight_(marginRight),
        marginBottom_(marginBottom),
        marginLeft_(marginLeft) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  enum class Mode : uint8_t { SelectWord, Article, NoDictionaries, NoWords };

  static constexpr size_t MAX_ARTICLE_BYTES = 8192;
  static constexpr size_t MAX_HEADWORD_BYTES = 96;
  static constexpr size_t LINE_BUFFER_BYTES = 384;
  static constexpr uint8_t MAX_ARTICLE_PAGES = 64;
  static constexpr uint8_t MAX_SELECTION_FRAGMENTS = 4;

  struct WordLocation {
    int elementIndex = -1;
    size_t wordIndex = 0;
  };

  struct SelectionFragment {
    WordLocation location;
    size_t startByte = 0;
    size_t endByte = 0;
    bool trailingHyphen = false;
  };

  struct SelectionSpan {
    SelectionFragment fragments[MAX_SELECTION_FRAGMENTS] = {};
    uint8_t count = 0;
  };

  std::unique_ptr<Page> page_;
  int fontId_ = 0;
  int marginTop_ = 0;
  int marginRight_ = 0;
  int marginBottom_ = 0;
  int marginLeft_ = 0;
  Dictionary::Store dictionaries_;
  Mode mode_ = Mode::SelectWord;
  int selectedElement_ = -1;
  size_t selectedWord_ = 0;
  uint8_t activeDictionary_ = 0;
  char selectedLookupWord_[MAX_HEADWORD_BYTES] = {};
  char alternateLookupWord_[MAX_HEADWORD_BYTES] = {};
  char matchedWord_[MAX_HEADWORD_BYTES] = {};
  char article_[MAX_ARTICLE_BYTES + 1] = {};
  char lineBuffer_[LINE_BUFFER_BYTES] = {};
  char selectionPrefixBuffer_[MAX_HEADWORD_BYTES] = {};
  char selectionTextBuffer_[MAX_HEADWORD_BYTES] = {};
  uint16_t articleLength_ = 0;
  uint16_t articlePageOffsets_[MAX_ARTICLE_PAGES + 1] = {};
  uint8_t articlePageCount_ = 1;
  uint8_t currentArticlePage_ = 0;

  bool selectFirstWord();
  bool isSelectableWord(int elementIndex, size_t wordIndex) const;
  bool getSelectionFragment(const WordLocation& location, SelectionFragment& fragment) const;
  bool resolveSelectionSpan(SelectionSpan& span) const;
  bool buildSelectedLookupWords();
  bool firstSelectableWord(int elementIndex, WordLocation& location) const;
  bool lastSelectableWord(int elementIndex, WordLocation& location) const;
  int adjacentLineElement(int elementIndex, int direction) const;
  bool linesAreVisuallyAdjacent(int firstElement, int secondElement) const;
  void moveHorizontal(int direction);
  void moveVertical(int direction);
  void lookupSelectedWord();
  bool loadDictionaryArticle(uint8_t dictionaryIndex);
  void changeDictionary(int direction);

  int articleFontId() const;
  void getPopupLayout(int& x, int& y, int& width, int& height, int& contentWidth, int& maxLines) const;
  size_t buildArticleLine(size_t offset, int maxWidth, bool& hasLine);
  void paginateArticle();
  void drawCurrentMode();
  void drawSelectionPage(bool showHints);
  void drawArticlePopup();
  void drawEmptyState(StrId title, StrId body);
};
