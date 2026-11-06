# Film Grain

**Film Grain** adds seeded, repeatable grain to an image. Find it under **Filters** in the Add menu.
The formulas and their limits are in
[Image filters, "Film Grain"](../architecture/image-filters.md#film-grain).

Connect your picture to **Input**. The grain is added to the colour in proportion to its alpha, so a
partly transparent picture takes its share and a transparent one none, and alpha never changes.
Nothing clips: HDR values keep their range, and grain may take a dark value slightly below 0.

## The grain

- **Intensity** is how strong the grain is, in linear light, where the response is 100%.
- **Size** is how wide a grain is, in full-resolution pixels. A proxy shows the same grain at the
  same place, and a wide pixel still shows a round grain.
- **Red amount**, **Green amount** and **Blue amount** set each channel's own strength. Each channel
  has its own grain; on real stock blue is usually the strongest. A channel at 0% keeps its value
  exactly.

## Response to brightness

How much grain a pixel takes depends on its own brightness (Rec. 709 luma, with its alpha taken out).
The graph on the card shows this over a ramp from black to white, with the grain drawn on it. Drag its
three handles up and down, or use the rows below it:

| Value | The share of the intensity at |
|---|---|
| **Shadows response** | black |
| **Midtones response** | mid grey (0.18) |
| **Highlights response** | white (1.0) |

Between them the share runs in a straight line. Above white it grows with the brightness, so an HDR
highlight keeps the same share of grain instead of going clean. Double-click a handle to reset it.

## Seed and time

- **Seed** picks which grain. The same seed and frame always give the same grain, on the CPU and the
  GPU, in preview and in a final render, in any order, and from one release to the next.
- **Grain changes** is **Every Frame** by default: each frame has its own grain, unrelated to the
  frames next to it, so nothing swims. **Held** keeps one pattern for the whole shot, for a texture or
  for a still.

Under a retimed layer the frame is the layer's own.

## Limiting it

Connect a Mask to **Mask** to keep the grain to part of the image, or an image read through **Mask
channel**. Every value but **Seed** and **Grain changes** can be keyed or driven through its socket.

## Cost

Grain costs about as much as a Grade or a Vignette, whatever its size. See
[Benchmarks](../development/benchmarks.md#film-grain-486).
