# Vector Artwork

Vector artwork is built from shapes, and a shape's outline is a path. This page says how to work
with paths and path values on the node canvas. The later sections cover importing an SVG, as one layer or as
editable shapes, and breaking a one-layer SVG apart.

## Keying a Path node

A Path node holds one whole path value: every contour and anchor together. Keying it animates the
whole outline, so a shape that reads the Path node morphs from one key to the next. A key holds the
entire path; there are no per-anchor keys.

You can key a Path node in three places, and all three are the same control and the same command:

- **On the node card.** The Path node's **Value** row carries a keyframe diamond beside the
  summary (for example `4 anchors`). Click it to key the path at the playhead.
- **In Properties.** Select the Path node and use the key diamond on its **Value** row.
- **In the timeline.** Open the node's property rows and use the key at the playhead on the Value
  row.

The diamond has three states, and it follows the playhead as you scrub or play:

| Diamond | Meaning | Click |
| --- | --- | --- |
| Dim, no fill | The path is constant | Adds a key at the playhead and starts animating |
| Gold outline | The path is animated and the playhead is between keys | Adds a key at the playhead holding the path you see |
| Gold fill | The playhead is on a key | Removes that key. Removing the last key leaves the path constant, holding that key's path |

Each click is one change and one undo step. While the path is animated, the card's summary shows the
path at the playhead (`4 anchors`) rather than a word such as "Animated". To change a key's outline,
move to that key and edit the path (in the Viewer with the anchor tools, or in Properties).

The same diamond appears on every card row that shows a keyable whole value: a Path node's Value,
and the gradient of a Linear or Radial Gradient. A row that another node drives
shows no diamond; key the driving node instead. A Shape Source's own **path** is not keyable: to
animate a shape's outline, add a Path node, key it, and connect it to the shape's **path** input.

## Importing an SVG

An SVG file imports in one of two modes. Both draw the same picture: gradients, clips, masks,
opacity, blur and text with the file's fonts, and both name what the file holds that Bloom does not
take.

| Mode | What you get | Use it for |
| --- | --- | --- |
| **As one layer** (the default) | One layer, with one Vector Artwork node in the graph. It stays crisp at any scale, rotation and render size, because it is drawn from the file's vector geometry at the output resolution, not from a picture made at import. The project keeps the file's bytes, so the layer renders, saves and reopens with the file gone | A logo or an icon you place, scale and animate as a whole |
| **As editable shapes** | A shape layer and a Path node for every element, groups kept as groups. You can move and key points, wire in Trim Path or Offset Path, and recolour one element | Artwork whose parts you animate. A complex file becomes many nodes, which is hard to read in the node editor |

### Choosing the mode

When you import SVG files with **File > Import**, or drop them on the Assets panel, the Timeline or
the Viewer, Bloom asks once for the whole batch: **As one layer** is selected, **As editable
shapes** is the deliberate choice, and **Cancel** imports none of the SVG files in the batch (other
files still import). Tick **Remember my choice** to stop being asked. The answer is stored as
**Preferences > Import > SVG files**, which you can set to **Ask each time**, **As one layer** or
**As editable shapes** at any time.

The Vector Artwork node's card and the layer's Properties show a read-only **Artwork** line with the
file's name and how much of it the project holds (`logo.svg · 1 KB held`). It changes only by
importing again.

### Reusing an imported SVG

An imported SVG stays in **Assets** as an SVG (Type **SVG**; its Dimensions tooltip says it is not a
pixel size). To use it again, add it to a composition the way you add any footage: drag it onto the
Viewer, the Timeline or the node editor, or choose **Add to Composition** in its Assets menu, or
**New Composition from Selection**. Every route builds the artwork through the SVG import, never an
image layer, with the same chooser (asked once per batch) and the same remembered default as the
first import, and it reuses the asset: no second asset record is made. Each add is one undo step.
In the node editor the artwork's nodes are built, unconnected, at the drop point.

The add uses the file if it is still there and unchanged. If the file is gone, the project's stored
copy is used, so it works without the file. If the file changed, the add is refused by name; use
**Reload Footage**. If there is neither a file nor a stored copy, the add is refused by name and
nothing is added (the rest of a mixed batch is refused with it). A plain image layer cannot take an
SVG asset: it is refused with the reason.

An empty image layer that an older Bloom made from an SVG asset shows a named warning on the layer
in the Timeline, in its node card and in Properties: "'logo.svg' is an SVG source, not footage: add
it again from Assets to rebuild the artwork". The same warning appears in the `layers` and `nodes`
queries of MCP and Python as `warnings`.

### Break Apart

**Composition > Break Apart SVG**, or **Break Apart SVG** in a layer's right-click menu in the
Timeline, turns a one-layer SVG into editable shapes in place, in one undo step. With several
one-layer SVGs selected it breaks all of them apart in one undo step. The layer keeps its
name, transform and keys, parent, timing, masks, track matte, opacity, blend mode and effects, and
now draws the shapes of the file's root group. Nothing is read from disk: the shapes come from the
copy the project holds. Undo gives the one-layer SVG back exactly.

The picture does not change while the layer is not scaled up or turned. A layer scaled past 100% or
rotated draws sharp as one layer; broken apart it is a group that is resampled like any group, so
it can look softer when scaled up. Break Apart when you need the parts, not before.

### Editing the parts in the Viewer

An SVG imported as editable shapes is a group in the Viewer: a click selects all of it. Double-click
it to enter it and select the part under the pointer, double-click again to go into a group inside
it, and double-click a shape to edit its points. Escape goes back out one level. Inside a group the
rest of the picture is dimmed and cannot be picked, and the Viewer header shows the groups you are
in as a breadcrumb. A double-click on a one-layer SVG offers Break Apart first. The
[Viewer guide](viewer.md#drilling-into-groups) has the whole set of gestures.

A shape the importer built from a few layers (a gradient fill, a dashed or a gradient stroke) is
one shape when you drill in: a click on its fill or its drawn stroke picks it, a gap between dashes
does not, and a double-click edits the one outline its parts share. With the points of a keyed Path
node, each drag is one whole-path key at the playhead.

### From MCP, Python and the command line

`bloom.layer.add-svg` takes `mode`: `"layer"` (the default when omitted) or `"shapes"`; anything
else is refused, naming both spellings. `bloom.layer.break-apart-svg` takes the `layer` and returns
the new root group as `merge`. Both work from MCP `transact`, `bloom.ops.layer.add_svg(path=...,
mode="shapes")` and `bloom.ops.layer.break_apart_svg(layer=...)` in Python, and `bloom-cli run`.
Re-adding an imported SVG takes the same `mode` on `bloom.layer.add-from-assets`,
`bloom.composition.add-from-assets` and `bloom.node.add-asset-source`; it never prompts, so the
default is `"layer"`. The file is read and parsed when the call is made, on the calling thread, so a live session's
interface stays responsive.
