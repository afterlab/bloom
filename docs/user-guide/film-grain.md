# Film Grain

**Film Grain** adds seeded, repeatable grain to an image. Find it under **Stylize** in the Add menu.
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
- **Monochrome** is off by default. Turn it on and one grain pattern is shared by all three channels,
  so the grain carries no colour noise. The red, green and blue amounts keep scaling that one pattern:
  with all three equal the grain is neutral, with them unequal it is tinted but still one pattern,
  and a channel at 0% still keeps its value exactly. Each channel keeps the strength it had, so the
  channels no longer partly cancel in the brightness: at the same **Intensity** Monochrome grain looks
  about a third stronger. Lower **Intensity** to match.

## Mode

**Mode** picks how the grain is drawn. **Film** is the grain described here and below. **Sensor Noise**
draws the noise of a small camera sensor instead (see the next section). Switching mode changes which
rows the card shows, and keeps every value: switch back and nothing is lost. A project saved before the
mode existed opens in Film and renders as it did.

## Response to brightness (Film)

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

## Sensor Noise

Choose **Sensor Noise** under **Mode** for the look of a phone camera: noise that is strong in the
shadows, soft colored blotches, and, if you want it, a pattern that never changes. It uses **Intensity**,
**Size**, the three **amounts**, **Monochrome**, **Seed** and **Grain changes** as above, and replaces
the three response rows with:

- **Shadow weighting** is how much the noise favors the shadows. At 100% the noise is half as strong at
  mid gray as at black and falls further toward white; at 0% it is even across the picture.
- **Chroma blotches** is the strength of soft, low-frequency color blotches, relative to **Intensity**.
  They shift color and not brightness. 0% turns them off.
- **Blotch size** is how wide a blotch is, in full-resolution pixels.
- **Fixed pattern** is the strength of a noise pattern that is the same on every frame, as a sensor's
  own pixel-to-pixel differences are. It follows **Seed** but never **Grain changes**: with **Every
  Frame** the grain and blotches change and the pattern stays. It is off at 0%, the default.

The graph shows the shadow weighting over the ramp. As with Film, a different **Seed** gives different
noise, the noise is added in proportion to alpha, and nothing clips.

## Seed and time

- **Seed** picks which grain. The same seed and frame always give the same grain, on the CPU and the
  GPU, in preview and in a final render, in any order, and from one release to the next.
- **Grain changes** is **Every Frame** by default: each frame has its own grain, unrelated to the
  frames next to it, so nothing swims. **Held** keeps one pattern for the whole shot, for a texture or
  for a still.

Under a retimed layer the frame is the layer's own.

## Limiting it

Connect a Mask to **Mask** to keep the grain to part of the image, or an image read through **Mask
channel**. Every value but **Mode**, **Seed**, **Grain changes** and **Monochrome** can be keyed or driven
through its socket.

## Cost

Grain costs about as much as a Grade or a Vignette, whatever its size. See
[Benchmarks](../development/benchmarks.md#film-grain-486).
