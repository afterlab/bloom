# Gaussian Blur and Transform 2D extras

## Gaussian Blur: separate axes and per-channel radius

**Radius** blurs the image by that many pixels on both axes. Turn on **Separate axes** to blur the two axes by different
amounts: **Radius** is then the horizontal radius and **Vertical radius** the vertical one. A radius of 0 on one axis
leaves that axis sharp, so a motion-like horizontal smear is **Radius** above 0 with **Vertical radius** at 0.

Turn on **Per-channel radius** to blur each of red, green, blue and alpha by its own share of the radius: **Red radius**,
**Green radius**, **Blue radius** and **Alpha radius** are fractions of **Radius** (and of **Vertical radius** with separate
axes) from 0 to 100%, 100% by default. A blurred red and a sharp green and blue, for example, fringes an edge with color.
Per-channel radius costs a blur for each channel plus one more, so use it where the effect is wanted.

**Edges** (Transparent or Clamp) repeats the edge pixels with Clamp, as before. Documents made before these controls open
unchanged as a plain blur.

## Transform 2D: skew and opacity

**Skew** slants the image by that many degrees (up to 89 either way) along the **Skew axis**: with an axis of 0 the
verticals lean, with 90 the horizontals do. It applies before scale and rotation and leaves the pivot where it is.
**Opacity** fades the whole image from 100% to 0. Both are animatable and each has a socket.

Skew and opacity work with the Transform 2D **Motion blur** off. With motion blur on, a skew or an opacity other than
the defaults stops the compile with a message; turn the motion blur off or set them back.
