# Color tools

Per-pixel color nodes for HDR scene-linear work. Each has an Image in, **Input**; the formulas and
their limits are in [Image filters](../architecture/image-filters.md).

## Tone Map

**Tone Map** keeps highlights from clipping, lifts or darkens shadows and, if you ask, pulls colors
that fell outside the gamut back inside. Find it under **Color Correction** in the Add menu. It works
on the straight color of scene-linear values and never changes alpha.

- **Roll-off range** is the **Knee** and the **Ceiling** on one track. A value below the knee is left
  alone. Above it the value bends smoothly toward the ceiling and never reaches it, so a highlight of
  any brightness keeps some detail instead of flattening to white. Put the ceiling at or below the
  knee for a hard clip at the ceiling. The defaults (0.8 and 1) tame a display-referred highlight; for
  an HDR peak raise both, for example a knee of 4 and a ceiling of 8.
- **Roll off by** chooses **Each channel**, which bends red, green and blue on their own (a very bright
  color drifts toward white), or **Brightest channel**, which bends the brightest channel and moves the
  others by the same factor, so a bright orange stays orange.
- **Shadow recovery** multiplies dark values: at 1 the darkest values double, at -1 they go toward
  black. **Shadow range** is how bright a value is when the effect has halved; a small range reaches
  only the darkest pixels. Black stays black.
- **Compress gamut** pulls a color with a negative channel back toward the edge of the gamut, which
  clean-up of a wide-gamut render or a grade that overshot often needs. Colors that are already
  inside stay as they are. **Gamut threshold** is how far a color may be from the brightest channel
  before it is touched, and **Gamut limit** the distance that lands exactly on the edge.

Every value can be keyed or driven through its socket. Connect a Mask to **Mask** to limit the node
to part of the image.

## HSL Qualifier

**HSL Qualifier** turns an image into a Mask of the pixels in a range of hue, saturation and
brightness, to correct only those colors. Find it under **Keying & Mattes** in the Add menu. Connect
its Mask to another node's **Mask** input. The formulas are in
[Image filters](../architecture/image-filters.md#hsl-qualifier).

- **Hue** is the middle of the colors to select, in degrees round the color wheel (0 red, 120 green,
  240 blue), and **Hue width** how wide the window is; the window wraps past 360. **Hue softness**
  feathers its edge.
- **Saturation from** and **to**, and **Luma from** and **to**, limit the selection to a range of
  colorfulness and brightness. Luma can go above 1 to reach bright HDR values. Each has a softness.
- **Invert selection** selects everything else. The Mask is as strong as the pixel is opaque.
- To shift or scale hues without selecting, use the hue curves in Curves; the qualifier only selects.

## Color Matrix

**Color Matrix** mixes the red, green and blue of an image with a matrix, and converts between color
models. Find it under **Color Correction** in the Add menu. The formulas are in
[Image filters](../architecture/image-filters.md#color-matrix).

- **Matrix size** is 3 x 3, which mixes color and leaves alpha alone, or 4 x 4, which also lets alpha
  feed the color and color make alpha (a luminance matte from an image, for example). Each element is
  named for what it does: **Red from green** is how much of the green goes into the red result. The
  identity (1 on the diagonal) changes nothing; a row of three equal luminance weights gives gray.
- **Preset** replaces the elements with Gray (luminance), a swap of two channels or Sepia. Choose
  **Custom matrix** to use the elements again; with a preset chosen the elements are not read.
- **Read input as** and **Write result as** convert color models. To change only a hue, write the result as
  HSV, edit it, and read it back as HSV with a second Color Matrix. HSV, YCbCr and Lab keep values above
  1; HSL works on 0 to 1. Hue is a fraction of a turn; Lab's channels are divided by 100.
- Color is mixed as straight color, so a soft edge keeps its color. A pixel with alpha 0 is not changed.

Every element can be keyed or driven through its socket.
