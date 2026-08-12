// Legacy X4 backend for reader::hal interfaces.
#pragma once

#include <HalGPIO.h>
#include <HalPowerManager.h>

#include "MappedInputManager.h"
#include "ReaderHal.h"

namespace reader::hal {

class LegacyDisplay final : public Display {
 public:
  void clear() override;
  void update(RefreshMode mode) override;
  void updatePartial(const Rect& dirty, RefreshMode mode) override;
};

class LegacyStorage final : public Storage {
 public:
  bool exists(const char* path) const override;
  bool remove(const char* path) override;
};

class LegacyInput final : public Input {
 public:
  explicit LegacyInput(MappedInputManager& input) : input_(input) {}
  bool poll(KeyEvent& out) override;

 private:
  MappedInputManager& input_;
};

class LegacyPower final : public Power {
 public:
  LegacyPower(HalPowerManager& power, HalGPIO& gpio) : power_(power), gpio_(gpio) {}
  void sleep() override;
  void shutdown() override;

 private:
  HalPowerManager& power_;
  HalGPIO& gpio_;
};

}  // namespace reader::hal
