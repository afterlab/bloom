# Dither

**Dither** adds a fine, even noise to the picture so that smooth gradients (skies, glows, vignettes,
soft shadows) do not turn into visible bands when the result is saved at a lower bit depth, such as an
8-bit PNG or video. Find it under **Stylize** in the Add menu and put it last, just before the output.
The rules are in [Image filters](../architecture/image-filters.md#dither).

The node itself does not reduce the bit depth: it adds the noise that the export's rounding needs to
break the bands up. The preview on the card shows the effect on a ramp cut to a few levels: banded
on top, dithered underneath.

## Settings

- **Pattern**: **Blue noise** (the default) spreads the noise evenly with no visible structure, the
  best choice for almost everything. **Ordered (Bayer)** is a regular cross-hatch, the look of
  classic ordered dithering; **Matrix size** (2 x 2 to 16 x 16) sets how fine it is.
- **Target bit depth**: the depth the picture will be saved at (8, 10, 12 or 16 bits). The amplitude
  is measured in its steps.
- **Amplitude**: how strong the noise is, in steps (LSB) of the target bit depth, peak to peak. The
  default of 1 LSB is just enough to hide the bands; go to 2 to 4 for a stronger, visible texture.
  0 turns the node off. The line under the slider shows what the amplitude is as a share of full
  scale. It can be keyed.
- **Measured in**: **Display (sRGB)** (the default) measures the steps as a display-encoded file
  counts them, which is right for ordinary 8-bit deliverables. **Stored value** measures them in the
  project's working values, for a linear or log file.
- **Pattern changes**: **Every Frame** moves the pattern on each frame, so it averages out in motion.
  **Held** keeps the same pattern on every frame, for stills or when the noise must not shimmer.
- **Seed**: picks a different placement of the same pattern. **Randomize seed** chooses a new one.
- **Monochrome**: uses the same noise for red, green and blue, so the dither adds no colour.

Transparency is kept, and fully transparent pixels are left alone. The pattern is tied to the
full-size picture, so it looks the same in a region, at a lower preview resolution and in the export.
