# Four-Color Gradient

The Four-Color Gradient source (Sources, search "Four Point Gradient") fills the composition with four colors
anchored at four points. Every pixel is a blend of the four colors, weighted by how close the pixel is to each
point: at a point the gradient is exactly that point's color.

- **Point 1 to Point 4** are positions in pixels from the center of the frame, with +y up. Drag them on the
  card's pad or type values. A new node places its four points at the corners of the composition, whatever its size (Point 1 top left, 2 top
  right, 3 bottom left, 4 bottom right), and keeps them there until you move them.
- **Color 1 to Color 4** are the colors at the points, in the working space. Alpha is kept, so a transparent
  color fades the picture out around its point.
- **Blend (sharpness)** from 0.25 to 8, default 2. Low values mix softly across the whole frame; high values
  keep each color near its point with narrow transitions between them.

Each pixel's weights are measured against its nearest point, so the picture stays smooth and correct however far
the points are from the frame and however high Blend is.

Everything is animatable. The result is the same on the CPU reference, the viewer and export, and at a
reduced render scale.
