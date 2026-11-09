# Denoise & Smooth

Denoise & Smooth (Blur & Sharpen, search "denoise", "median", "bilateral" or "despeckle") removes noise,
dust and scratches. Pick a mode:

- **Median** replaces each value with the middle of its neighbors. With Strength 0 it is a plain median,
  which also softens fine detail. Raise Strength and only values that differ from their neighbors by more
  than that are replaced, which cleans dust and scratches and leaves the rest alone.
- **Bilateral** blurs, but only with neighbors of a similar color, so edges stay sharp. Strength is how
  different a neighbor may be and still count.
- **Spatial** compares small patches, not single pixels, so it keeps texture and fine repeating detail
  better than Bilateral on grainy footage. It is the slowest mode.
- **Temporal** averages the current frame with the frames before it (Frames, 2 to 16). Static areas get
  cleaner by the square root of the frame count; anything that moves leaves a trail, so use it on locked-off
  shots.

**Radius** is how far, in pixels, each pixel looks (0 to 6). It is relative to the full-size picture, so a
preview at reduced size looks the same. **Strength** is in the picture's linear values; a noisy plate
usually needs 0.02 to 0.2.

**Measuring the noise.** Crop a flat area of the plate (a wall, a sky), connect it to an Image Statistics
node and connect its Deviation output to Strength. Multiply it by 2 or 3 with a Math node if the grain
still shows. The strength then follows the footage.

A mask limits the effect to part of the picture. Radius and Strength can be animated and driven.
