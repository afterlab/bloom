# Workspace Layout

Status: working

Updated: 2026-11-06

## Purpose

Bloom's workspace layout is session state that arranges replaceable editor areas. It must remain
independent of project truth, command history, evaluation, and rendered output.

The current implementation is a custom recursive split tree built with Qt Widgets. It replaces the
initial mixed central-splitter and dock-widget feasibility shell and is the working mechanism for
the first Compositing workspace proof.

## Runtime Structure

- `WorkspaceHost` owns one recursive split tree.
- A normal split is a non-collapsible `QSplitter` with an orientation, at least two children, and
  proportional weights. The authored default nests a full-height right column (Assets over
  Properties) beside a left region whose top row is Viewer | Nodes and whose bottom is the Timeline.
- A leaf is an `EditorArea` with a stable area ID and one selected stable editor type ID.
- `EditorArea` instantiates only its selected editor. Switching type destroys the old editor widget
  and creates the selected editor without mutating project data.
- The application-lifetime `EditorRegistry` owns editor descriptors and factories. Editor type
  identity never depends on display text, picker order, widget position, or translated labels.
- The built-in `bloom.assets` editor is the Assets composition browser. Its Folder icon and
  display name are presentation metadata; composition identity remains the document's stable
  `CompositionId`.
- Mouse and keyboard focus establish the active area. Commands resolve the active area at
  invocation time rather than capturing a persistent widget position.

The widget tree is the current UI-session representation. It must not acquire document objects,
render resources, task ownership, or project mutation responsibilities. Extract a Qt-free layout
model only when another consumer or independently testable session behavior justifies that
boundary.

## Supported Operations

The first slice supports:

- replace the editor type in any area
- split an area right or down (the new area opens to the right of it, or below it)
- close an area while preserving at least one leaf
- join two areas across the divider between them
- maximize and restore an area without rewriting the underlying tree
- reset the Compositing workspace to the Default preset
- save and restore topology, proportions, area IDs, active area, and editor type IDs

Split, close, maximize, restore, and reset are available through visible Window-menu actions
(Split Active Area Right, Split Active Area Down). An area header's right-click menu offers Split
Right, Split Down, Fullscreen and Close for that area. No dedicated default shortcut is assigned
yet; shortcut and remapping policy needs a cross-platform keymap design.

While one area is maximized, off-path branches are hidden but remain in the tree. Split, close and
join are disabled until the layout is restored. Maximizing must not change serialized topology,
proportions, editor choices, or project state. The maximized area is the only one visible, so no
area paints the active highlight while it is maximized; the active area, keyboard focus and the
area's `active` property are unchanged (`EditorArea::setActiveHighlightSuppressed`). Restoring
paints the highlight again on the area that is active then.

Exactly one area is highlighted as active at any time outside maximization. Every path that replaces
or rearranges the tree (reset, restore, `applyArrangement()`) clears the previous active area's
highlight before it activates the new one, so an area a preset keeps never retains a stale
highlight.

### Joining areas

Each divider between two areas is a `QSplitterHandle` that keeps its resize cursor and drag. Its
right-click menu offers the two joins of its orientation; **Join X** means the area on the other
side expands toward X and takes over the area on that side:

| Divider | Join | Survivor | Closes |
| --- | --- | --- | --- |
| Vertical (areas side by side) | Join Left | right area | left area |
| Vertical | Join Right | left area | right area |
| Horizontal (areas stacked) | Join Up | bottom area | top area |
| Horizontal | Join Down | top area | bottom area |

A join is offered only when both sides of the divider are single areas, which then share that whole
edge. Otherwise, and while an area is maximized, the items are disabled and their tooltip gives the
reason. `WorkspaceHost::joinAreas()` closes the other area through the same path as Close (its
editor and editor state are dropped), gives its room to the survivor, and makes the survivor the
active, focused area. The survivor keeps its editor and that editor's state. The change emits
`layoutChanged()` and is saved with the workspace like a split or a close.

## Persistence Contract

The Compositing layout is stored through `QSettings` as versioned compact JSON with:

- format identifier `bloom.workspace-layout`
- integer schema version
- recursive area and split nodes
- stable area and editor type IDs
- split orientation and normalized proportional weights
- active-area identity

This data is user/session preference state, not part of a `.bloom` project.

Restoration validates format, schema, depth, node count, split child counts, identities,
orientations, and weights before replacing the live layout. Invalid data leaves the safe default
layout intact. A future unsupported schema is not overwritten automatically when Bloom exits.

An unavailable editor type is localized to its area. Bloom preserves the missing stable editor ID,
shows an unavailable-editor placeholder, and lets the artist select a registered editor. Other
areas continue to restore normally.

Schema version 3 (#536) adds two things to the same document, both session state:

- an optional `state` object on an area node, the editor's own session state when its editor
  implements `EditorSessionStateProvider`. Today only the Viewer writes one (its pin and its own
  display/view; see [Workspace windows](#workspace-windows)). A `state` that is present but not an
  object makes the layout invalid; an editor that does not recognise a field ignores it.
  Workspace presets never carry it (`saveLayoutState(false)`).
- on the application's layout only, a `windows` array, one entry per secondary workspace window:
  `{id, layout, placement}`, where `layout` is that window's own complete layout document and
  `placement` is `{screen, x, y, width, height, fullScreen, maximized}` (the normal geometry while
  full screen or maximized).

Zoom and scroll positions are still not saved. Readers accept schemas 1 to 3. A schema-2 layout
restores unchanged; only a schema-1 layout is replaced with the Default preset.

## Authored default and Version 2 migration

The first-run and `Window → Reset Workspace` arrangement is the built-in **Default** workspace
preset (owner, 2026-10-31), authored in `WorkspacePresets` as normalized split weights and applied
through `WorkspaceHost::applyArrangement()`:

| Split | Children, left-to-right or top-to-bottom | Weights |
| --- | --- | --- |
| Workspace columns | Left region, Properties | 80% / 20% |
| Left region | Assets/Viewer row, Timeline | 66% / 34% |
| Assets/Viewer row | Assets, Viewer | 25% / 75% |

The Timeline layer-table/lanes divider defaults to 37% of the Timeline width. Its persisted pixel
width remains authoritative for an existing session; Reset Workspace clears that width and writes
the newly resolved 37% default together with the workspace tree. Viewer starts at RGBA/Fit and
Properties shows Composition when there is no selection. These are editor-owned presentation
defaults and do not enter document state. The Workspace field then reads "Default". Nodes is no
longer in the default; the Node preset has it. The older five-area arrangement (Viewer | Nodes
beside Assets over Properties) is gone; a saved workspace of an existing session is restored as it
is and is never replaced by a preset.

Schema version 2 stores the arrangement as a tree of areas and splits. The menu bar and the
toolbar under it remain the only fixed application surfaces
([ADR 0031](../decisions/0031-contextual-tool-bar.md)).

The application first validates and restores a legacy version-1 layout, then replaces its panel
arrangement with the Default preset. Window geometry is retained. This intentional reset prevents
old saved arrangements from omitting Assets or giving Timeline the sidebar's width. Subsequent saves
write schema 2. The generic WorkspaceHost still reads valid version-1 trees for its callers; the
application owns the Compositing migration policy. Invalid layouts keep the safe default, and future
versions retain the existing do-not-overwrite guarantee.

Split fractions are applied once the complete widget tree receives its window extent. Context-bound
callbacks are retired if that tree is replaced before showing, so restoration cannot be overwritten
by pending default-layout work. Minimum widths continue to apply normally. The timeline left column
uses the sum of its token-defined columns; metric audits verify full menus at 1600 and 1920 pixels.
The main-window migration test starts with a valid one-area version-1 layout without Assets and
asserts the Default preset's four areas, including Assets, with the preset named Default.

## Workspace windows

**Window → New Window** (`newWindowAction`) opens a `WorkspaceWindow`: a top-level window with its
own `WorkspaceHost` and no menu bar or toolbar, opening with a single Viewer. It works on the same
`CompositionSession`, project, playhead, selection, active composition, undo stack and command
system as the main window; it holds no state of its own beyond its layout and placement. The main
window's application actions are added to every secondary window, so a `Qt::WindowShortcut` (Undo,
Save, F11, the panel commands) fires in whichever window has focus. On macOS the menu bar is the
global parentless `QMenuBar`, so it serves every window.

Each window's areas, splits, joins and Fullscreen work exactly as in the main window. Window-menu
panel commands act on the active workspace host: the host of the window most recently activated.
Exactly one area across all windows paints the active highlight: activating a window moves the
highlight to its active area, and every other host is told `setActiveHighlightEnabled(false)`.

Closing a secondary window closes only it, retiring its native surfaces through the host's
retirement gate first. Closing the main window or quitting closes every window, after the one
unsaved-changes prompt the main window already shows.

**F11** (View → Full Screen) acts on the window it is pressed in, going borderless full screen on
that window's monitor. On Windows a double-click on a secondary window's title bar does the same
(`WM_NCLBUTTONDBLCLK` on the caption). On Linux the title bar belongs to the window manager or the
compositor's server-side decoration, which maximizes on double-click; F11 is the supported
equivalent there. On macOS the title bar's green button gives the platform full screen.

### Tear-off and docking

Dragging a panel by its header background carries the `application/x-bloom-panel` MIME type with
the source area ID. Over another area, a Selection-wash dock indicator covers the half nearest the
pointer (`EditorArea::dockEdgeAt`: left, right, top or bottom by distance relative to the area's
extent); a drop there moves the panel beside that area (`WorkspaceHost::dockPanel`). A drag that is
released over no Bloom window tears the panel off into a new window at the release point. The
header menu's **Move to New Window** (`panelMoveToNewWindowAction`) does the same without a drag.
A window's only panel, and any panel while one is maximized, cannot move; the item is disabled
with the reason in its tooltip. A moved panel keeps its editor type and its editor session state;
the window it leaves closes when it has no panel left. Moving panels is an undo-free UI action and
is saved with the layout like any other change.

### Persistence and placement

The application's layout document carries the `windows` array described above. Restoring re-opens
each valid entry (at most 64; a malformed entry is skipped, never fatal), then makes the main
window's workspace active again.

A saved placement is resolved against the screens present (`resolveWindowPlacement`):

1. If the named screen exists and the saved frame overlaps it, the frame stays, fitted inside that
   screen's available area.
2. Otherwise the frame goes to the screen it overlaps most, fitted inside it.
3. Otherwise (its monitor is gone and the frame is off every screen) it is centred on the primary
   screen, shrunk to fit, and reported as relocated.

On Wayland a client cannot position its own windows (`platformPositionsWindows()` is false). Bloom
restores the size, the screen as a hint and full screen, and the compositor chooses the position.
A tear-off there opens where the compositor places it, since a drag released outside reports no
position.

### Viewer pin and per-window display

A Viewer follows the session's active composition by default. Its header's pin control
(`viewerPinButton`) offers **Follow Active Composition**, **Pin to Composition** and **Pin to
Selected Node** (the node's first Image or Mask output, shown through the #490 channel-inspection
route). A pinned Viewer shows the filled Accent pin and a "Pinned" label (with the node name for a
node pin). It keeps showing its target when the active composition changes, evaluating that
composition from the live document snapshot. Pinned to another composition, it is a reference
view: no layer bounds, overlays or direct manipulation, only pan, zoom and ROI. A pin whose
composition or node is deleted falls back to Follow, and a pin is restored only for the same
project.

Each Viewer's pin is also remembered per project (#550), under its area ID, in the project's
`ProjectViewMemory` (see [Project session](project-session.md#recent-paths-and-presentation-state)).
When New or Open installs a project, every Viewer in every window restores the pin it had in that
project, or follows when it had none or its composition or node is gone. So a project closed and
opened again, or opened after a relaunch whose windows came back from the saved layout, shows each
pinned Viewer's own target again; areas keep their IDs across the layout save, which is what ties a
remembered pin to its Viewer.

A pinned Viewer and every Viewer in a secondary window get their own `CompositionPreviewController`
from the application's `ViewerPreviewPool`. Each controller shares the one preview pipeline, frame
cache, GPU display submitter and processing policy, so there is no fixed per-window cap. Only the
main controller scopes the shared cache's retention range. Each window's Viewer has its own OCIO
display/view, which starts on the project's Viewer default. Because display and view are part of
`PreviewFrameCacheKey`, two windows showing one frame through different transforms never share a
cache entry. The main window's Viewers keep the per-project display/view memory.

Every window owns its native surfaces (and swapchains) and shares the device and caches.
`MainWindow::liveNativeSurfaces()` reports the surfaces of every window to the publication sink and
to shutdown retirement. Per-monitor scale changes reach each window's Viewer through the existing
`DevicePixelRatioChange` handling, which resizes only that window's surface. No new
per-window frame retention is added, so Viewer memory behavior (#463) is unchanged.

## Workspace presets

A preset is a named arrangement, stored as the same versioned layout state `saveLayoutState()`
writes (topology, proportions, stable editor type IDs); it never holds project data or an editor's
own state. `WorkspacePresets` (`src/ui`) owns the list: built-in Default, Node, Export and Animation
are authored in code, the artist's own are stored in application preferences under
`workspace/presets/user` (a JSON array of `{name, layout}`), and the active preset's name under
`workspace/presets/active`. Names are unique across both lists, case-insensitively, and a built-in's
name cannot be reused. The toolbar's Workspace field reads this one list; there is no menu for it.

`WorkspaceHost::applyArrangement()` applies a preset without rebuilding what it keeps: each area of
the new tree is taken from the live tree when one hosts the same editor type (so zoom, scroll and
selection survive), created otherwise, and areas with no place close. The state is validated in a
dry pass before any widget moves, so an invalid state changes nothing. It runs through the same
native-surface retirement gate as every other mutation. Reset Workspace applies the Default preset, so
the field reads Default.

`WorkspaceHost::layoutChanged()` fires for splits, closes, divider drags, editor swaps and restored
layouts. After a preset is chosen, the picker compares the live arrangement (topology, editor types,
divider weights within a tolerance of six percent) with the preset and shows "<name> (edited)" while
they differ.

## Reveal policy

`WorkspaceHost::revealEditor(id)` shows an editor and makes its area active and focused: the area
that already hosts it, else a new area split below the Timeline's. Per-editor policy overrides the
second step. `bloom.export` is never split off: with an Export area open it is only activated; else
an open Timeline's area switches to Export in place; else a new area takes the bottom of the whole
workspace.

## Acceptance And Gating

The split tree remains a working product mechanism until its focus, scaling, persistence,
maximization, and recovery behavior is exercised on Linux, macOS, and Windows. The implementation
must retain equivalent semantics on each platform even if menu presentation or window chrome is
platform-owned.

The following remain deferred:

- drag-from-corner splitting and drag-to-merge gestures (joining from the divider menu exists)
- tabs within an area
- per-workspace content beyond the arrangement (a preset stores panel arrangement only)
- per-editor internal session-state migration
- keymap customization
- layout undo history

These deferred features must extend the same session-state and editor-registry contracts rather
than creating a second workspace mechanism.

## Script editor

`bloom.script` is a replaceable registered editor with kit input, history, output and shared editor
header/footer controls. It is offered by the panel switcher without changing the default
layout. Python-enabled builds attach it to the live host; other builds show an explicit unavailable
state. See [Scripting Bloom with Python](../user-guide/python.md) for execution and undo behavior.

## MCP Server editor

`bloom.mcp-server` (display name **MCP Server**, ADR 0034) is a replaceable registered editor for the
in-app MCP server. It holds no server state: every MCP Server editor and the status bar read one
application-lifetime `McpServerAdapter`, which owns the `mcp::McpServerService`, drains it on the UI
thread and emits one change signal per published snapshot. Closing, duplicating or swapping the
editor leaves the server running. The editor issues the service's commands (Start, Stop, Port,
Start when Bloom opens, Reveal, Copy, Regenerate, Copy client config) and shows each typed refusal's
reason verbatim under the control it belongs to; port, auto-start and the token are preferences,
never project state, and nothing the editor does enters the undo history. See
[Using Bloom from an agent](../user-guide/mcp.md).
