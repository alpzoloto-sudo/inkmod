#pragma once

#include "activities/Activity.h"

class SystemDiagnosticsActivity final : public Activity {
 public:
  SystemDiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SystemDiagnostics", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  bool storageReadySeen = false;
};
