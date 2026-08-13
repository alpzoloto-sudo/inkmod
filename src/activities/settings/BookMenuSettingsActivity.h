#pragma once
#include <vector>
#include "BookMenuConfig.h"
#include "activities/Activity.h"
class BookMenuSettingsActivity final: public Activity { public: BookMenuSettingsActivity(GfxRenderer& r,MappedInputManager& i):Activity("BookMenuSettings",r,i){} void onEnter() override; void loop() override; void render(RenderLock&&) override; private: std::vector<BookMenuLayoutEntry> layout; int selectedIndex=0; uint32_t leftPressedAt=0,rightPressedAt=0,upPressedAt=0,downPressedAt=0; bool leftMoveHandled=false,rightMoveHandled=false,upMoveHandled=false,downMoveHandled=false; static constexpr uint32_t MOVE_HOLD_MS=550; void moveSelected(int d); void persist(); };
