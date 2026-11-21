# Corner Pin, ST Map and Displace

Three nodes under **Distort** in the Add menu that move a picture by reading it from somewhere else.
The rules are in [Image filters, "Corner Pin, ST Map and Displace"](../architecture/image-filters.md#corner-pin-st-map-and-displace).

## Corner Pin

Connect a picture to **Image**. Drag the four corner points on the pad (or edit the values), and the
picture is pinned to that four-sided shape with perspective. Each corner is an offset in pixels from
the picture's own corner, so the node starts as the identity. Use it for screens, signs and
billboards; corners can be keyed or driven.

## ST Map

Connect a picture to **Image** and a map to **Map**. The map's **U** and **V** channels (red and green
by default) say where each output pixel reads, as a fraction of the frame: 0, 0 is the bottom left and
1, 1 the top right. This applies the **ST Map** output of Lens Distortion to another picture.

## Displace

Connect a picture and a map. Each pixel moves by `(value - midpoint) x amount` pixels, using the **X**
and **Y** channels (green and blue by default; **None** leaves an axis alone). With the default
midpoint of 0.5 a mid-grey map moves nothing.

## Choices

- **Filter**: Bilinear (smooth), Nearest (one exact texel; use it for ID and data pictures), Bicubic
  (sharper), Trilinear or Anisotropic (for a picture the map shrinks: they average the picture down to the size it
  is drawn at, so it stays smooth instead of shimmering; Anisotropic also copes with a stronger squash one way
  than the other), or Automatic (the default of a new node: Trilinear, or Nearest in a [pixel-art](pixel-art.md)
  composition). Trilinear and Anisotropic look the same as Bilinear where the picture is not shrunk. Nodes from
  earlier projects keep the filter they had.
- **Edges** (ST Map, Displace): what is read beyond the picture: Transparent, Extend, Repeat or Mirror.

The map is used as raw data: nothing is clamped to 0 to 1 and no colour conversion is applied. The
picture's colour, values above one and transparency are kept. With no map connected, ST Map and Displace
pass the picture through.

## Limits

- Corner Pin cannot show a quad folded through its horizon (corners that cross); that part is transparent.
- Cost is one pass per node; see [Benchmarks](../development/benchmarks.md#corner-pin-st-map-and-displace-348).
