# Gaussian Blur and Transform 2D extras

## Gaussian Blur: separate axes and per-channel radius

**Radius** blurs the image by that many pixels on both axes. Turn on **Separate axes** to blur the two axes by different
amounts: **Radius** is then the horizontal radius and **Vertical radius** the vertical one. A radius of 0 on one axis
leaves that axis sharp, so a motion-like horizontal smear is **Radius** above 0 with **Vertical radius** at 0.

Turn on **Per-channel radius** to blur each of red, green, blue and alpha by its own share of the radius: **Red radius**,
**Green radius**, **Blue radius** and **Alpha radius** are fractions of **Radius** (and of **Vertical radius** with separate
axes) from 0 to 100%, 100% by default. A blurred red and a sharp green and blue, for example, fringes an edge with color.
Per-channel radius costs a blur for each channel plus one more, so use it where the effect is wanted.

**Vertical radius** shows only while **Separate axes** is on, and the four channel radii only while **Per-channel
radius** is on: a row that would do nothing is not shown. The same goes for the Timeline's property rows.

**Edges** (Transparent or Clamp) repeats the edge pixels with Clamp, as before. Documents made before these controls open
unchanged as a plain blur.

## Transform 2D: skew and opacity

**Skew** slants the image by that many degrees (up to 89 either way) along the **Skew axis**: with an axis of 0 the
verticals lean, with 90 the horizontals do. It applies before scale and rotation and leaves the pivot where it is.
**Opacity** fades the whole image from 100% to 0. Both are animatable and each has a socket.

Skew and opacity work with the Transform 2D **Motion blur** on or off. With motion blur on, each point of the shutter
is resampled with the skew and the opacity the animation has at that moment, so a skew that animates smears along
its shear and an opacity that fades through the shutter fades the blur with it.

## Transform 2D, Resize and Fit: Filter

**Filter** is how the picture is sampled when it is moved, rotated or resized.

- **Bilinear** blends the four nearest pixels. It is smooth when the picture stays the same size or grows, but a picture
  shrunk by half or more shimmers and shows jagged edges, especially when it moves.
- **Nearest** takes each pixel from one source pixel, so it looks blocky when enlarged and never blends values. Use it
  for depth, ID and other data pictures.
- **Trilinear** averages the source down to the size it is being drawn at first, so a shrunk picture is smooth and stable
  while it moves. Use it for pictures that shrink. When the picture does not shrink it looks the same as Bilinear.
- **Anisotropic** is Trilinear that also copes with a picture shrunk more in one direction than the other, such as a
  receding floor or a strong squash. It keeps detail along the long direction instead of blurring it, and costs a little
  more. When the picture does not shrink it looks the same as Bilinear.
- **Automatic** (the default for a new node) lets Bloom choose; today it uses Trilinear.

Projects made before these choices keep the Bilinear or Nearest they had and render exactly as before. With Transform 2D's
**Motion blur** on, each point of the shutter is sampled with the same filter. Layers have the same **Filter** choice for
their own transform; a new layer uses Automatic and a layer from an older project stays Bilinear.
