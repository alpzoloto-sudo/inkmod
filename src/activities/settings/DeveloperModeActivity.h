#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DeveloperModeActivity final : public Activity {
 public:
  DeveloperModeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DeveloperMode", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
 private:
  int selected = 0;
  ButtonNavigator buttonNavigator;
  static constexpr int ITEM_COUNT = 11;
  void activate();
};
