# OEM 6.2.4 X4 panel validation

Use this only on the isolated `vendor-drivers-6.2.4` branch.

## New-revision X4

- Cold boot: logo/home screen must appear without blank or corrupted panel.
- Full refresh: no permanent black/white lockup.
- Fast page turns: 20 consecutive turns without BUSY timeout.
- Grayscale/image page: stable 4-level output.
- Sleep/wake: wake restores normal refresh operation.
- Reboot after sleep: panel still initializes cleanly.

## Older X4 regression

Repeat the same sequence on an older working X4. The 10 MHz EPD transaction rate and QY booster profile must not regress old panels.

## Serial evidence

Expected experimental boot marker:

`X4 OEM panel profile: GDEQ0426T82/QY SSD1677 enabled`

If a new-revision X4 still remains blank, capture the boot serial log from power-on through the first refresh. The next reconstruction stage will then focus on reset/BUSY timing and the OEM full/partial waveform sequence rather than adding more speculative controller variants.

CI note: this branch is intentionally validated independently from `optimize/core`; do not merge until hardware tests pass on both old and new X4 revisions.
