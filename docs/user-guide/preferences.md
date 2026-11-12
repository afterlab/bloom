# Preferences

Bloom keeps two kinds of saved choice apart:

- **Global preferences** are user-wide choices for your installation. They live in this Preferences
  window and are stored outside any project.
- **Project settings** are part of one `.bloom` file. Color configuration is the current example,
  under **File → Project Settings…**.

Open **Edit → Preferences…** (⌘, on macOS, where the operating system moves it into the Bloom
application menu). Choose a category in the sidebar on the left (the arrow keys, Home, End and
typing a category's name move through it); Bloom shows the last category you used again for the
rest of the session. The window uses **OK**, **Cancel**, and **Apply**. Apply saves without
closing; Cancel discards anything not applied. **Restore Defaults** is in the menu button at the
foot of the sidebar: it asks first (Cancel is the default answer) and only resets the window's
values; nothing is saved until OK or Apply. Each section's reset button returns just that section to
its defaults, again only in the window. A cache folder that is a file, or one Bloom cannot write
or create, is refused under the field and OK waits.

## Pages

| Page | What it controls | When it takes effect |
| --- | --- | --- |
| General | Audio playback, loop playback, **Crash recovery** (see below), and the two **Agents** switches (see below) | Immediately |
| Import | **Sequence frame rate** (Match composition, or a rate such as 24 or 23.976; default 24) and **Still duration** (Composition length, or a fixed length in seconds) that newly imported footage starts with, and **SVG files** (Ask each time, As one layer or As editable shapes). See [Interpret Footage](interpret-footage.md#import-options-and-preferences) | The next import |
| Sounds | Play sounds (master), volume, event sounds, interaction sounds, render notifications, and a Play test sound button. See [UI sound feedback](../ux/sound.md) | Immediately |
| Viewer | Resolution, the background around the composition frame (Solid, Gray, Black, White), the transparency grid inside it, and the safe-areas, center-cross, thirds, rulers and pixel-grid overlays | Immediately |
| Timeline | Time format, snapping, keyframes, graph editor, layer column width | Immediately |
| Node Graph | Link style, snap to grid, grid size, **Show zoom controls** (the zoom buttons and value in the node editor footer) and **Show minimap** (the overview in the canvas corner); both on by default | Immediately |
| Memory & Caches | RAM and VRAM headroom, **Worker memory cap** and **Worker threads** for export workers, media disk cache enable/directory/budget | Headroom on Apply or OK; worker limits on the next export you start; disk cache after restart |
| Performance | Read-only acceleration status | Not a preference |

Immediate changes reach every open panel: the window hands the committed value to each editor,
which applies it through the same setters its own menus use. Headroom reaches the engine on Apply or OK. Values marked **Restart** (the disk
cache enable, location and disk-space budget, marked once in the Media disk cache header) are read
when Bloom starts. Leave the cache directory empty for the default location; **Browse…** picks a
folder and the reset button beside it returns to the default.

## Crash recovery

**Recover unsaved changes after a crash** (General page, stored as `recovery/enabled`, MCP key
`crashRecovery`) is off by default. While it is off Bloom writes no recovery data, shows no
**Recover Unsaved Changes** prompt at startup, and leaves recovery files already on disk untouched
and unoffered. While it is on, Bloom periodically writes a recovery copy of unsaved work and offers
it after an unexpected exit. Changing it takes effect at once, without a restart.

## Agents

Two switches on the General page let a connected agent act on this computer beyond editing the
project. Both are off by default and only this window turns them on (an agent cannot, through
`preferences.set` or otherwise).

- **Let agents answer open dialogs** (`agents/answer-dialogs`, MCP key `agentsAnswerDialogs`)
  lets an agent press a button of the dialog that is blocking Bloom, including a button that
  discards or overwrites data; the agent is told which buttons do before it chooses.
- **Let agents run Python scripts** (`agents/run-scripts`, MCP key `agentsRunScripts`) lets an
  agent run Python in the open project through the Script editor. Python can reach anything your
  account can, so this is access to this computer for whoever holds the MCP token. The script's
  edits use the normal undo history. See [Using Bloom from an agent](mcp.md).

Both take effect at once.

## Import

The Import page sets what footage starts with when it is imported: a new image sequence assumes the
**Sequence frame rate** (stored as `import/sequence-frame-rate`, an exact `n/d` such as
`24000/1001`, or `composition`), and a new still lasts the **Still duration** (`import/still-duration`,
seconds as `n/d`, or `composition` for the composition's length). Bloom writes them into each new
asset's interpretation when it imports; footage already in the project keeps its own, which
**Interpret Footage** changes. An unreadable or out-of-range stored value reads as the default.

The Import page also has **SVG files**: what an imported or dropped SVG becomes (`import/svg-mode`,
`ask`, `layer` or `shapes`). **Ask each time** (the default) offers the choice for every import;
**As one layer** and **As editable shapes** use that mode without asking. Ticking **Remember my
choice** in the chooser sets this row. See [Vector Artwork](vector-artwork.md#importing-an-svg).

## Export workers

Movie and WAV exports encode in a separate helper process, the export worker. Two rows in the
Memory section of **Memory & Caches** limit it. Both are off by default, and with both off an
export behaves exactly as it does without them:

- **Worker memory cap** (`export/worker-memory-cap-bytes`, MCP key `exportWorkerMemoryCapBytes`):
  **No limit**, or **Limit** with a size from 0 to 1024 GiB. At the cap the export waits instead of
  using more memory: it holds back the next frame until the worker has finished what it already
  holds, and goes on once the worker is under the cap. If the worker is still at the cap with
  nothing left to finish, the export stops with a message that names the memory the worker holds and
  the cap, and publishes nothing.
- **Worker threads** (`export/worker-threads`, MCP key `exportWorkerThreads`): **Automatic**, or
  **Custom** with 1 to 64 encoder threads. More threads encode faster and hold more frames in
  memory, so lowering them is the other way to fit under a cap. A movie made through the
  platform's own media framework chooses its own threads and ignores this row;
  the memory cap still applies.

The cap measures the worker's resident memory as the operating system reports it for that process;
it does not limit Bloom itself, other helper processes or the graphics card. Both rows apply to
every export the application starts after the change, from the Export panel or over MCP, and `export`,
`export.start` and `export.start-all` can override them for one call (see
[MCP](mcp.md#export-worker-limits)). The result of an export reports the worker's peak memory
whether or not a cap is set.

## Memory headroom and disk budget

**RAM headroom** and **VRAM headroom** are the memory Bloom leaves free for the rest of your
computer; Bloom uses everything else for rendering and caching. They are the only memory limits:
usable VRAM, the graphics driver's live budget minus the VRAM headroom, is the one ceiling for
everything Bloom keeps on the graphics card. Each is a size from 0 to 256 GiB.
The default is 10% of the memory it protects: RAM headroom is 10% of physical memory (or of a
tighter process or container limit, about 6 GiB on a 64 GB workstation), VRAM headroom is 10% of
the graphics card's memory (about 2.4 GiB on a 24 GB card; 2 GiB until the card has been measured).
They are stored as decimal byte strings under `memory/ramHeadroomBytes` and
`memory/vramHeadroomBytes` only when they differ from the default; a missing, non-numeric,
overflowing or above-256-GiB value selects the default, which follows the measured capacity.
The older `playback/operation-cache-bytes` and `playback/ram-preview-memory-bytes` preferences are
no longer settings and are ignored.

The disk cache budget, and the timeline layer column width, have a mode: **Automatic** (stored as 0)
selects the machine-derived default, and **Custom** shows the value to enter. See
[Memory](memory.md) for how Bloom derives the defaults. The disk cache can be purged at any time from
**Edit → Purge… → Purge media cache**, which clears the on-disk store and the applicable in-memory
decoded media off the interface thread; the source files are never changed.

## MCP server

The in-app MCP server's port (`mcp/port`, default 6767), **Start when Bloom opens** (`mcp/autoStart`,
off by default) and its access token are global preferences, kept per user and outside projects.
They are edited only in the **MCP Server** editor, which this window links to rather than
duplicating the controls; the port changes only while the server is stopped. The token is not in
the settings store at all: it lives in an owner-only file in Bloom's user-data folder. See
[Using Bloom from an agent](mcp.md).

## Project, session, and per-area state

Not everything persisted is a global preference. This window deliberately excludes:

- per-area and per-revision viewer analysis, display-view, and look state;
- per-composition safe-area presets;
- per-project export settings;
- window geometry and the workspace layout, which are session state rather than preferences.

Those are restored by the surface that owns them. Keeping them out of Preferences keeps one owner for
each value.

## Performance and GPU

The Performance page is read-only. It reports which backend is evaluating work. This version has no
GPU backend, so it reports the CPU reference path and leaves deterministic output unaffected. When
GPU execution ships, a capability-report-backed provider fills in the device, driver, and
per-operation qualification; the page needs no change, and GPU policy preferences (currently a
reserved `render/` key namespace) will be added deliberately rather than inferred from having a GPU.
See [`architecture/gpu-backend.md`](../architecture/gpu-backend.md).
