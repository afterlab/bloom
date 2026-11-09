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
