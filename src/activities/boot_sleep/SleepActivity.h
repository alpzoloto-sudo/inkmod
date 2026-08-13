#pragma once
#include <string>
#include <utility>

#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool canSnapshotOverlayBackground,
                         std::string currentBookPath = {}, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput),
        canSnapshotOverlayBackground(canSnapshotOverlayBackground),
        currentBookPath(std::move(currentBookPath)),
        fromTimeout(fromTimeout) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderReadingStatsSleepScreen() const;
  void renderMinimalSleepScreen() const;
  void renderMinimalStatsSleepScreen() const;
  void renderDashboardSleepScreen() const;
  void renderCalendarSleepScreen() const;
  void renderCalendarSleepScreenLandscape() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool blackBackground = false) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen() const;
  uint8_t effectiveSleepScreenMode() const;
  const std::string& effectivePinnedSleepImagePath() const;
  bool canSnapshotOverlayBackground = false;
  bool overlayBackgroundBufferStored = false;
  std::string currentBookPath;
  bool fromTimeout = false;
};
