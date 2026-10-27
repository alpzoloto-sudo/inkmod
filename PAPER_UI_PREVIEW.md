# Paper UI — preview firmware (X3/X4 and X4 Pro)

This experimental branch is intentionally separate from the inkMOD v1.1.10 release.

## What you can test immediately

1. Install the PR's **x3x4-test-firmware** on X3/X4, or **x4pro-test-firmware** on X4 Pro. Do not cross-flash.
2. Go to **Settings → Display → UI Theme → Paper UI (preview)**.
3. Visit Home, Settings, file browser, dictionary, notes and the reading menus. The reader itself keeps the established text/layout engine.
4. On X4 Pro, verify that tapping settings rows and Home cards works on every visible row and after scrolling.
5. Change back to Classic/RoundedRaff in Settings to compare. All previous UI themes remain available.

## Safety boundaries

- This is a **preview skin**, not a wholesale replacement of the Activity/navigation engine.
- No changes to EEPROM/NVS layout, screen waveforms, filesystem, dictionary formats, notes or network protocol.
- New persisted theme numeric ID is 7; IDs 0–6 are unchanged.
- Explicitly test theme switching, portrait/landscape, low battery, home with/without book cover, a very long book name and settings with long localized strings.
- A compile-success result does not guarantee touch or E-Ink rendering on actual hardware.

## Follow-up features for Paper UI

Bottom-sheet equivalents for high-frequency reader settings, redesigned library filtering, unified note/dictionary cards and a dedicated touch navigation bar require functional changes and physical touch-map validation. They have **not** been implemented by the initial theme preview.
