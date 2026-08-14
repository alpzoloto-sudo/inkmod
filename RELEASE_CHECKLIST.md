# inkMOD v1.1.3 release checklist

## Required before tagging

- [ ] `python scripts/release_check.py`
- [ ] `pio run -e tiny`
- [ ] Flash the resulting `tiny` build on an X4 and complete a cold boot.
- [ ] Open at least one EPUB and one cached FB2/FB2.ZIP.
- [ ] In FB2, change the reading font and confirm the current chapter rebuilds once and opens normally.
- [ ] Verify `Sunlight Fading Fix = OFF` does not log `sunlight power-down` after FAST refresh.
- [ ] Verify `Sunlight Fading Fix = ON` logs `sunlight power-down` on SSD1677/X4 and does not add an extra image flash.
- [ ] Check navigation/settings for visible ghosting after normal use.
- [ ] Check Wi-Fi/NTP boot synchronization on X4, then confirm Wi-Fi returns to OFF.
- [ ] Check X3 smoke test if hardware is available; the SSD1677 sunlight change must not execute there.
- [ ] Confirm OTA metadata reports `1.1.3` in Settings/web UI.

## Tag and publish

```bash
./scripts/release.sh 1.1.3
git push origin main
git push origin v1.1.3
```

The GitHub release workflow builds `tiny` and publishes `firmware-tiny-v1.1.3.bin`.

## 1.1.4 stress corpus

Host-side structural check:

```bash
python scripts/book_corpus_check.py <large.epub> <large.fb2> <large.fb2.zip>
```

Known stress characteristics used while preparing 1.1.4:

- EPUB: ~346 KiB archive with a single ~1.08 MiB XHTML member.
- FB2: ~46.5 MiB source, 711 sections, 74 binaries, ~97.7k paragraphs.
- FB2.ZIP: ~2.16 MiB uncompressed FB2, 30 sections, 5 binaries.

Hardware gate before publishing:

- [ ] release build opens the oversized-XHTML EPUB without reset
- [ ] release build opens/reopens the ~46.5 MiB FB2 without reset
- [ ] FB2.ZIP first-open and second-open both work
- [ ] XTC/XTCH rapid paging remains stable
- [ ] dictionary and clippings open/close correctly
- [ ] manual sleep and timeout sleep both wake correctly
- [ ] `/crash_report.txt` absent after a clean run
