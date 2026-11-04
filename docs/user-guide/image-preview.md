# The Image Preview node

Use an Image Preview node to check the image at any point in a node graph while you build or debug
it. It works like a Viewer node and backdrop in other compositors. The node has no output, so it never
changes the composition's output, RAM Preview, exports or renders.

## Adding one

Add **Image Preview** from the Output section of the Add menu or the Tab search. You can also drag
from any image output and add it there. Its one input takes an image. It also takes a mask, or a
channel set, which it shows as its color layer. The card draws the input at the playhead below its
controls. A preview connected through reroutes shows what reaches the reroute chain.

## Display

The **Display** dropdown on the card chooses what the thumbnail and the background show:

| Display | Shows |
| --- | --- |
| RGBA (default) | The image as the Viewer shows it, with transparency over a checkerboard. |
| RGB | The color without alpha. |
| Alpha | Alpha as gray. For a channel set, its alpha channel. |
| Red, Green, Blue | One channel as gray. |
| Normalized | The image's lowest to highest value stretched from black to white. Use it to read data passes such as depth, mist or position that fall far outside 0 to 1. |

The color views use the Viewer's display, view and look. Normalized shows the values themselves with
no view transform. The Display choice is saved with the project and can be undone like any other edit.

## Using it as the node editor background

Right-click an Image Preview and choose **Show as Node Editor Background**. The node editor then
draws that node's image behind the graph. Only one node is the background at a time. Choosing
another node moves the background to it, and choosing the same node again turns it off. The
background is part of your session, not the project, so choosing it adds nothing to undo.

The background has its own view, separate from the graph's:

| Action | How |
| --- | --- |
| Fit the background to the editor | View ▸ Background ▸ Fit Background |
| Show it at 100% (one screen pixel per composition pixel) | View ▸ Background ▸ Background Actual Size |
| Zoom it about the pointer | `Alt` + wheel, or View ▸ Background ▸ Zoom Background In/Out |
| Pan it | `Alt` + middle drag |
| Hide or show it | View ▸ Background ▸ Show Background |
| Stop using it | View ▸ Background ▸ Clear Background |

A newly chosen background starts fitted to the editor. Zooming or panning the graph never moves the
background, and zooming or panning the background never moves the graph.

## Keeping it fast

A preview never slows the editor down:

- It is prepared on background workers, through the same GPU preview route the Viewer uses, at
  the size it is drawn. A thumbnail is a small proxy. The background is the size it covers on
  screen, and reaches full resolution only at 100% or closer.
- A preview is prepared only when it is visible. When the input, playhead, size or display changes,
  Bloom cancels the work in progress and prepares the new preview.
- Bloom keeps the previews it has made, so returning to a time, a size or a node you have already
  seen does not prepare it again.
- While a preview is being prepared, the card keeps the last image, dimmed, or shows *Preparing
  preview…*. A preview that cannot be prepared says why on the card.
- A **CPU** label means the pixels did not come from the GPU route. Normalized always reads the
  values on the CPU at the drawn size, the way the Viewer's pixel probe does. Otherwise the label
  appears when no GPU is available.
