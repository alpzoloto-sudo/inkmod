#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "util/ScreenshotInfo.h"

#ifndef portMUX_INITIALIZER_UNLOCKED
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
#endif

class Activity;    // forward declaration
class RenderLock;  // forward declaration

enum class RequestUpdateResult { Rendered, Rejected };
enum class HomeMenuItem { NONE, FILE_BROWSER, RECENTS, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU };

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = PendingAction::None;
  // See hasPendingActivityTransition() - covers the window between
  // releaseCurrentActivityHeavyResources() and the actual push/pop/replace
  // call, which pendingAction alone does not cover.
  bool backgroundTransitionPending = false;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;
  portMUX_TYPE renderStateMux = portMUX_INITIALIZER_UNLOCKED;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  bool requestedUpdate = false;

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin();
  void loop();

  // Minimum free render-task stack observed since the task started. ESP-IDF's
  // uxTaskGetStackHighWaterMark() reports bytes, so this can be compared directly
  // with the 16384-byte render stack before deciding whether it is safe to shrink.
  size_t getRenderTaskStackHighWaterMark() const {
    return renderTaskHandle ? static_cast<size_t>(uxTaskGetStackHighWaterMark(renderTaskHandle)) : 0;
  }

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer(std::string returnBookPath = {});
  void goToCalibreWireless(std::string returnBookPath = {});
  void goToJoinNetworkFileTransfer(std::string returnBookPath = {});
  void goToHotspotFileTransfer(std::string returnBookPath = {});
  void goToNearbyStatsSync();
  void goToSettings();
  void goToFileBrowser(std::string path = {});
  void goToRecentBooks();
  void goToBrowser();
  void goToReader(std::string path, bool suppressBackRelease = false);
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE);

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  bool isReaderActivity() const;
  // Unlike isReaderActivity(), this checks only the activity currently on
  // screen and ignores a reader parked underneath a modal/menu activity.
  bool isCurrentReaderActivity() const;
  // True from the moment releaseCurrentActivityHeavyResources() runs until
  // push/pop/replaceActivity() is actually called. The render task runs
  // independently (see renderTaskLoop(), woken by xTaskNotify) and can still
  // call the outgoing activity's render() in that window - activities that
  // had heavy state released early (see releaseCurrentActivityHeavyResources())
  // should check this and skip any work that would reacquire it, since a
  // reload racing against whatever the caller is itself doing (e.g. loading
  // a second Epub for a background sync) can contend for the same
  // non-reentrant resources (the SD card) from two tasks at once.
  // pendingAction alone is NOT enough for this: it's only set once
  // pushActivity() is actually called, which can be well after
  // releaseCurrentActivityHeavyResources() already dropped the resource -
  // this flag covers that whole earlier window too.
  bool hasPendingActivityTransition() const {
    return pendingAction != PendingAction::None || backgroundTransitionPending;
  }
  // Immediately releases the current activity's heavy heap-resident state
  // (see Activity::releaseHeavyResourcesForBackgroundActivity()). Call this
  // before doing your own heavy allocation/loading if you're about to
  // pushActivity() - pushActivity() alone only frees it once the queued push
  // is actually processed, which is too late to help your own peak usage.
  void releaseCurrentActivityHeavyResources();
  bool canSnapshotForSleepOverlay() const;
  bool skipLoopDelay() const;
  std::string getCurrentBookPath() const;
  ScreenshotInfo getScreenshotInfo() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Returns Rejected when a synchronous render would be unsafe, such as from the render task,
  // while another task is already waiting, or while holding a RenderLock.
  RequestUpdateResult requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
