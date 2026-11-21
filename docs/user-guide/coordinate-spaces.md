# Coordinate spaces

A **coordinate space** is a named set of units placed on the frame: "x from -5 to 5, y from -3 to 3,
drawn here at this size". Everything you draw or place in one space reads the same placement, so a
plot, its axes, its labels and the layers placed on it stay aligned when you move, scale or turn the
space, even while it animates. The rules are in
[Coordinate Spaces](../architecture/coordinate-spaces.md).

Find the nodes under **Vector** in the Add menu. A space travels along links into square sockets
labelled **Space**; a space input has nothing to type into, only a socket.

## Defining a space

Add a **Coordinate Space** node and give it a **Name** (`graph` to start with). Pick the **Mode**;
the card and Properties show only what that mode uses.

- **Origin and scale**: **Origin** is the pixel where (0, 0) lands, **Pixels Per Unit** how many
  pixels one unit is along each axis. **Rotation** turns the space about its origin (on the dial,
  clockwise), and **Y Up** makes a positive y rise, as on a graph.
- **Range to rectangle**: the range from **Range Min** to **Range Max** fills a rectangle of **Size**
  pixels centred on **Center**.
- **Range to corners**: the range's four corners land on **Top Left**, **Top Right**, **Bottom
  Right** and **Bottom Left**. The space is then a plane seen in perspective, so a grid drawn in it
  narrows towards the far edge.

Select the node and its points appear on the Viewer: the origin, the rectangle's centre or the four
corners, with the range's outline and one unit along each axis drawn through it. Drag a point to
move it; each drag is one undo step, and Escape keeps the drag as it stands.

Key **Origin** and **Pixels Per Unit** (or the centre and size, or the corners) to animate the space.

A space can be defined inside another: connect a space to **Parent**, and the new space's origin,
size and corners are in the parent's units. Moving the parent then moves both.

## Drawing and placing things in a space

- **Function Plot**, **Axes** and **Axis Tick** each have a **Space** input. Connect the same space to
  all of them and the curve, the axes, the ticks and the label positions agree, whatever the space
  does. Without a space they use their own Origin and Pixels Per Unit, as before.
- Draw a plot or axes as an entry of a Merge (Shape, with its Path driven by the plot or the axes)
  so its path's pixels are the frame's pixels.
- To place a layer at a point of the space, add **Place in Space**, connect the space, set
  **Point** in the space's units, and link its **Position**, **Scale** and **Rotation** outputs to
  the layer's. **Offset** nudges the layer in pixels (a label just below its mark), and **Unit Size**
  is how many pixels one unit is when the layer should be at 100%.

## Reading a space as numbers

**Space Readout** turns a space into values you can wire into anything: where its origin is, the
pixel step of one unit along each axis, the scale and rotation, the range, its 3x3 matrix as a
Number List, its name and its parent's name, whether it has perspective, and whether it is valid.

**To Space** reads a pixel as a point of the space, and **From Space** puts a point of the space on
its pixel. Set **Conversion** to **Vector** to convert a direction or a distance rather than a
position: it turns and scales with the space but does not move with its origin.

## When a space is refused

A Coordinate Space answers no space, and its card says why, when its range does not run forward, a
number is not finite or is huge, three of its corners are on one line, it has no size along an axis,
its name is very long, or it is nested more than sixteen spaces deep. Everything reading it then
falls back as if nothing were connected: a plot uses its own placement, and a layer placed in the
space stays at its offset.

## From a script or an agent

Build a space like any node: add `bloom.coordinate-space`, set its parameters, and connect its
`space` output to each `space` input with `bloom.node.connect`. See
[Scripting Bloom with Python](python.md) and [Using Bloom from an agent](mcp.md).
