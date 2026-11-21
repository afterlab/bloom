# Pixel art

Pixel art stays crisp only when every pixel is copied whole: no blending between neighbors and no
half-pixel moves. A composition's **pixel-art mode** makes Bloom choose those settings for you.

## Turn it on

Pixel-art mode is a setting of the composition, off by default. Open **Composition > Composition Settings**, choose the
**Advanced** tab and check **Pixel Art**. A script, the Python API or an MCP client sets the same thing with
`bloom.composition.set-settings` and its `pixelArt` argument (see [Python](python.md) and [MCP](mcp.md)). It is one
undoable change, a duplicated composition keeps it, and a project saves it only while it is on.

## What it changes

- Every **Automatic** filter becomes **Nearest** instead of Trilinear: Layer, Transform 2D, Resize and Fit, and the warp
  nodes (Corner Pin, ST Map, Displace, Stabilize, Procedural Warp, Tile & Mirror).
- Every layer whose **Snap** is **Automatic** snaps to whole pixels.

Anything you set explicitly is left alone. A layer or node set to Bilinear, Trilinear or Anisotropic still uses it, and
a layer with Snap **Off** or **On** keeps that choice.

## Per-layer overrides

- **Snap** (Automatic, Off, On) is on every layer: in the Object group of Properties, in the layer's Transform rows in
  the Timeline and on the Layer node's card. A new layer is Automatic, so it follows the composition. A layer from an
  older project shows Off and does not snap until you choose a value. Scripts use `bloom.layer.set-snap`.
- **Filter** (on a layer, in the same places as Snap, and on Transform 2D, Resize, Fit and the warp nodes) overrides
  Automatic for that one item. Choose
  Nearest to force crisp pixels in a composition that is not in pixel-art mode, or Bilinear to smooth one layer in
  a composition that is.

## Snap and Snap to Pixels

Snap rounds where the layer ends up on screen, after any parent has moved it, to the nearest whole output pixel. A
sprite that only moves therefore copies its pixels exactly, and a moving sprite steps by whole pixels. Children follow
their parent's unrounded position, so snapping a parent does not shift them. At a proxy resolution the position
rounds to the proxy's pixels, so a half-size preview rounds to its own pixels.

Transform 2D has **Snap to Pixels** (off by default) for the same rounding on a node.

## Scale by whole numbers

Snap fixes position, not size. A sprite scaled by 200%, 300% or 400% with Nearest shows every source pixel as an exact
square. A fractional scale such as 150% shows some pixels wider than others, which no filter can avoid. Size the
composition to a whole multiple of the artwork's pixel size, and rotate by multiples of 90 degrees when you can.
