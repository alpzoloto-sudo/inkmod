#pragma once

#include "activities/Activity.h"

class LegacyRenderPromptActivity final : public Activity {
 public:
  LegacyRenderPromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LegacyRenderPrompt", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  // The secret sequence ends with Menu/Confirm. Ignore that stale release so
  // the prompt cannot auto-answer before the user actually sees it.
  bool ignoreInitialConfirmRelease_ = true;
};
