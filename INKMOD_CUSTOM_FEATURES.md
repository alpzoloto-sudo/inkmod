# InkMOD custom integration

This source tree includes the integrated X4 changes requested for the custom build.

## Battery
- X4 ADC percentage keeps EMA smoothing.
- When USB is connected and measured battery voltage reaches 4.140 V or above, UI snaps to 100% instead of slowly crawling through the mid/high 90s.
- `trackChargingState()` / `lastChargeEpochSeconds` are unchanged and remain independent from percentage smoothing.

## Dictionary
- Dictionary lookup can be assigned to short Power, long Power, long Menu/OK and long Back.
- Opening lookup selects the selectable word nearest the visual centre of the current page.
- If dictionary lookup is assigned to one of those buttons, Dictionary is automatically omitted from the book menu.

## Configurable book menu
- Settings -> System -> Book menu.
- OK toggles visibility.
- Short Left/Right moves selection; hold Left/Right reorders the selected entry.
- Dictionary is last in the default order.
- Layout is saved in `bookMenuLayout` inside `/.inkmod/inkmod-settings.json`.

## Sleep overlay from file browser
- Image action: "Set as sleep overlay".
- Sets selected sleep image and switches sleep screen mode to Overlay.
- Does NOT change the user's auto-sleep timeout.
- PNG overlay treats only near-white background connected to the left/right image edge as transparent, preserving enclosed white details such as eyes, socks and highlights.

## Hidden reader Easter egg
Sequence while reading:

`Menu -> Back -> Menu -> Back -> Menu`

The first two Menu presses open the normal reader menu and each Back closes it. The fifth key opens the hidden prompt.

- Yes starts the hidden falling-block mini-game.
- No returns to reading with one of several phrases.
- Gameplay has rotating encouragement phrases.
- Game over and exit also have phrase variants; a new high score gets a special message.
- Save files use intentionally generic hidden names under `/.inkmod/`.
- Source class/file names intentionally avoid the game name.

## Build profiles
Developer (serial logs enabled):

    pio run -e developer

Legacy alias for developer:

    pio run -e tiny

Release (serial logging compiled out, USB CDC / USB flashing remains enabled):

    pio run -e release

Release inherits the same USB build flags from `[base]`; it simply does not define `ENABLE_SERIAL_LOG`.
