// Fixed-capacity page commands: no vector growth during layout/render.
#pragma once

#include <array>
#include <cstdint>

#include "reader/document/DocumentTypes.h"

namespace reader {

enum class RenderCommandType : uint8_t { DrawText, DrawImage, DrawLine, FillRect };
struct RenderRect { int16_t x = 0; int16_t y = 0; int16_t width = 0; int16_t height = 0; };
struct RenderCommand {
  RenderCommandType type = RenderCommandType::DrawText;
  RenderRect rect = {};
  TextStyle style = {};
  StringRef text = {};
  ResourceId resource = {};
};

class RenderPage {
 public:
  // 192 commands are bounded page-lifetime memory, not a growing heap cache.
  // RenderPage must be an activity/session member allocated once, never a
  // local task-stack variable.
  static constexpr uint16_t MAX_COMMANDS = 192;
  void clear() { count_ = 0; }
  bool append(const RenderCommand& command) {
    if (count_ >= commands_.size()) return false;
    commands_[count_++] = command;
    return true;
  }
  uint16_t count() const { return count_; }
  const RenderCommand& operator[](uint16_t index) const { return commands_[index]; }

 private:
  std::array<RenderCommand, MAX_COMMANDS> commands_ = {};
  uint16_t count_ = 0;
};

}  // namespace reader
