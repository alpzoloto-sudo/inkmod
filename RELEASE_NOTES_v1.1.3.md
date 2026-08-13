# inkMOD v1.1.3

## Re-uploaded stable build

This revision replaces the earlier v1.1.3 release asset and includes post-release stability fixes discovered during real-world testing on Xteink X4/X3.

### Stability and memory

- Reworked the FB2 structural index for very large books to reduce long-lived heap usage.
- Tested direct opening of `Колесо Времени.fb2` (~46.2 MB) without preliminary conversion.
- Large FB2.ZIP archives use a safer sequential preparation path when the fused decompression/XML path would create unnecessary memory pressure.
- Kept the safe idle CPU downclock while removing experimental light-sleep that could freeze X4 after USB disconnect.
- Continued low-memory fallback behavior for image-heavy books so text reading is preferred over an OOM reboot.

### Reader

- Fixed dictionary quick-action button carry-over: Back/Confirm/front buttons no longer execute an accidental first action after opening the dictionary.
- Fixed the first Menu press after leaving the dictionary.
- Improved clipping creation/removal and clipping-list rendering.
- Preserved logical chapter numbering for internally split large EPUB/FB2 chapters.
- Improved XTC/XTCH low-memory streaming and refresh behavior.

### UI

- Fixed seven-icon centering in Lyra Carousel.
- Fixed duplicate TXT reader clock placement.
- Improved two-line file names in the file manager.
- Added customizable reader-menu ordering and visibility.
- Added the Support InkMOD screen and Telegram project link.

### Web portal

- EPUBKIT browser-side EPUB optimizer.
- Dictionary preparation/upload workflow.
- Sleep-screen PNG generator with background removal and direct upload.
- File rename support.

### Support

InkMOD stays free. Optional project support is available through the Telegram channel:

https://t.me/inkmodx4
