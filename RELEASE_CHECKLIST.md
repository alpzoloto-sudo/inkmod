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
