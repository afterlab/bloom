# Dither

**Dither** reduces the picture to a few levels per channel and adds a fine, even noise as it does,
so smooth gradients (skies, glows, vignettes, soft shadows) break into grain instead of visible
bands. With its defaults (4 bits, sixteen levels) the effect is plainly visible; at the depth the
picture will be saved at, such as 8 bits for a PNG or video, it is the classic dither that keeps
bands out of the delivery. Find it under **Stylize** in the Add menu; for a delivery, put it last,
just before the output. The rules are in [Image filters](../architecture/image-filters.md#dither).

The preview on the card shows the effect on a ramp cut to a few levels: banded on top, dithered
underneath.

## Settings

- **Pattern**: **Blue noise** (the default) spreads the noise evenly with no visible structure, the
  best choice for almost everything. **Ordered (Bayer)** is a regular cross-hatch, the look of
  classic ordered dithering; **Matrix size** (2 x 2 to 16 x 16) sets how fine it is.
- **Target bit depth**: how many levels each channel keeps: 1 to 6 bits (2 to 64 levels) for a
  visible, graphic look, or the depth the picture will be saved at (8, 10, 12 or 16 bits). The
  default is 4 bits. The amplitude is measured in its steps.
- **Amplitude**: how strong the noise is, in steps (LSB) of the target bit depth, peak to peak. The
  default of 1 LSB is just enough to hide the bands; go to 2 to 4 for a stronger texture. 0 keeps
  the levels without any noise, so the bands show. The line under the slider shows what the
  amplitude is as a share of full scale. It can be keyed.
- **Measured in**: **Display (sRGB)** (the default) measures the steps and the levels as a
  display-encoded file counts them, which is right for ordinary 8-bit deliverables. **Stored value** measures them in the
  project's working values, for a linear or log file.
- **Pattern changes**: **Every Frame** moves the pattern on each frame, so it averages out in motion.
  **Held** keeps the same pattern on every frame, for stills or when the noise must not shimmer.
- **Seed**: picks a different placement of the same pattern. **Randomize seed** chooses a new one.
- **Monochrome**: uses the same noise for red, green and blue, so the dither adds no colour.

Transparency is kept, and fully transparent pixels are left alone. The pattern is tied to the
full-size picture, so it looks the same in a region, at a lower preview resolution and in the export.
