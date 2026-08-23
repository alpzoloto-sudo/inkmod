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

  // Returns the font ID to preview the currently highlighted entry with,
  // loading it from the SD card first if needed. Cheap to call every
  // render(): only actually touches the SD card / font manager when
  // selectedIndex_ changed since the last call.
  int previewFontId();

  const SdCardFontRegistry* registry_;
  ButtonNavigator buttonNavigator_;
  std::vector<FontEntry> fonts_;
  int selectedIndex_ = 0;

  // Preview font cache (see previewFontId()) so scrolling through the list
  // doesn't reload the SD card font on every single render() call.
  int previewedIndex_ = -1;
  int previewFontId_ = 0;
};
