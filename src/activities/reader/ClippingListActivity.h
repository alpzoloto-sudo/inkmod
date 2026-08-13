#pragma once

#include "ClippingStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ClippingListActivity final : public Activity {
 public:
  ClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ClippingStore& store)
      : Activity("ClippingList", renderer, mappedInput), store_(store) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  ClippingStore& store_;
  int selectedIndex_ = 0;
  bool longPressHandled_ = false;
  ButtonNavigator navigator_;

  int pageItems() const;
  void openDeleteMenu(bool ignoreInitialConfirmRelease);
};
