#pragma once

#include <Epub/Page.h>
#include <Epub/Section.h>

#include <memory>
#include <vector>

#include "ClippingUtils.h"
#include "activities/Activity.h"

class ClippingSelectionActivity final : public Activity {
 public:
  ClippingSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Section& section,
                            int fontId, int marginLeft, int marginTop);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  Section& section_;
  std::unique_ptr<Page> page_;
  std::vector<ClippingUtils::WordRef> words_;
  int fontId_ = 0;
  int marginLeft_ = 0;
  int marginTop_ = 0;

  int originalPage_ = 0;
  int currentPage_ = 0;
  int anchorPage_ = 0;
  size_t cursor_ = 0;
  size_t anchorWord_ = 0;
  bool selecting_ = false;

  static constexpr unsigned long PAGE_JUMP_HOLD_MS = 550;

  bool loadPage(int pageNumber, bool selectLastWord = false);
  void moveHorizontal(int delta);
  void moveVertical(int direction);
  void jumpPage(int direction);
  void saveSelection();
  size_t nearestCenterWord() const;
  void orderedRange(int& startPage, size_t& startWord, int& endPage, size_t& endWord) const;
  void appendPageText(std::string& out, int pageNumber, size_t startWord, size_t endWord);
};
