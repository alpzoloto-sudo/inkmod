#pragma once

#include "InkMODSettings.h"

inline bool isPowerButtonActionAvailableOutsideReader(const InkMODSettings::SHORT_PWRBTN action) {
  switch (action) {
    case InkMODSettings::SHORT_PWRBTN::SLEEP:
    case InkMODSettings::SHORT_PWRBTN::QUICK_RESUME_SLEEP:
    case InkMODSettings::SHORT_PWRBTN::FORCE_REFRESH:
    case InkMODSettings::SHORT_PWRBTN::SYNC_PROGRESS:
    case InkMODSettings::SHORT_PWRBTN::SCREENSHOT:
    case InkMODSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case InkMODSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
    case InkMODSettings::SHORT_PWRBTN::JOIN_NETWORK:
    case InkMODSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      return true;
    case InkMODSettings::SHORT_PWRBTN::IGNORE:
    case InkMODSettings::SHORT_PWRBTN::PAGE_TURN:
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_FONT:
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
    case InkMODSettings::SHORT_PWRBTN::MARK_FINISHED:
    case InkMODSettings::SHORT_PWRBTN::READING_STATS:
    case InkMODSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
    case InkMODSettings::SHORT_PWRBTN::FOOTNOTES:
    case InkMODSettings::SHORT_PWRBTN::FILE_BROWSER:
    case InkMODSettings::SHORT_PWRBTN::DICTIONARY_LOOKUP:
    case InkMODSettings::SHORT_PWRBTN::CREATE_CLIPPING:
    case InkMODSettings::SHORT_PWRBTN::SHORT_PWRBTN_COUNT:
    default:
      return false;
  }
}

void enterDeepSleep(bool fromTimeout = false);
