# Sharpen

**Sharpen** is an unsharp mask: it finds the image's detail, the difference between the image and a
blur of it, and adds that back. Find it under **Filters** in the Add menu. The formulas and their
limits are in [Image filters, "Sharpen"](../architecture/image-filters.md#sharpen).

Connect your picture to **Input**. Alpha never changes, and a transparent pixel stays transparent. A
partly transparent picture is sharpened by its colour alone, so the edge of a matte does not get a halo
of its own.

## The controls

The graph on the card draws an edge as the node sharpens it: the thin line is the edge going in, the
bright curve the edge coming out. Drag the handle on the overshoot up and down to change the amount,
and the handle where the halo ends sideways to change the radius. Double-click a handle to reset it.

- **Amount** is how much of the detail is added back: at 100% it is added once. Above about 300% edges
  ring visibly. 0% changes nothing.
- **Radius** is how far from an edge the sharpening reaches, in full-resolution pixels, so a proxy
  looks the same as the full frame. It is the same radius as Gaussian Blur's: a sigma of radius / 3.
  Keep it to a few pixels to sharpen detail; a wide radius with a small amount lifts local contrast
  instead. 0 changes nothing.
- **Threshold** leaves detail smaller than it alone, in linear light, so grain and flat areas stay
  clean. Larger detail starts from the threshold, so there is no jump where it takes over.

## HDR

Nothing clips at the top: a highlight's overshoot keeps its range, and the result scales with the
exposure, so sharpening before or after a gain gives the same picture (a Threshold is a fixed amount of
light, so it is the one control that does not scale). A dark halo stops at black rather than going
negative; a value already below black is left where it is. A very bright highlight next to a dim area
can still leave a dark ring, because the difference between them is large: lower the amount or the
radius, or keep the node away from it with a mask.

## Limiting it

Connect a Mask to **Mask** to keep the sharpening to part of the image, or an image read through **Mask
channel**. Every value can be keyed or driven through its socket.

## Cost

Sharpen costs a Gaussian Blur of the same radius and one more pass. See
[Benchmarks](../development/benchmarks.md#sharpen-486).
