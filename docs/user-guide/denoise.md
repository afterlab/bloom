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
- **Temporal** averages the current frame with the frames before it (Frames, 2 to 16), comparing small
  patches of each earlier frame with the current one. Where the picture is the same, the frames are averaged
  and the noise drops by about the square root of the frame count; where something moved, the patches
  differ, that frame is left out and nothing smears. Strength is how different a patch may be and still
  count: too low leaves noise, too high lets moving detail trail. The matte (alpha) is always the current
  frame's.

Median, at a soft edge (a feathered matte, motion blur, antialiasing), treats the picture's color and coverage
separately: it picks the color from the neighbors that cover something and the coverage from all of them, so
the color of a soft edge stays the color of the subject instead of darkening or brightening with its coverage.

**Radius** (0 to 6, in pixels) shows in every mode but Temporal; **Frames** shows only in Temporal;
**Strength** shows while Auto strength is off. A row that would do nothing is hidden, and a key left on a
hidden value is ignored.

**Radius** is how far, in pixels, each pixel looks. It is relative to the full-size picture, so a
preview at reduced size looks the same. **Strength** is in the picture's linear values; a noisy plate
usually needs 0.02 to 0.2.

**Auto strength.** Turn on Auto strength and the node measures the noise for you: it looks at a sample
region of the incoming picture, measures how much the pixels vary there, and sets Strength from that for
each frame. Place the region on a flat area (a wall, a sky, a grey card) with **Sample corner 1** and
**Sample corner 2**, by dragging them on the node's pad or typing pixels from the centre of the
composition (up is positive). The region starts as a 64 x 64 square in the middle of the frame. Moving
it measures again. In the viewer a frame with Auto strength is computed on the CPU; renders and exports
measure on the GPU.

You can still measure by hand: Crop a flat area, connect it to an Image Statistics node and connect its
Deviation output to Strength, multiplied by 2 or 3 with a Math node.

A mask limits the effect to part of the picture. Radius and Strength can be animated and driven.
