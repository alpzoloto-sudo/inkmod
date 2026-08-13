#pragma once

#include "activities/Activity.h"

class SupportInkMODActivity final : public Activity {
 public:
  SupportInkMODActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SupportInkMOD", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
