#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual RequestUpdateResult requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }
  virtual bool allowPowerAsConfirmInReaderMode() const { return false; }
  virtual bool canSnapshotForSleepOverlay() const { return false; }
  virtual std::string getCurrentBookPath() const { return {}; }
  // Called just before another activity is pushed on top of this one (see
  // ActivityManager::pushActivity) — unlike replaceActivity, a pushed
  // activity keeps this one fully alive in the background stack so it can
  // be returned to, so nothing about it is freed automatically. Activities
  // holding large heap-resident state (e.g. the reader's decoded chapter
  // Section) should override this to release what it can safely regenerate
  // on return, so the activity being pushed doesn't have to compete with a
  // full duplicate of this activity's own working set for RAM.
  virtual void releaseHeavyResourcesForBackgroundActivity() {}
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);
};
