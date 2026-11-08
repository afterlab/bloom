# Text layers

A text layer's **Position** is a point on its text block that follows the text's **Alignment**, so a
block can be placed predictably whatever its lines say or how long they are:

| Alignment | Position is the block's |
| --- | --- |
| Left | left edge |
| Center | centre |
| Right | right edge |

With Left alignment, a line that grows longer therefore grows to the right of Position instead of
spreading to both sides and running off the left of the frame. With Right alignment it grows to the
left, and with Center alignment evenly on both sides. Every line of the block shares the same edge or
centre, so lines of different lengths line up on it. Vertically, Position is still the middle of the
block, and **Anchor** moves the pivot away from Position by a pixel offset as it does for any layer.

The block is the widest line for text without a box, and the box for text with a **Box** (see
[the layer and graph model](../architecture/layer-graph-model.md#text-and-mute-lowering)). The same
layout is used by the Viewer, by still and sequence export and by video export, on the CPU path and
on the GPU path.

A new text layer starts at the centre of the composition, so a Left-aligned new layer starts at the
centre and reads to the right; drag it or set **Position** to move its left edge where you want it.

## Anchor mode and older projects

The Text node's **Anchor Mode** choice has four settings. **Follow alignment** is the behaviour above
and is what every new text layer uses. **Left**, **Center** and **Right** are the settings projects
saved before Follow alignment existed carry: they keep Position at the centre of the text block, so an
old project looks exactly as it did when you reopen it, with no jump. Nothing is rewritten when you
open a project; switch a text layer to Follow alignment when you want its Position to follow the
alignment. Changing it moves the text, because Position then names a different point of the block.
