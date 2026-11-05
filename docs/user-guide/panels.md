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

Bloom saves the arrangement and restores it next time. Changing it after choosing a workspace preset
shows the preset as edited; see [Workspace presets](workspace-presets.md).
