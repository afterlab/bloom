# Lens Flare

**Lens Flare** adds the light a bright source scatters inside a lens: a stack of elements around the
light and along the line through the frame's centre. Find it under **Light & Shadow** in the Add menu.
The formulas and their limits are in
[Image filters, "Lens Flare"](../architecture/image-filters.md#lens-flare).

Connect your picture to **Input**. The flare is added to it as light, so an opaque picture stays
opaque, and the flare reaches the whole frame even if the input is smaller. On a transparent picture
the flare brings its own coverage, so you can also flare an empty layer and composite it later.

## Placing the light

Drag the light on the card's picture, which shows where each element lands, or type its **Light
position**: pixels from the centre of the frame.

- **Moving light:** key **Light position**, or connect any Vector 2 output to its socket, so the
  flare follows whatever drives it.
- **3D light:** set **Light is** to **3D position** under **Light source**. The position then has a
  **Light distance (Z)** behind the frame, and **Camera zoom** sets how strong the perspective is. A
  light that passes behind the camera shows no flare. Both values have sockets, so a projected 3D
  light can drive them.

## The whole flare

- **Preset** starts from a look: Zoom Lens, Prime Lens, Long Lens, Anamorphic, Sun Star or Soft Glow.
  It sets the element stack in one undo step and leaves the light's position, its intensity and its
  readings alone. Nothing stays linked to the preset.
- **Intensity** and **Light color** set the flare's brightness and tint. **Size** scales the whole
  stack; sizes are shares of the frame's height, so a flare looks the same at any resolution.
- **Rotation** turns the streaks, the anamorphic streak and the iris's polygon.
- **Chromatic split** separates the ghosts, the iris, the ring and the hoop into red and blue edges.

## The elements

The **Element** picker shows one part of the stack at a time. An element with amount 0 is off.

| Element | What it is | Its values |
|---|---|---|
| Glow | The bloom around the light | Amount, Size |
| Streaks | Needles through the light | Amount, Lines, Length, Width |
| Ghosts | Soft discs on the line through the frame's centre | Amount, Count, Size, Spread, Pattern |
| Ring | A thin ring around the light | Amount, Size, Width |
| Iris | Ghosts in the aperture's polygon | Amount, Count, Blades, Size, Spread |
| Hoop | A wide arc around the frame, away from the light | Amount, Size, Width |
| Anamorphic | One long thin streak in its own colour | Amount, Length, Width, Color |

**Ghost pattern** picks another arrangement of the ghosts and iris; the same number always gives the
same arrangement.

## Following the picture

Both of these are under **Light source**, and read a disc of **Light sample radius** pixels around
the light, so the flare fades as the light passes an edge instead of popping.

- **Brightness from source** scales the flare by how bright the input is under the light. At 100% a
  dark spot gives no flare; **Source white** is the brightness that gives the full flare. Use it to
  make a flare follow a flickering or passing light.
- **Occlusion** hides the flare by how much of a matte covers the light. Connect a Mask or an image
  to the **Occlusion** input, for example the roto of whatever passes in front of the light. For an
  image, **Occlusion channel** picks what covers it (alpha, a colour channel or luma, or any of them
  inverted, for a matte that marks where the light is visible); a Mask is read as its coverage, with
  only the invert applying.

## Cost

A flare costs about as much as a Grade or a Vignette and about a fifth of a Glow, whatever its counts
and sizes. Reading the source or a matte makes it up to about twice a Grade. See
[Benchmarks](../development/benchmarks.md#lens-flare-507).
