#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SystemDiagnosticsActivity final : public Activity {
 public:
  SystemDiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SystemDiagnostics", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
 private:
  bool storageReadySeen = false;
  int selectedAction = 0;  // 0=force clean, 1=SD update, 2=other OTA slot
  ButtonNavigator buttonNavigator;
  static constexpr int ACTION_COUNT = 3;
};
