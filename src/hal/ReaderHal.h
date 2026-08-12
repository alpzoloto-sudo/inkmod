// Reader-facing HAL contracts.
//
// These interfaces deliberately sit beside legacy lib/hal instead of replacing
// working X4 drivers. New reader code depends on this namespace only; a small
// legacy adapter is the sole place allowed to touch the existing globals.

#pragma once

#include <cstdint>

namespace reader::hal {

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
};

enum class RefreshMode : uint8_t { Full, Fast };

class Display {
 public:
  virtual ~Display() = default;
  virtual void clear() = 0;
  virtual void update(RefreshMode mode = RefreshMode::Fast) = 0;
  // A backend may conservatively perform a full fast refresh if its panel
  // cannot safely refresh an arbitrary rectangle.
  virtual void updatePartial(const Rect& dirty, RefreshMode mode = RefreshMode::Fast) = 0;
};

class Storage {
 public:
  virtual ~Storage() = default;
  // Paths are null-terminated and owned by the caller. This keeps the HAL
  // allocation-free on an ESP32-C3; a future path object may validate bounds.
  virtual bool exists(const char* path) const = 0;
  virtual bool remove(const char* path) = 0;
};

enum class Key : uint8_t { None, Up, Down, Left, Right, Confirm, Back, Power };
enum class KeyAction : uint8_t { Pressed, Released, Held };
struct KeyEvent { Key key = Key::None; KeyAction action = KeyAction::Released; };

class Input {
 public:
  virtual ~Input() = default;
  virtual bool poll(KeyEvent& out) = 0;
};

class Power {
 public:
  virtual ~Power() = default;
  virtual void sleep() = 0;
  virtual void shutdown() = 0;
};

}  // namespace reader::hal
