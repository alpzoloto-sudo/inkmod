#include <HalDisplay.h>
#include <HalGPIO.h>
#include <BoardConfig.h>

#include "HalSpiBus.h"
#include "nvs.h"

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

namespace {

// Read the controller type written by the factory without touching the EPD bus.
// App-only flashes at 0x10000 leave this NVS calibration partition intact. If the
// key is missing/unknown (for example after a full erase), fail closed to the
// profile's stock controller.
bool applyOemNvsDisplayController(bool isX3) {
  nvs_handle_t h;
  if (nvs_open("hw_calib", NVS_READONLY, &h) != ESP_OK) {
#ifdef ENABLE_SERIAL_LOG
    if (Serial) Serial.printf("[%lu] [EPD] hw_calib missing; keep stock controller\n", millis());
#endif
    return false;
  }

  uint8_t screenType = 0;
  const esp_err_t err = nvs_get_u8(h, "screenType", &screenType);
  nvs_close(h);
  if (err != ESP_OK) {
#ifdef ENABLE_SERIAL_LOG
    if (Serial) Serial.printf("[%lu] [EPD] screenType missing; keep stock controller\n", millis());
#endif
    return false;
  }

  // Factory encodings recovered from the Xteink hardware-selection code:
  //   1 / 0x0B = UC8179 family (X4 only)
  //   2 / 0x0C = UC8279 family (new X3/X4 production runs)
  //   3         = original controller for the device family
  // Multiple board/display revisions can therefore share one controller driver.
  // We select the full X3 sibling profile for UC8279, not just its controller
  // field, so every revision-specific profile setting stays coherent.
  if (isX3) {
    if (screenType == 2 || screenType == 0x0C) {
      BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
#ifdef ENABLE_SERIAL_LOG
      if (Serial) Serial.printf("[%lu] [EPD] X3 screenType=%u -> XteinkX3Uc8279 profile\n", millis(), screenType);
#endif
      return true;
    }

    // 1/0x0B identify the UC8179 family used by X4. Do not ever promote an X3
    // to an X4 controller/profile from that value; an unknown/corrupted NVS key
    // must leave the known-working X3 UC8253 profile intact.
#ifdef ENABLE_SERIAL_LOG
    if (Serial) {
      if (screenType == 1 || screenType == 0x0B)
        Serial.printf("[%lu] [EPD] X3 screenType=%u is not a known X3 mapping; keep UC8253\n", millis(), screenType);
      else
        Serial.printf("[%lu] [EPD] X3 screenType=%u -> stock UC8253 profile\n", millis(), screenType);
    }
#endif
    return false;
  }

  if (screenType == 1 || screenType == 0x0B) {
    BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8179;
    BoardConfig::ACTIVE.displayControllerVariant = 0x01;
#ifdef ENABLE_SERIAL_LOG
    if (Serial) Serial.printf("[%lu] [EPD] X4 factory screenType=%u -> UC8179\n", millis(), screenType);
#endif
    return true;
  }

  if (screenType == 2 || screenType == 0x0C) {
    BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8279;
    // UC8279 X4 uses the 0x68 waveform set as its safe default when the exact
    // LUT revision is not available without probing the live display bus.
    BoardConfig::ACTIVE.displayControllerVariant = 0x68;
#ifdef ENABLE_SERIAL_LOG
    if (Serial) Serial.printf("[%lu] [EPD] X4 factory screenType=%u -> UC8279 (safe LUT 68)\n", millis(), screenType);
#endif
    return true;
  }

#ifdef ENABLE_SERIAL_LOG
  if (Serial) Serial.printf("[%lu] [EPD] X4 factory screenType=%u -> stock SSD1677\n", millis(), screenType);
#endif
  return false;
}

// Official Xteink X4 6.2.4 production firmware identifies the production
// panel as GDEQ0426T82 (QY 4.26). Re-apply the vendor booster soft-start
// profile after the common SSD1677 initialization. This must run ONLY when the
// selected controller is actually SSD1677; never send it to an UltraChip part.
void applyX4QyPanelCompatibilityProfile() {
  constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x0C;
  constexpr uint8_t booster[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
  const SPISettings panelSpi(10000000, MSBFIRST, SPI_MODE0);

  SPI.beginTransaction(panelSpi);
  digitalWrite(EPD_DC, LOW);
  digitalWrite(EPD_CS, LOW);
  SPI.transfer(CMD_BOOSTER_SOFT_START);
  digitalWrite(EPD_CS, HIGH);

  digitalWrite(EPD_DC, HIGH);
  digitalWrite(EPD_CS, LOW);
  SPI.writeBytes(booster, sizeof(booster));
  digitalWrite(EPD_CS, HIGH);
  SPI.endTransaction();

#ifdef ENABLE_SERIAL_LOG
  if (Serial) Serial.printf("[%lu] [EPD] X4 GDEQ0426T82/QY SSD1677 booster enabled\n", millis());
#endif
}

}  // namespace

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
  HalSpiBus::Lock spiLock;

  // Select the device family first. This only chooses the X3/X4 board profile;
  // no display-bus probing is performed.
  const bool isX3 = gpio.deviceIsX3();
  if (isX3) {
    einkDisplay.setDisplayX3();
  } else {
    // X4 OEM 6.2.4 uses a 10 MHz display SPI clock. The E-Ink waveform itself
    // dominates refresh time, so this adds signal margin with negligible UI cost.
    BoardConfig::ACTIVE.displaySpiHz = 10000000;
  }

  // Safe revision selection from factory calibration only. Unknown/missing data
  // leaves the stock controller untouched and boot continues normally.
  applyOemNvsDisplayController(isX3);

  einkDisplay.begin();

  // The QY booster belongs to SSD1677 only. Never emit SSD commands after an
  // NVS-selected UC8179/UC8279 begin.
  if (!isX3 && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::SSD1677) {
    applyX4QyPanelCompatibilityProfile();
  }

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::deepSleep() {
  HalSpiBus::Lock spiLock;
  einkDisplay.deepSleep();
}

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  einkDisplay.displayGrayBuffer(turnOffScreen);
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return einkDisplay.supportsStripGrayscale(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
