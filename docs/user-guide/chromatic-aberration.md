# Chromatic Aberration

**Chromatic Aberration** pulls the red and blue of a picture apart, the coloured fringe a real lens leaves
at the edges of a frame. Red is shifted away from a centre and blue towards it, by an amount that grows
with the distance from that centre; green stays put. Find it under **Distort** in the Add menu. The model
and its limits are in [Image filters, "Chromatic Aberration"](../architecture/image-filters.md#chromatic-aberration).

Connect your picture to **Input**; **Image** is the result.

## Controls

- **Amount** is how far red and blue are pulled apart, as a percentage of half the frame's diagonal, reached at
  that distance from the centre (a corner of the frame, for a centred pad). Red moves outward and blue inward;
  below zero swaps them. The fringe is nothing at the centre and grows quickly towards the edges, so a small
  amount (a fraction of a percent to a few percent) is a subtle lens, a large one a deliberate colour split.
- **Center** is the point the colours spread from, in pixels from the frame's centre. Drag it on the pad. It
  is the middle of the frame unless the lens or the effect is meant to be off-centre.

**Amount starts at 0, which leaves the picture exactly as it is**, so a new node changes nothing until you
move it. Both values can be keyed or driven through their sockets. An amount means the same on a proxy as at
full size.

## What it does to the picture

- Values above one, negative values and partial transparency keep their range; the colour is not clipped or
  changed, only moved. A bright highlight gets a bright red edge on one side and a bright blue one on the other.
- The picture is sampled smoothly (bilinear), so a large amount softens fine detail a little.
- Where a colour would be read from outside the picture it is transparent, so the very edge of the frame can
  show a thin fringe of one colour at a large amount.
- At the soft edge of a partly transparent object the fringe sits at the opacity of the widest of the three
  colours, so a colour is never cut off by a smaller opacity than the one it came with.

## Limits

- The offset is the same along the line from the centre for all three colours; it is not a model of any
  particular lens, and does not vary by wavelength beyond the red and blue pair.
- It costs three reads of the picture per pixel. See
  [Benchmarks](../development/benchmarks.md#chromatic-aberration-486).
- The CPU and GPU agree on the same input to the field kernel's rounding (see [Fields](../architecture/fields.md)).
