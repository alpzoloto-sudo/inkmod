#include "SystemDiagnosticsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <MemoryBudget.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "SettingsList.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

const char* resetReasonRu(const esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "Включение питания";
    case ESP_RST_EXT: return "Внешний сброс";
    case ESP_RST_SW: return "Программная перезагрузка";
    case ESP_RST_PANIC: return "Критическая ошибка";
    case ESP_RST_INT_WDT: return "Сторожевой таймер CPU";
    case ESP_RST_TASK_WDT: return "Сторожевой таймер задачи";
    case ESP_RST_WDT: return "Сторожевой таймер";
    case ESP_RST_DEEPSLEEP: return "Выход из глубокого сна";
    case ESP_RST_BROWNOUT: return "Просадка питания";
    case ESP_RST_SDIO: return "Сброс SDIO";
    case ESP_RST_USB: return "Сброс USB";
    case ESP_RST_JTAG: return "Сброс JTAG";
    case ESP_RST_EFUSE: return "Ошибка eFuse";
    default: return nullptr;
  }
}

std::string resetReasonDisplay() {
  const esp_reset_reason_t reason = esp_reset_reason();
  if (const char* name = resetReasonRu(reason)) return std::string(name);
  char buf[40];
  snprintf(buf, sizeof(buf), "Неизвестно (код %d)", static_cast<int>(reason));
  return std::string(buf);
}

std::string sinceLastChargeDisplay() {
  const uint64_t last = powerManager.getLastChargeEpochSeconds();
  const uint64_t now = static_cast<uint64_t>(time(nullptr));
  if (last == 0 || now < last) return "-";
  const uint64_t elapsed = now - last;
  const uint32_t days = static_cast<uint32_t>(elapsed / 86400ULL);
  const uint32_t hours = static_cast<uint32_t>((elapsed % 86400ULL) / 3600ULL);
  const uint32_t minutes = static_cast<uint32_t>((elapsed % 3600ULL) / 60ULL);
  char buf[40];
  if (days) snprintf(buf, sizeof(buf), "%u д %u ч", days, hours);
  else if (hours) snprintf(buf, sizeof(buf), "%u ч %u мин", hours, minutes);
  else snprintf(buf, sizeof(buf), "%u мин", minutes);
  return std::string(buf);
}

std::string bytesHuman(uint64_t bytes) {
  char buf[32];
  if (bytes >= 1073741824ULL) {
    snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1073741824.0);
  } else if (bytes >= 1048576ULL) {
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
  } else {
    snprintf(buf, sizeof(buf), "%llu KB", static_cast<unsigned long long>(bytes / 1024ULL));
  }
  return buf;
}
}

void SystemDiagnosticsActivity::onEnter() {
  Activity::onEnter();
  storageReadySeen = StorageUsageCalc::ready();
  requestUpdate(true);
}

void SystemDiagnosticsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    StorageUsageCalc::start();
    requestUpdate(true);
  }

  const bool readyNow = StorageUsageCalc::ready();
  if (readyNow != storageReadySeen) {
    storageReadySeen = readyNow;
    requestUpdate(true);
  }
}

void SystemDiagnosticsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DIAGNOSTICS));

  const auto heap = MemoryBudget::snapshot();
  const bool crashReport = Storage.exists("/crash_report.txt");

  std::vector<std::pair<std::string, std::string>> rows;
  rows.reserve(10);
  rows.push_back({tr(STR_DIAG_FIRMWARE), std::string(INKMOD_VERSION) + " (" + INKMOD_FIRMWARE_VARIANT + ")"});
  rows.push_back({tr(STR_DIAG_DEVICE), gpio.deviceIsX3() ? "X3" : "X4"});
  rows.push_back({tr(STR_DIAG_FREE_HEAP), bytesHuman(heap.freeHeap)});
  rows.push_back({tr(STR_DIAG_MAX_ALLOC), bytesHuman(heap.maxAllocHeap)});
  rows.push_back({tr(STR_INTERNAL_STORAGE), StorageUsageCalc::display()});
  rows.push_back({tr(STR_BATTERY), std::to_string(powerManager.getBatteryPercentage()) + "%"});
  // X4's "since last charge" depends on the software clock. When the clock is
  // disabled the value is not meaningful, so omit the row instead of showing
  // stale/partial timing information.
  if (!SETTINGS.clockDisabled) {
    rows.push_back({tr(STR_SINCE_LAST_CHARGE), sinceLastChargeDisplay()});
  }
  rows.push_back({tr(STR_DIAG_RESET_REASON), resetReasonDisplay()});
  rows.push_back({tr(STR_DIAG_CRASH_REPORT), crashReport ? tr(STR_YES) : tr(STR_NO)});

  const int left = metrics.contentSidePadding;
  const int right = pageWidth - metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  for (const auto& row : rows) {
    if (y + lineH >= pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing) break;
    renderer.drawText(UI_10_FONT_ID, left, y, row.first.c_str());
    const int valueW = renderer.getTextWidth(UI_10_FONT_ID, row.second.c_str());
    renderer.drawText(UI_10_FONT_ID, std::max(left, right - valueW), y, row.second.c_str());
    y += lineH + metrics.verticalSpacing;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
