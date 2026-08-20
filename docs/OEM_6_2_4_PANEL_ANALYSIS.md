# Xteink OEM 6.2.4 hardware baseline

This branch is intentionally isolated from `optimize/core`. It exists to reconstruct hardware-facing behaviour from official Xteink production firmware without risking the stable inkMOD line.

## Supplied production images

| Image | SHA-256 |
| --- | --- |
| `V6.2.4-X3-CH-PROD-0728_210023.bin` | `561e5341f2af76a2cc5671ed977d3a092e6fe3e820d94e32ac13429d390c540d` |
| `V6.2.4-X3-EN-PROD-0728_210023.bin` | `7ec1b2d1b6e05f78dbfd16a61c620bcbad3d68bc3cff9c3c2280c18ff58ff52a` |
| `V6.2.4-X4-CH-PROD-0728_210023.bin` | `d8fc910ca75ab10ecd749b9b0dfce99fbabb5e8f9bbd90d1e6629dc0819e6aed` |
| `V6.2.4-X4-EN-PROD-0728_210023.bin` | `eb345d59068ad9a2a14164134cfc521444f9fd295ca05d8164f78d17a46a44f0` |

All four files are ESP32-C3 application images. The X4 EN image contains six loadable segments with entry point `0x403818d4`; the large code segment is mapped at `0x42000020` and the primary read-only data segment at `0x3c1e0020`.

## X4 display evidence

Both X4 EN and X4 CH production images contain the same production panel identifiers and display-path labels:

- `GDEQ0426T82(QY 4.26)`
- `QY_GDEQ0426T82`
- `gray4_426`
- `gray4_black_flash`
- `_updateFull`
- `_updatePart`
- `refreshLocalGC`
- `grey_full_wave`
- `grey_fast_wave`
- `grey_bw_base_fc`
- `grey_bw_base_fc_dual_stream`
- `grey_fast_wave_dual_stream`
- `grey_bw_base_fc_stream`
- `grey_fast_wave_stream`

The equivalent X3 images do not contain the QY GDEQ0426T82 identifiers. This makes the QY path an X4-specific production hardware path rather than a locale-specific string set.

## First compatibility layer

The current experimental changes deliberately keep the existing inkMOD rendering/LUT architecture and modify only the X4 hardware link:

1. Clamp EPD SPI transactions to 10 MHz for production-panel compatibility.
2. Re-apply the QY/SSD1677 booster soft-start profile after X4 controller initialization: `AE C7 C3 C0 80` via command `0x0C`.
3. Leave X3 initialization and refresh policy unchanged.

This stage is intentionally conservative. Further OEM reconstruction (full/partial/local-GC/grayscale waveforms, BUSY timing, power-off and sleep sequencing) must be validated separately before replacing existing stable paths.

## Hardware validation order

1. Cold boot and first full refresh on a newer-revision X4.
2. Fast black/white page turns.
3. Full refresh / ghost cleanup.
4. Grayscale images.
5. Sleep and wake.
6. Repeat the same checks on an older X4 before considering merge into the stable branch.
