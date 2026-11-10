# Vector Artwork

Vector artwork is built from shapes, and a shape's outline is a path. This page says how to work
with paths and path values on the node canvas. Later sections of this page cover importing and
editing SVG artwork.

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
