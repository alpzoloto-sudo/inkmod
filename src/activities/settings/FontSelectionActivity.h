#pragma once

#include <SdCardFontRegistry.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FontSelectionActivity final : public Activity {
 public:
  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const SdCardFontRegistry* registry);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;  // index used by valueSetter
  };

  // CrossPoint-style preview: temporarily apply the highlighted family to
  // SETTINGS and load it through the exact same SdCardFontSystem path used by
  // the reader. Back restores the saved settings; Select commits them.
  void updatePreviewFont();
  void restoreSavedFontSettings();

  const SdCardFontRegistry* registry_;
  ButtonNavigator buttonNavigator_;
  std::vector<FontEntry> fonts_;
  int selectedIndex_ = 0;

  int previewedIndex_ = -1;
  int previewFontId_ = 0;

  // Snapshot of the real reader setting while the picker temporarily mutates
  // SETTINGS for live preview (the same design used by CrossPoint #2349).
  uint8_t savedFontFamily_ = 0;
  uint8_t savedFontSize_ = 0;
  std::string savedSdFontFamilyName_;
  uint8_t previewTargetPointSize_ = 12;
  bool selectionCommitted_ = false;
};
