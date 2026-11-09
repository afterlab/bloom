# Four-Color Gradient

The Four-Color Gradient source (Sources, search "Four Point Gradient") fills the composition with four colors
anchored at four points. Every pixel is a blend of the four colors, weighted by how close the pixel is to each
point: at a point the gradient is exactly that point's color.

- **Point 1 to Point 4** are positions in pixels from the center of the frame, with +y up. Drag them on the
  card's pad or type values. The defaults sit at the four corners of a 1920 by 1080 frame, pulled in halfway.
- **Color 1 to Color 4** are the colors at the points, in the working space. Alpha is kept, so a transparent
  color fades the picture out around its point.
- **Blend (sharpness)** from 0.25 to 8, default 2. Low values mix softly across the whole frame; high values
  keep each color near its point with narrow transitions between them.

Everything is animatable. The result is the same on the CPU reference, the viewer and export, and at a
reduced render scale.
