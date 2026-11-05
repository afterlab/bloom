# Fields: noise, patterns and procedural drivers

Fields are procedural pictures that are not images yet: noise, cells, stripes, distances from a
shape, forces. A field is a function of a position in the composition, so it has no resolution and
no edges. Fields connect with diamond sockets; an image or mask socket never accepts one directly.

Find them under **Fields** in the Add menu.

## Getting pixels out of a field

- Every generator (Noise, Voronoi, Random Field, Pattern, Gradient Field, Shape Distance) has its
  own **image** and **mask** outputs. Connect `image` to a layer, Glow, Colorize or any image input;
  connect `mask` to Image Mix's mask or a matte input. **Colorize** takes a field on its Input as
  well, so a Gradient Map colours a field directly.
- **Rasterize Field** turns any field into an image or a mask. Choose the **Output** (Raw Data,
  Mask, or Colour through the **Ramp**), the **Extent** (the whole composition, an input image's
  area, or an explicit rectangle) and **Sub-samples** for smoother edges. **Inspect** shows a field
  as a heatmap, arrows or streamlines, which is the quickest way to see a vector field.
- **Field Sample** reads a field at one position as numbers. Link its outputs to any parameter to
  drive it: a layer's opacity from noise at a point, a position wiggled by a vector field.

## Building fields

- **Noise**: Perlin, Simplex and value noise (Linear, Soft Linear, Block, Spline) with fractal
  sums. Its outputs are the value, a two-channel vector, the gradient and the curl (a swirling,
  divergence-free flow for particles and warps).
- **Voronoi**: cells, with the distance to the nearest point (F1), the second (F2), their
  difference, crisp edges or smooth cells; plus each cell's id and position.
- **Random Field**: a value per cell, stepping or crossfading as Evolution moves.
- **Pattern**: stripes, checker, rings, dots, grid lines, hexagons and waves with anti-aliased edges;
  **Speed** animates the phase on its own.
- **Gradient Field**: a linear, radial, angular or diamond ramp between two points.
- **Shape Distance**: the signed distance to a circle, box, rounded box, segment, polygon, star or
  ring, or to the edge of a mask, with a falloff.
- **Image Field**: reads an image as a field (a channel, a pair of channels such as motion vectors,
  or its colour); connect a vector field to **position** to look it up elsewhere, like an ST map.
- **Force Field**: vortex, radial or uniform forces with a falloff (including a drawn curve).

Combine them with **Field Math**, **Field Remap** (ranges, steps, bias and gain, a curve), **Field
Transform** (move, turn, scale, warp by another field, polar, tiled, mirrored or kaleidoscope
coordinates), **Field Derivative** (gradient, curl, divergence) and **Combine Field** / **Separate
Field**. Every field node feeding one raster runs as one program, so a long chain costs no extra
images.

## Noise controls for users of fractal noise

The Noise node carries every control of the fractal noise effect in a layer-based compositor, under
the same names where they differ:

| You know it as | Noise control |
| --- | --- |
| Fractal Type Basic / Turbulent Basic / Turbulent Smooth / Turbulent Sharp | Fractal type fBm (Basic) / Turbulence (Turbulent Basic) / Billow (Turbulent Smooth) / Ridged (Turbulent Sharp) |
| Dynamic / Dynamic Progressive / Dynamic Twist | the fractal types of the same names |
| Noise Type Block / Linear / Soft Linear / Spline | Noise type Block / Linear / Soft Linear / Spline |
| Invert, Contrast, Brightness | Invert, Contrast, Brightness |
| Overflow Clip / Soft Clamp / Wrap Back / Allow HDR Results | Overflow, the same four (Allow HDR Results is the default) |
| Complexity | Complexity (octaves); fractional values blend the last octave in |
| Sub Influence, Sub Scaling, Sub Rotation, Sub Offset | the same names (gain, lacunarity, octave rotation, octave offset) |
| Offset Turbulence | Offset (Offset Turbulence) |
| Evolution, Cycle Evolution, Cycle (in Revolutions) | Evolution (cycles), Cycle evolution, Cycle length |

Positions use the composition's centre as zero with +Y up, and angles turn counter-clockwise. The
Noise card draws the offset on a position pad and the rotations on dials.

## Animating

Every number and position on a field node can be keyed or linked like any other parameter. A noise
with **Cycle evolution** on repeats exactly after the cycle length (the composition's length when
it is 0), so it loops cleanly. A field that does not change over time is computed once and reused
on every frame of a RAM preview.

Fields render on the GPU in the Viewer, RAM preview and every export, with the CPU reference giving
the same picture.
