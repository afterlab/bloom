# The Viewer

The Viewer shows the composition at the playhead and is where you move, scale and rotate layers
directly. Every edit is the same undoable command Properties and the Timeline use.

## Moving layers

With the Select tool, drag a selected layer to move it. The drag is one undo step, committed when
you release the mouse.

Hold **Shift** while dragging to lock the move to a fixed axis: horizontal, vertical, or one of the
two 45 degree diagonals. Bloom picks the axis nearest to the direction you have dragged since you
pressed the mouse.

- Press or release Shift at any moment during the drag. The layer never jumps: it always sits where
  a constrained (or free) move from the original position would put it.
- The axis is the one you see on screen, at any zoom or pan, even for a rotated or parented layer.
- The same lock applies when you drag points of a path or mask.
- Shift still adds to the selection when you click, scales proportionally when you drag a corner
  handle, and steps rotation by 15 degrees. Shift only constrains a move that is already dragging.

## Selecting several layers

With the Select tool, drag from empty space to draw a selection rectangle. When you release, every
layer the rectangle touches is selected; a layer does not have to be fully inside it.

- A plain drag replaces the selection. A rectangle that touches nothing clears it.
- Hold **Shift** to add the touched layers to the selection.
- Hold **Ctrl** (**Command** on macOS) to toggle them: selected layers leave the selection, the
  others join it.
- Press **Escape** before releasing to cancel the rectangle and keep the selection as it was.
- A click on empty space, without dragging, clears the selection.

The selection is the same one the Timeline, the Nodes panel and Properties show. The rectangle's
border keeps the same thickness at every zoom. Inside a group you have entered, a drag on empty
space does not draw a rectangle; Shift-click the children instead.

To draw a region of interest, hold **Ctrl+Alt** (**Command+Option** on macOS) and drag.

## Drilling into groups

A group is a layer whose content is a Merge of other layers: an SVG imported as editable shapes,
and every group inside it, or a Merge entry you built yourself. Clicking in the Viewer selects the
group as a whole. To work on what is inside it, drill in.

- **Double-click a group** to enter it. The child under the pointer is selected. Double-click again
  on a child that is itself a group to go one level deeper, to any depth.
- **Double-click a shape** whose outline is a path (or is drawn by a Path node) to edit its points
  with the anchor tools. An imported shape with a gradient fill, a dashed stroke or a gradient
  stroke is one shape here, though it is built from a few layers: double-clicking it edits its
  outline.
- **Double-click a one-layer SVG** and the Viewer offers to Break it Apart into editable shapes.
  Break Apart is one undo step; the Viewer then enters the result.
- **Escape**, or a double-click on empty space, goes back up one level: from point editing to the
  shape's group, then out one group at a time, with the group you left selected.
- **Enter** (or Return) enters the selected group, selecting its topmost child, or edits the
  selected shape's points.

While you are inside a group, a single click picks only among that group's children (a child group
is picked whole). Everything outside the group stays visible but is drawn at 30% of its own opacity,
and it cannot be picked. A pick follows what the frame draws at the playhead: the outline after keys,
parents and transforms, a layer's masks and track matte, the topmost child first. A click on nothing
clears the selection and stays inside.

The dimming is only in the Viewer. It is part of what the Viewer and RAM Preview cache, so a frame
cached inside a group is never shown outside it, or the reverse, and it never reaches an export,
a render from the command line, Python or MCP, or a still. Entering or leaving a group re-renders
the Viewer's frame (from the cache when that frame was shown before).

The Viewer header shows where you are after the selected layer: the composition, then each group
you are in, outermost first, with **Points** after the shape whose points you edit. Click a crumb to
go back to that level; the group you came out of is selected. When the header is narrow the middle
crumbs fold into one "…" first; the tooltip always has the whole path. The breadcrumb is one Tab
stop: Left and Right move between crumbs, Enter or Space goes to the focused one.

Selecting a layer anywhere else (the Timeline, the node editor, Properties, MCP) sets the Viewer's
level to that layer's groups; selecting a Path node goes to the shape it draws. The level is not
saved with the project and is not an undo step. MCP `viewer.set` with `level` and `points`, and
Python `bloom.viewer.set_level`, set it; `query` `context` and `bloom.context.level` read it
([MCP](mcp.md), [Python](python.md)).

Dragging the points of a shape whose path (or driving Path node) is keyed writes one whole-path key
at the playhead, one undo step per drag. Moving, scaling and rotating a child inside groups works as
at the top level: the gizmo follows the child's groups, and each drag is one undo step. A selected
shape whose outline is a path also shows its points, and a point or handle under the pointer
answers before the gizmo does.

## RAM Preview

Click **RAM Preview** in the Viewer footer, choose Composition > RAM Preview, or press **Numpad 0**
(or Ctrl+Shift+Space) to cache the composition's range into RAM and then play it. While it fills,
the button reads **Cancel** with the fill progress; click it again, press the same key, or press
**Esc** to stop. Frames already cached are kept, and the button returns to RAM Preview. Numpad 0
does nothing while you type in a text or value field.
