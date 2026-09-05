# 📖 inkMOD User Guide

**inkMOD** is an open-source custom firmware for the **Xteink X4 / X3**, focused on comfortable everyday reading, native book-format support, improved typography and useful reader features while remaining lightweight enough for the limited hardware of the device.

> **Installation and flashing instructions are intentionally not duplicated here.**
> Use the current installation guide published with inkMOD / on 4PDA for flashing and recovery procedures.

---

## 1. What inkMOD can read

inkMOD supports:

* **EPUB**
* **FB2**
* **FB2.ZIP**
* **TXT**
* **XTC**
* **XTCH**
* supported book archives detected by their actual contents

### No mandatory book conversion

You do **not** need to convert FB2 to EPUB or prepare every book on a computer before copying it to the reader.

inkMOD is designed to open normal book files directly.

Large books, large chapters and FB2.ZIP archives are processed using memory-conscious streaming specifically because the Xteink hardware has very little available RAM.

For particularly large or image-heavy EPUB files, the web interface also provides **EPUBKIT** browser-side optimization. This is an optional tool rather than a requirement for reading books.

---

# 2. Controls

The standard Xteink physical controls are used.

## Front buttons

* **Back**
* **Confirm / Menu**
* **Left**
* **Right**

## Side controls

* **Power**
* **Volume Up**
* **Volume Down**
* **Reset**

The exact actions of many buttons can be changed in:

**Settings → Controls**

Page-turn buttons and their long-press actions can also be customized.

---

# 3. Basic navigation

## Menus and lists

Use:

* **Left / Volume Up** — move upward
* **Right / Volume Down** — move downward
* **Confirm** — select
* **Back** — return

Holding a navigation button automatically repeats the action, which makes long lists much faster to navigate.

---

# 4. Home screen

The Home screen provides quick access to the main reader functions.

Depending on the selected theme, it can display:

* the currently opened book;
* recent books;
* reading progress;
* covers;
* shortcuts to files;
* file transfer;
* settings and other reader functions.

Several home-screen layouts are available, including:

* **Lyra**
* **Lyra Carousel**
* **RoundedRaff**
* **Minimal**
* **Dashboard**

The exact appearance and available shortcuts depend on the selected theme.

---

# 5. File Browser

The built-in File Browser is used for books, folders, images and other files stored on the SD card.

You can:

* browse folders;
* open books;
* open supported images;
* rename files;
* delete files;
* view book information;
* clear a book cache;
* mark a book as finished or unfinished;
* use supported PNG/BMP images as sleep screens.

Long filenames are displayed using multi-line wrapping instead of simply being cut off.

## Long press on a book

Holding **Confirm / Open** on a book opens additional actions.

Depending on the file, these can include:

* **Book information**
* **Rename**
* **Delete**
* **Delete book cache**
* **Mark finished / unfinished**

### Book information

The information screen can show:

* book/file format;
* source file size;
* cache/preparation status;
* basic loading information.

This check is intentionally lightweight and does not fully parse the book.

---

# 6. Reading

Open a supported book from the File Browser, Recent Books or Home screen.

## Page turning

By default:

* **Left / Volume Up** — previous page
* **Right / Volume Down** — next page

The side-button direction can be reversed in settings.

Long-press actions are configurable and can be assigned to actions such as:

* next/previous chapter;
* jump several pages;
* change font size;
* change orientation;
* create a clipping;
* other reader commands.

---

# 7. EPUB and FB2 rendering

inkMOD contains extensive changes to the original reader engine.

## EPUB

The reader supports:

* EPUB text and chapters;
* embedded book formatting;
* images;
* CSS-based layout where supported;
* footnotes;
* chapter navigation;
* large EPUB files using low-memory processing.

## FB2 / FB2.ZIP

FB2 does not need to be converted to EPUB.

inkMOD understands FB2 document structure and provides dedicated formatting for elements such as:

* headings;
* subtitles;
* annotations;
* epigraphs;
* quotations;
* poems;
* stanzas;
* authors/signatures;
* emphasis.

Poetry and other structured text are kept visually distinct from ordinary paragraphs.

Large logical chapters may internally be divided into smaller pieces to stay within the Xteink memory limit, but inkMOD presents them to the reader as one logical chapter and maintains normal chapter page numbering.

---

# 8. Reader appearance

Reading appearance can be customized from:

**Settings → Reader**

Available options include settings for:

* font family;
* font size;
* line spacing;
* screen margins;
* paragraph alignment;
* paragraph spacing;
* embedded book styles;
* hyphenation;
* text anti-aliasing;
* images;
* reading orientation.

## Embedded book styles

When embedded styles are enabled, inkMOD attempts to preserve the formatting intended by the book author instead of forcing everything into plain paragraphs.

For FB2 this includes dedicated semantic styling implemented by inkMOD.

## Hyphenation

Word hyphenation can be enabled to improve text justification and line layout.

## Anti-aliasing

Text anti-aliasing adds grayscale edges to glyphs for smoother-looking fonts.

It can be disabled if maximum page-turn speed is preferred.

---

# 9. Custom fonts

inkMOD supports custom reader fonts in **`.cpfont`** format.

Fonts can be installed in several ways.

## From the web interface

The easiest method is:

1. Open **File Transfer** on the reader.
2. Open the inkMOD web interface.
3. Go to **Fonts**.
4. Upload a prepared `.cpfont` font or select a `.ttf` / `.otf` font for conversion.
5. inkMOD's web tools prepare and upload the resulting font.

## From the SD card

Prepared fonts can also be placed in the font directory on the SD card.

After installation they appear in the reader font list.

The available font sizes depend on the sizes contained in that particular font package.

---

# 10. Dictionary

inkMOD includes built-in dictionary lookup while reading.

It supports:

* word selection directly on a book page;
* multiple installed dictionaries;
* **StarDict** dictionaries;
* StarDict synonym tables;
* long dictionary articles;
* multi-page dictionary entries;
* punctuation-aware lookup;
* words split by line-end hyphenation.

Dictionary articles are paginated directly on the reader.

## Installing dictionaries

The inkMOD web interface can prepare dictionary source files in the browser and upload the optimized dictionary to the device.

This prevents the Xteink itself from having to perform expensive dictionary indexing.

---

# 11. Clippings / saved text

inkMOD can save text excerpts from **EPUB and FB2** books.

Selected passages can be stored as clippings and managed later.

The **Create Clipping** action can also be assigned to a physical-button action or long press.

This makes it possible to save interesting passages without leaving the book.

---

# 12. Bookmarks

Bookmarks can be created for individual books.

They allow you to save a position and later return to it.

Bookmarks can be viewed and removed from the Reader Menu.

---

# 13. Reader Menu

Press **Confirm** while reading to open the Reader Menu.

Depending on the current book and enabled features, the menu can provide actions such as:

* chapter selection;
* bookmarks;
* dictionary;
* clippings;
* reader options;
* controls;
* orientation;
* automatic page turning;
* jump to position;
* reading statistics;
* screenshot;
* book cache removal;
* mark finished / unfinished;
* other configured reader actions.

### Custom menu

inkMOD allows reader-menu entries to be reordered or hidden.

This means frequently used commands can be placed near the top and unwanted entries can be removed from the menu.

---

# 14. Reading statistics

inkMOD tracks reading activity.

Available information can include:

* reading time;
* current session;
* reading progress;
* finished-book status;
* book statistics.

Completed books can be marked as **Finished**, either automatically where applicable or manually from the book menu.

---

# 15. Screenshots

Screenshots can be saved directly on the reader.

A screenshot can be created using the configured physical-button shortcut or from the Reader Menu.

Screenshots are saved to the SD card and can later be downloaded through the web interface.

---

# 16. Sleep screens

inkMOD provides several sleep-screen styles.

Depending on the firmware version and selected mode, these can include:

* inkMOD screen;
* current book cover;
* custom image;
* custom overlay;
* calendar;
* reading information;
* Quick Resume;
* separate screen for automatic timeout sleep.

## Book cover

The current book cover can be displayed while the device is sleeping.

inkMOD contains dedicated handling for EPUB and FB2 covers and avoids unnecessarily enlarging very small images when that would result in poor quality.

## Custom image

Supported PNG/BMP images can be selected directly from the File Browser.

## Sleep-screen generator

The web interface contains a dedicated sleep-screen preparation tool.

It can:

* prepare an image for the Xteink display;
* handle PNG transparency;
* remove edge-connected backgrounds;
* upload the finished image;
* apply it directly to the reader.

---

# 17. Separate timeout sleep screen

Automatic sleep after inactivity can use a different screen from manually activated sleep.

The available timeout behavior includes options such as:

* same as normal sleep;
* Quick Resume;
* custom overlay with Quick Resume;
* custom image.

This makes it possible, for example, to keep the current page visible during short automatic sleeps while using a book cover or custom image for normal sleep.

---

# 18. Web interface

One of inkMOD's major features is its browser-based management interface.

Open **File Transfer** on the device and connect the reader to Wi-Fi.

The screen displays the address needed to access the web interface.

Depending on the network, `inkmod.local` may also be available.

## Files

The web interface can be used to:

* upload books;
* upload several files;
* upload directory trees;
* create and manage folders;
* rename files;
* delete files;
* download files from the reader.

Books can normally be uploaded **as they are**.

No mandatory computer-side conversion step is required.

---

# 19. EPUBKIT

EPUBKIT is an optional browser-side EPUB preparation tool included with inkMOD.

Processing takes place in the browser instead of consuming the reader's limited RAM.

It can be useful for:

* very large EPUB files;
* books containing many large images;
* badly prepared EPUB files;
* preparing images specifically for the Xteink E-Ink display.

For ordinary books you can simply upload the original EPUB and read it.

---

# 20. Browser-side processing

Several heavy tasks are deliberately performed by your computer or phone browser instead of by the Xteink.

This includes tools for:

* EPUB optimization;
* dictionary preparation;
* font conversion;
* sleep-screen preparation.

The finished data is then uploaded to the reader.

This architecture lets inkMOD provide features that would otherwise be difficult to perform directly on the memory-constrained ESP32-C3.

---

# 21. Wi-Fi and time

## Xteink X4

The X4 does not contain a normal hardware real-time clock.

inkMOD therefore synchronizes the current time through Wi-Fi and preserves the last synchronized time for offline use.

The clock can also be disabled if it is not needed.

## Xteink X3

Supported X3 hardware can use its hardware RTC.

---

# 22. Firmware updates

inkMOD can check GitHub for new stable releases through Wi-Fi.

Use the firmware update function in:

**Settings → System**

Stable release builds are separated from developer/debug builds.

For normal everyday use, use the regular **release firmware** unless you specifically need a diagnostic build.

---

# 23. Diagnostics

Open:

**Settings → System → Device → Diagnostics**

The diagnostics screen provides useful information such as:

* detected device/display variant;
* available RAM;
* largest available memory block;
* SD card/storage information;
* reset reason;
* crash-report status.

This information is especially useful when reporting problems with very large books.

---

# 24. Crash reports

If inkMOD detects certain memory-related or guarded crashes, diagnostic information can be stored on the SD card.

Check for:

`/crash_report.txt`

If you report a reproducible crash, attach this file together with:

* the firmware version;
* device model/revision;
* what you were doing before the problem occurred;
* the problematic book if it can legally be shared.

Release firmware keeps lightweight diagnostic breadcrumbs without requiring permanent verbose serial logging.

---

# 25. Cache

inkMOD stores generated reader data on the SD card so books do not have to be completely analyzed every time they are opened.

If one specific book behaves incorrectly after an update:

1. Open the File Browser.
2. Hold **Confirm / Open** on that book.
3. Select **Delete Book Cache**.
4. Open the book again.

The cache will be rebuilt.

There is normally no reason to delete the entire inkMOD data directory.

---

# 26. Recovery

inkMOD includes an SD-card recovery mechanism for cases where the normal firmware cannot start correctly.

Recent releases support recovery using:

`inkmod-recovery.bin`

The recovery image is checked before the normal interface starts.

For the exact recovery procedure and firmware installation instructions, use the current recovery/install guide supplied with the firmware or the inkMOD instructions on 4PDA.

Do not use recovery files intended for a different device or hardware revision.

---

# 27. E-Ink refresh and ghosting

E-Ink displays work differently from LCD/OLED screens.

A small amount of residual content after page turns is normal.

inkMOD allows periodic full refreshes to reduce ghosting.

The refresh interval can be adjusted in Display/Reader settings.

More frequent full refreshes:

* reduce ghosting;
* cause more visible black/white flashing.

Less frequent full refreshes:

* reduce flashing;
* can allow more residual image buildup.

Choose whichever behavior is more comfortable for you.

---

# 28. Large books

The Xteink X4 is based on an ESP32-C3 with very limited RAM.

inkMOD contains several systems specifically designed around this limitation:

* streaming book processing;
* compact FB2 indexing;
* low-memory FB2.ZIP handling;
* logical chapter splitting;
* on-demand pagination;
* browser-side processing for heavy preparation work.

Because of this, very large FB2, FB2.ZIP and EPUB books can be opened without loading the complete document into RAM.

An unusually large or malformed book can still take longer to open for the first time.

Subsequent opens are usually faster once the necessary cache has been created.

---

# 29. Supported interface languages

inkMOD includes complete **Russian** and **Ukrainian** localization together with the other languages inherited and maintained by the project.

The on-screen keyboard includes dedicated layouts for:

* English;
* Russian;
* Ukrainian.

Custom fonts can be used when a book requires characters that are missing from the currently selected reader font.

---

# 30. Recommended everyday setup

There is no single required configuration, but a simple starting setup is:

* choose the interface language you prefer;
* select a comfortable reader font;
* enable hyphenation if you use justified text;
* enable embedded book styles if you want to preserve the author's formatting;
* set the desired full-refresh interval;
* choose a sleep screen;
* configure side buttons the way you prefer;
* connect Wi-Fi for OTA updates, time synchronization and web tools.

After that, simply copy your books to the SD card or upload them through the web interface.

**No mandatory book optimization or conversion is required.**

---

# 31. If something goes wrong

For a problem with one book:

1. Delete only that book's cache.
2. Open the book again.
3. If the problem remains, check **Diagnostics**.
4. Check whether `/crash_report.txt` was created.
5. Report the problem with the firmware version and book format.

For a firmware/startup problem, use the current inkMOD recovery instructions.

Avoid deleting all settings or reflashing the device before checking whether the problem is only a damaged book cache.

---

# 32. Project and support

inkMOD is free and open source.

The project is developed primarily for the Xteink X4 while maintaining compatible X3 support.

Project development focuses on:

* native book-format support;
* reliable reading;
* low-memory operation;
* good FB2/EPUB typography;
* useful reader tools;
* Russian and Ukrainian localization;
* extending the Xteink without requiring users to preprocess their entire library.

Project repository:

**GitHub: alpzoloto-sudo/inkmod**

Community, releases, discussion and support:

**Telegram: @inkmodx4**

Installation instructions, community experience and device-specific guides are also available in the inkMOD / Xteink discussion on **4PDA**.

---

## Important

inkMOD is custom firmware.

Installation and use are performed at the device owner's own risk.

Use firmware and recovery files intended for your exact supported Xteink model/revision and follow the current installation instructions.

---

**Enjoy reading with inkMOD. 📚**
