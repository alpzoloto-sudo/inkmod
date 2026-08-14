# inkMOD v1.1.4

A stability-focused release for Xteink X3/X4.

## What changed

- Large EPUBs now open through the direct BMC/ERS reader pipeline; the experimental runtime EPUB pre-splitter is disabled after it was isolated as the source of release-only reboots.
- Large FB2/FB2.ZIP indexing uses a compact pooled section index and safer sequential ZIP handling to reduce heap pressure.
- Release builds keep an allocation-free RTC breadcrumb trail. If an OOM/guarded restart happens, the next boot writes the last reader actions to `/crash_report.txt`.
- New **Diagnostics** screen shows firmware variant, device, free heap, largest allocatable block, SD capacity, reset reason and crash-report presence.
- New **Book information** action in File Browser gives a lightweight format/size/cache/load-profile view without deeply parsing the book.
- Automatic timeout sleep can use a different screen from manual sleep: same as normal, Quick Resume, custom overlay + Quick Resume, or custom image.
- Lyra Carousel uses symmetric edge margins with seven icons.
- Cover Mode and Cover Filter remain visible in sleep-screen settings.
- Easter-egg prompt no longer repeats button labels already shown by the hardware hint bar.
- X4 keeps safe low-frequency idle behavior without the light-sleep path that could freeze after USB disconnect.

## Release engineering

GitHub Actions now builds and publishes the real `release` environment as:

`firmware-release-v1.1.4.bin`

The release workflow updates both version keys before building, and CI validates both developer and release variants.

## Support

InkMOD remains free. Optional project support: https://t.me/inkmodx4
