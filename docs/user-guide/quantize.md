# Quantize

**Quantize** cuts the picture into hard bands: a **Threshold** that makes it black and white, or a
**Posterize** that keeps only a few levels of each colour. Find it under **Stylize** in the Add menu.
The rules are in [Image filters](../architecture/image-filters.md#quantize).

Pick the **Mode** first; the card and Properties show only what that mode uses.

## Threshold

Everything brighter than the **Level** becomes white and everything darker black.

- **Threshold on** is **Luma** (the picture's brightness decides, so the result is a plain
  black-and-white shape) or **Per channel** (red, green and blue each decide, which gives eight colours).
- The **Threshold** strip shows the span on a black-to-white scale. Drag its handles: the middle of the
  span is the **Level** and its width is the **Softness**. With the handles together the edge is hard.
  Pull them apart for a soft ramp, centred on the level.
- **Invert** swaps white and black.
- Values are scene-linear: a level of 0.5 is half the scene's brightness, which looks brighter than
  half on screen. Try 0.2 for a display-like middle.
- Transparency is kept.

## Posterize

Snaps each colour to a few equally spaced levels.

- **Levels** is how many levels each colour keeps: 2 is the most extreme, 256 changes nothing you can see.
- **Link levels** gives red, green and blue the same number. Turn it off to set **Red levels**, **Green
  levels** and **Blue levels** one by one.
- **Posterize alpha** also snaps transparency to **Alpha levels**.
- Values above full white stay white and values below black stay black.

Level, softness and levels can all be keyed, so a posterize can step up through its levels or a
threshold can sweep across a shot.
