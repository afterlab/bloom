# Inner Shadow and Inner Glow

**Inner Shadow** darkens the inside of a shape along its edges, as if a hole were cut into the
picture. **Inner Glow** lights the inside of a shape along its edges, or from its middle. Find both
under **Light & Shadow** in the Add menu. Both use the picture's transparency as the shape, so they
suit titles, buttons and interface elements. They never change the transparency: the shape keeps its
outline. The rules are in [Image filters](../architecture/image-filters.md#inner-shadow-and-inner-glow).

## Inner Shadow

- **Color** is the shadow's colour: click the swatch to pick one.
- **Opacity** is how strong it is.
- **Direction** is the way the light falls. Turn the dial: the shadow lies along the edges on that
  side. **Distance** is how far the shadow is pushed in, in pixels.
- **Blur** softens the shadow's edge, in pixels. **Choke** hardens it: at 0 the shadow fades smoothly
  from the edge, and at 100% it is a solid band with a crisp edge.
- **Shadow Only** shows just the shadow inside the shape, to use it as a separate layer.

## Inner Glow

- **Color** and **Opacity** as above. **Intensity** multiplies the colour, so a glow can be brighter
  than white.
- **Size** is how far the glow reaches into the shape, in pixels.
- **Source** chooses where the glow starts: **Edge** is brightest at the outline and fades inward;
  **Center** is brightest in the middle and fades toward the outline.
- **Choke** widens the fully lit band before the fade starts.
- **Glow Only** shows just the glow inside the shape.

## Tips

Sizes are in full-resolution pixels, so a half-size preview shows the same picture. The edge of the
frame counts as an edge: a shape that fills the frame gets a shadow or glow along the frame's border.
Every value can be keyed or driven from a node.
