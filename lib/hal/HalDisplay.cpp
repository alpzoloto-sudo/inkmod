#include <HalDisplay.h>
#include <HalGPIO.h>

#include "HalSpiBus.h"

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

namespace {

// Xteink production firmware identifies the newer X4 panel as
// GDEQ0426T82 (QY 4.26), driven by SSD1677. Re-apply the panel vendor's
// booster soft-start profile after the common SSD1677 initialization.
// This is intentionally X4-only: the X3 uses a different controller path.
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

  if (Serial) {
    Serial.printf("[%lu]   X4 panel profile: GDEQ0426T82/QY SSD1677 compatibility enabled\n", millis());
  }
}

}  // namespace

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
  HalSpiBus::Lock spiLock;

  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  // Newer X4 production revisions use the QY GDEQ0426T82 panel. The common
  // driver already speaks SSD1677; the only vendor-specific correction needed
  // here is the current booster profile. EPD transactions themselves are
  // clamped to the panel-rated 10 MHz in EInkDisplay.h.
  if (gpio.deviceIsX4()) {
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
