# Light Rays

**Light Rays** adds volumetric rays that radiate from a point, the shafts you get from a bright sun, a
window or a light source behind something. Bright (or opaque) parts of the picture streak away from the
point; the streaks are tinted, boosted and added on top of the picture. Find it under **Light & Shadow** in
the Add menu. The model and its limits are in
[Image filters, "Light Rays"](../architecture/image-filters.md#light-rays).

Connect your picture to **Input**; **Image** is the result.

## Controls

- **Center** is the point the rays radiate from, in pixels from the frame's center. Drag it on the pad. Put
  it on the light.
- **Source** chooses what makes the rays: **Luminance** streaks the bright parts of the picture,
  **Alpha** streaks whatever is opaque (use it to make rays from a cut-out shape).
- **Length** is how far each ray reaches towards the center, as a percentage of the distance to it. 100%
  reaches all the way; 0 adds no rays.
- **Falloff** is how quickly a ray fades along its length. 0 keeps it even, higher values fade it faster
  so the rays are strongest near their source.
- **Boost** is a multiplier on the rays' brightness. 0 adds no rays.
- **Tint** colors the rays. White leaves them the color of what they come from.

A new node starts with the center in the middle of the frame, **Luminance**, length 50%, falloff 1, boost
1.5 and a white tint. **Center**, **Length**, **Falloff** and **Boost** can be keyed or driven through their
sockets; the source and the tint are set per node.

## What it does to the picture

- The rays are added to the picture as light: values above one stay above one, and the picture itself is
  not changed, only brightened where rays fall.
- With **Alpha**, a transparent area behind an opaque shape gains opacity where the rays fall, so the
  rays show over a transparent background.
- Rays are made only from what is inside the picture; a light outside the frame casts none.

## Limits

- **Center** is measured from the composition's center in the input's own pixel space, as in the other
  centered nodes. An input that is smaller than the composition and has not been moved by a layer transform
  yet does not sit at the center, so the rays leave from where that input actually is, not where the layer
  later appears.
- It is one gather towards a point; it does not know about depth or shadows, so a shaft is not blocked by an
  object between the light and a pixel except through the picture's own brightness.
- It is a heavy node: each pixel takes up to 96 reads of the picture. See
  [Benchmarks](../development/benchmarks.md#light-rays-508).
- The CPU and GPU agree on the same input to the field kernel's rounding (see [Fields](../architecture/fields.md)).
