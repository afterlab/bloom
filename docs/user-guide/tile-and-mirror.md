# Tile & Mirror

**Tile & Mirror** repeats a picture, scrolls it round, reflects it, or folds it into a kaleidoscope.
Find it under **Transform** in the Add menu. The rules are in
[Image filters](../architecture/image-filters.md#tile--mirror).

Pick the **Mode** first; the card and Properties show only the values that mode uses.

## Tile

Repeats a piece of the picture across the frame.

- **Tile Center** is the middle of the piece that is repeated. Drag it on the pad.
- **Tile Width** and **Tile Height** are the size of the piece, in percent of the frame. Turn on
  **Link Tile Size** to give both the width.
- **Mirror Edges** flips every second tile so neighbours meet without a jump.
- **Horizontal Phase** and **Vertical Phase** slide the tiles: 360 degrees is one whole tile, so keying a
  phase from 0 to 360 makes a seamless loop.
- **Output Width** and **Output Height** limit where the tiles show, in percent of the frame, when
  **Clamp to Output** is on. Turn it off to fill the whole frame.

## Wrap Offset

Moves the picture by **Offset** (pixels; drag the point on the pad) and brings what leaves one side back
in at the other. A whole-pixel offset moves the pixels exactly, with no seam.

## Mirror

Reflects one side of a line onto the other. Drag **Line Point** on the pad, turn **Line Angle** on the
dial (90 is vertical, 0 horizontal), and choose with **Reflect** which side is kept.

## Kaleidoscope

Folds the picture round **Center** into **Segments** wedges, turned by **Rotation** and magnified by
**Size** (percent).

## Small pictures

Tile and Kaleidoscope fill the whole frame, and they treat the middle of the picture as the middle of the
frame, so a small shape or text layer tiles into copies of itself and folds round its own middle.

## Both

**Filter** is how the picture is sampled (Bilinear, Nearest, Bicubic). **Edges** is what shows where the
picture does not reach (transparent by default; not used by Wrap Offset, which always wraps). All sizes
and offsets are in full-resolution pixels, so a half-scale preview shows the same picture.
