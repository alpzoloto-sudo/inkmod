#include "DeveloperModeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "diagnostics/DiagnosticManager.h"
#include "fontIds.h"

namespace {
struct Item { const char* ru; const char* en; DiagnosticManager::Flag flag; bool toggle; };
constexpr Item ITEMS[] = {
  {"Режим разработчика", "Developer mode", DiagnosticManager::Developer, true},
  {"Расширенное логирование", "Extended logging", DiagnosticManager::Extended, true},
  {"Лог чтения книг", "Reader logging", DiagnosticManager::Reader, true},
  {"Лог файловой системы", "Filesystem logging", DiagnosticManager::Filesystem, true},
  {"Лог памяти", "Memory logging", DiagnosticManager::Memory, true},
  {"Лог Wi-Fi / Web / OTA", "Wi-Fi / Web / OTA logging", DiagnosticManager::Network, true},
  {"Лог дисплея", "Display logging", DiagnosticManager::Display, true},
  {"Снимок состояния", "State snapshot", DiagnosticManager::Developer, false},
  {"Принудительная очистка дисплея", "Force display clean", DiagnosticManager::Developer, false},
  {"Создать диагностический отчёт", "Create diagnostic report", DiagnosticManager::Developer, false},
  {"Очистить диагностические данные", "Clear diagnostic data", DiagnosticManager::Developer, false},
};
const char* label(const Item& i) { return i.ru; }
}

void DeveloperModeActivity::onEnter() { Activity::onEnter(); DIAGNOSTICS.begin(); requestUpdate(true); }

void DeveloperModeActivity::activate() {
  if (selected < 7) {
    DIAGNOSTICS.toggle(ITEMS[selected].flag);
  } else if (selected == 7) {
    DIAGNOSTICS.snapshot("DeveloperMode");
    GUI.drawPopup(renderer, "Snapshot saved");
    renderer.displayBuffer(); delay(650);
  } else if (selected == 8) {
    renderer.clearScreen(); renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    renderer.clearScreen(); renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  } else if (selected == 9) {
    const bool ok = DIAGNOSTICS.createReport();
    GUI.drawPopup(renderer, ok ? "Report saved" : "Report failed"); renderer.displayBuffer(); delay(650);
  } else if (selected == 10) {
    DIAGNOSTICS.clearFiles();
    GUI.drawPopup(renderer, "Diagnostic files cleared"); renderer.displayBuffer(); delay(650);
  }
  requestUpdate(true);
}

void DeveloperModeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }

  // Use the same navigation helper as the rest of Settings.  On X4 the
  // physical third/fourth buttons may map to Left/Right depending on the
  // current orientation/remap, while the on-screen hints still mean Up/Down.
  // ButtonNavigator accepts both pairs and therefore keeps this engineering
  // screen navigable with every supported button layout.
  buttonNavigator.onPreviousRelease([this] {
    selected = ButtonNavigator::previousIndex(selected, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onNextRelease([this] {
    selected = ButtonNavigator::nextIndex(selected, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selected = ButtonNavigator::previousIndex(selected, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selected = ButtonNavigator::nextIndex(selected, ITEM_COUNT);
    requestUpdate();
  });
}

void DeveloperModeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth(), h = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0,m.topPadding,w,m.headerHeight}, "Режим разработчика");
  const int left=m.contentSidePadding, right=w-m.contentSidePadding;
  int y=m.topPadding+m.headerHeight+m.verticalSpacing;
  const int lh=renderer.getLineHeight(UI_10_FONT_ID);
  const int visible=std::max(1,(h-y-m.buttonHintsHeight-6)/(lh+m.verticalSpacing));
  int start=0; if(selected>=visible) start=selected-visible+1;
  for(int i=start;i<ITEM_COUNT && i<start+visible;i++) {
    if(i==selected) renderer.fillRect(left-4,y-2,right-left+8,lh+4,true);
    renderer.drawText(UI_10_FONT_ID,left,y,label(ITEMS[i]), i!=selected);
    if(ITEMS[i].toggle) {
      const char* v=DIAGNOSTICS.enabled(ITEMS[i].flag)?tr(STR_YES):tr(STR_NO);
      int vw=renderer.getTextWidth(UI_10_FONT_ID,v);
      renderer.drawText(UI_10_FONT_ID,right-vw,y,v,i!=selected);
    }
    y+=lh+m.verticalSpacing;
  }
  const auto labels=mappedInput.mapLabels(tr(STR_BACK),tr(STR_SELECT),tr(STR_DIR_UP),tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer,labels.btn1,labels.btn2,labels.btn3,labels.btn4,true);
  renderer.displayBuffer();
}
