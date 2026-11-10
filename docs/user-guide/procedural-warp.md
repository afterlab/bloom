# Procedural Warp

**Procedural Warp** bends the picture by a formula: twirl, bulge or pinch, a glass sphere, ripples,
waves, a polar wrap or a magnifying loupe. Find it under **Distort** in the Add menu. The rules are in
[Image filters](../architecture/image-filters.md#procedural-warp).

Pick the **Mode** first; the card and Properties show only the values that mode uses. Every mode leaves
the picture as it is at zero strength, so you can key the strength up from nothing.

## Twirl

Turns the picture round **Center** (drag it on the pad), most at the centre and not at all at
**Radius**. **Angle** is how far the middle turns, on the dial: positive is clockwise, and it can go past
a full turn. **Falloff** is how gradually the turn fades towards the radius: 100% fades over the whole
radius, 0% keeps the full turn right up to the edge.

## Bulge / Pinch

Swells the picture out from **Center** (a positive **Amount**) or pinches it in (negative), inside
**Radius**, fading by **Falloff** as Twirl does.

## Spherize / Lens

Looks through a glass ball placed on the picture: the middle is magnified and the picture squeezes
towards the rim. **Refraction Index** is how strongly the glass bends light (1.5 is ordinary glass; 1 is
no glass at all). **Amount** blends from the plain picture (0) to the full lens (100%); a negative amount
does the opposite, as if the picture were painted onto the ball.

## Ripple

Rings spread out from **Center** like a stone dropped in water. **Wavelength** is the distance between
rings, **Amplitude** how far the picture moves (pixels), and **Falloff** fades them towards **Radius**.
**Phase** slides the rings (360 degrees is one ring); set **Speed** in cycles a second to make them travel
on their own.

## Wave

Waves across the whole frame. **Wave** is the shape (Sine, Triangle, Square, Saw), **Direction** the way
the waves travel (on the dial, clockwise from up; 90 travels right), and **Wavelength**, **Amplitude**,
**Phase** and **Speed** work as for Ripple. **Pin Edges** holds edges of the frame still so the picture
does not pull away from them.

## Polar Coordinates

**Rectangular to Polar** wraps the frame into a disc round its centre: the top of the frame becomes the
middle, the bottom the outside, and the left and right edges meet at the top. **Polar to Rectangular**
unwraps a disc back into the frame. **Interpolation** blends from the plain picture (0%) to the full
conversion (100%).

## Magnify

A loupe at **Center**: inside the circle or square of **Radius** the picture is enlarged by
**Magnification** (200% is twice the size). **Grow with Magnification** makes the loupe itself grow as you
magnify (Size), or the loupe and its soft edge (Size and Feather). **Feather** softens the edge inside
the loupe.

## Every mode

**Filter** is how the picture is sampled (Bilinear, Nearest, Bicubic). **Edges** is what shows where a
mode reads past the picture (transparent by default, or the edge pixels extended, repeated or mirrored).
All lengths are in full-resolution pixels, so a half-scale preview shows the same picture.
