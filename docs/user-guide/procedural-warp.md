# Procedural Warp

**Procedural Warp** bends the picture by a formula: twirl, bulge or pinch, a glass sphere, ripples,
waves, a polar wrap, a magnifying loupe, a bent or split frame, turbulent noise or raindrops. Find it under **Distort** in the Add menu. The rules are in
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

Magnify also has **Pull** and a **Pull Point** (drag it on the pad or in the Viewer): the loupe drags
the picture towards that point, most at the centre, so a pull of 100% moves the centre's picture all the
way to the Pull Point.

Ripple has **Pulse Radius** and **Pulse Speed** (pixels, and pixels a second): with either set the rings
only move inside a front that travels outward, so a single wave goes out and the water behind it is stirred.

## Warp

Bends the whole frame by one of thirteen **Styles**: Arc, Arc Lower, Arc Upper, Arch, Bulge, Flag, Wave,
Fish, Rise, Fisheye, Inflate, Squeeze and Twist. **Bend** is how far (negative bends the other way);
**Horizontal Distortion** and **Vertical Distortion** add a perspective, as if one side of the picture
were nearer. A bend of 0 with no distortion leaves the picture alone.

## Bend

Bows the picture along a **Direction** (on the dial) with a smooth bump about **Center**, **Height** pixels
wide: the picture is pushed along the direction by **Bend**, most at the centre line and not at all at the
edges of the bump.

## Split

Cuts the picture along the line from **Line Start** to **Line End** (drag both ends on the pad or in the
Viewer) and slides the two halves apart by **Split Distance**; the gap between them is transparent. Turn
on **Mirror** instead to reflect one side onto the other (a mirror line you can move with the two points).
Turn on **Asymmetric** to open each half by its own amount: **Split Distance** is then how far the half to
the left of the line (looking from Line Start to Line End) opens, and **Opposite Distance** how far the
half to its right opens. Equal distances are the ordinary split.

## Turbulent

Pushes the picture around with fractal noise, like heat haze or a flag in a gale. **Displacement** is how
far (pixels), **Noise Size** the size of the swirls (each axis), **Noise Offset** slides the pattern,
**Complexity** adds finer detail (octaves) and **Evolution** (degrees) makes it change over time, so key it
to animate. **Seed** picks another pattern, and **Pin Edges** holds the frame's edges still.

## Drizzle

Raindrops on a pond: up to 64 **Drops** land across the frame and each sends a ring out to **Radius**,
fading as it spreads. **Wavelength** and **Amplitude** shape the rings, **Phase** and **Speed** move them
through their life, and **Seed** scatters the drops somewhere else.

## Handles in the Viewer

With the node selected the Viewer shows its handles. Drag a point (the centre, the Pull Point, the line's
ends) to move it, or drag the **ring** to set the radius; the inner dashed ring of Twirl, Bulge / Pinch and
Ripple is where the falloff starts. Each drag is one Undo step, and Escape keeps the drag as it stands. On
the card, **Radius** and **Falloff** are one two-handle range bar.

## Every mode

**Filter** is how the picture is sampled (Bilinear, Nearest, Bicubic). **Edges** is what shows where a
mode reads past the picture (transparent by default, or the edge pixels extended, repeated or mirrored).
All lengths are in full-resolution pixels, so a half-scale preview shows the same picture.
