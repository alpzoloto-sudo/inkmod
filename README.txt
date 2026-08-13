InkMOD Telegram support patch

Adds:
- System -> "Поддержать InkMOD ❤️"
- bounded QR screen to https://t.me/inkmodx4
- @inkmodx4 label
- EN/RU/UK translations
- web-home support card/button to the Telegram channel

No nags, ads, paid locks or disabled features.

QR layout is bounded for X3/X4:
- intro max 3 lines
- QR size derived from remaining content area
- footer only drawn if it fits
- bottom button-hint area reserved

Does NOT touch XTC, FB2/EPUB, dictionary, clippings, sleep, battery,
power-saving, wallpaper generator or display code.

Build:
  pio run -e developer -t upload
