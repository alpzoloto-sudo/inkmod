# Fonts

This build ships only the system UI font, Inter. All reading-font families
(Bitter, ChareInk7, LexendDeca) and the Bookerly/Roboto SD-card font pack
have been removed from this fork. Inter is used for both the interface and
book text; there is no font-family picker with multiple built-in typefaces.

Note: the SD-card font *loading* mechanism (`SdCardFontSystem` and the
"Manage Fonts" screen) is still present in the code, since large parts of
the reader and settings UI call into it for font-cache lifecycle management.
With no `.fonts`/`fonts` folder on the SD card and no extra families
compiled in, it simply has nothing to discover — but the screen itself,
and the `FONT_FAMILY` enum in `InkMODSettings` (which still lists
LexendDeca/Bitter/ChareInk by name), have not been fully stripped out. See
the project README/CLAUDE.md for the current state of that cleanup.
