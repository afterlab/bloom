# Arranging panels

Everything under the toolbar is made of panels. You can split, join, close and enlarge them;
none of this changes your project.

## Split

Right-click a panel's header and choose:

- **Split Right**: a new panel opens to the right of this one.
- **Split Down**: a new panel opens below this one.

The new panel starts with the same editor. Use the switcher at the left of its header to change it.
**Window → Split Active Area Right** and **Split Active Area Down** do the same for the panel that
has focus.

## Resize and join

Drag the gap between two panels to resize them. The pointer changes to a resize cursor over it.

Right-click the gap to join the two panels into one. **Join X** means the panel on the other side
grows toward X and takes over the panel on that side:

| Gap | Join | What happens |
| --- | --- | --- |
| Between panels side by side | **Join Left** | The right panel grows left; the left panel closes |
| | **Join Right** | The left panel grows right; the right panel closes |
| Between panels stacked | **Join Up** | The bottom panel grows up; the top panel closes |
| | **Join Down** | The top panel grows down; the bottom panel closes |

The panel that grows keeps its editor and everything in it, and gets focus. The panel that closes
loses its editor state, exactly as **Close** does. A join needs one panel on each side of the gap.
If a side is itself split into more panels, the join items are greyed out and their tooltip says
why: join inside that side first, or right-click a gap between two single panels.

## Close and full screen

**Close** in a panel's header menu removes the panel; the panels next to it take the room. The last
panel cannot close.

**Fullscreen** (the button at the right of the header, the header menu, or the backtick key `` ` ``)
enlarges the focused panel to the whole workspace. While it is full screen it is the only panel
shown, so it has no highlighted border. Choose **Exit Fullscreen** to bring the other panels back;
the highlight returns on the panel that has focus. Split, close and join wait until you exit.

## More windows

**Window → New Window** opens another window on the same project, for example for a second
monitor. It starts with one Viewer and has no menu bar or toolbar; everything else works as in the
main window: split, join and arrange its panels, and use your shortcuts there. Undo, the playhead,
the selection and the active composition are shared by every window. Closing a window closes only
it; closing the main window or quitting closes them all, asking once about unsaved changes.

To move a panel into its own window, drag its header (the empty part, not a control) outside every
Bloom window and release, or choose **Move to New Window** from the header menu. To put it back,
drag its header over a panel in any window: the highlighted half shows where it goes. A window's
only panel stays in it.

Press **F11** in a window to make that window full screen on its monitor, and press it again to
come back. On Windows, double-clicking a window's title bar does the same; on Linux the title bar
belongs to your desktop, so use F11. On macOS the green title-bar button makes a window full screen.

Each window's Viewer has its own display and view (the footer's Display / View), starting on the
project's Viewer default, so one monitor can show a review transform while another shows the plain
view.

Bloom remembers your windows, their panels, monitors, positions and sizes. If a monitor is missing
next time, its windows open on a monitor that is there. On Wayland the desktop chooses where
windows open; Bloom restores their size and full screen.

## Pin a Viewer

A Viewer shows the active composition. To keep it on one thing, use the pin button next to the
composition in the Viewer's header:

- **Pin to Composition** keeps showing that composition whatever becomes active. Pinned to a
  composition you are not editing, the Viewer is for reference: you can pan, zoom and set a region,
  but not select or move layers.
- **Pin to Selected Node** shows the selected node's image output.
- **Follow Active Composition** goes back to following.

A pinned Viewer shows a filled pin and **Pinned**. A pin to something you delete goes back to
following.

Bloom saves the arrangement and restores it next time. Changing it after choosing a workspace preset
shows the preset as edited; see [Workspace presets](workspace-presets.md).
