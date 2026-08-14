# Global InkMOD logo correction

What was fixed:
- Boot logo: pre-rotated 90° counter-clockwise so it appears upright on X4.
- Default InkMOD sleep logo: same correction.
- Portrait Calendar logo: same correction.
- Landscape Calendar compact logo source: same counter-rotation; its existing
  rotation-safe renderer then compensates correctly.
- Web UI logo replaced with the new full InkMOD logo.
- Web UI duplicate text `inkMOD` removed because it is already inside the image.
- Standalone duplicate InkMOD text under firmware logo removed wherever present.

The firmware logo bitmaps are 1-bit monochrome and optimized for the e-ink panel.
