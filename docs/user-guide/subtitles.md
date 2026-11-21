# Subtitles

A Subtitles layer draws a list of timed cues over the picture, styled and placed the way subtitles
are, and keeps the cues so they can also be exported as a subtitle track or file. The cues come from
an SRT, WebVTT, TTML or ASS file, from text you type, or from another node's cue list. The full
specification is [Subtitles](../architecture/subtitles.md).

## Adding subtitles

- **File > Import**, or drop a subtitle file (.srt, .vtt, .ttml, .dfxp, .ass, .ssa) on the window,
  the Timeline or the Viewer: a Subtitles layer that follows the file starts at the composition's
  start. A subtitle file does not become an asset.
- **Add > Subtitles** in the Timeline or the Viewer: an empty Subtitles layer to type cues into.
- The **Subtitles** node in the Nodes Add menu (Text & Paths).

A new layer's Size is 5.5 % of the composition's height (59 pixels at 1080) and its outline 1/360
of it (3 pixels at 1080).

## Editing cues

Select the Subtitles layer. Properties opens its section with the **Source** row and the cue list.

The **Source** row says where the cues come from: typed in Bloom, a file (with its state: in sync,
changed, missing or unreadable) or another node. **Load File…** reads a file into the node; choose
its format and encoding, or leave them to be detected, and whether the cues follow the file. What
the reader dropped or changed is listed under the row. **Reload** reads a followed file again;
**Unlink** keeps the cues and stops following the file. Cues that follow a file, or come from
another node, cannot be edited in Bloom; edit the file, or Unlink.

The cue list shows each cue's number, start, end, duration, text, speaker and checks. Times read in
frames or timecode, as the Timeline does. Double-click a cell to edit it; a time takes a frame
number or a timecode. The filter shows the cues whose text or speaker holds the words.

| Button | Key | What it does |
| --- | --- | --- |
| Add | `Insert` | A cue at the playhead, two seconds long or up to the next cue |
| Delete | `Delete` | Deletes the picked cues |
| Split | `Ctrl+B` | Cuts the picked cue in two at the playhead |
| Merge | `Ctrl+J` | Joins the picked cue and the next one |
| Set Start, Set End | `[`, `]` | Moves the picked cue's start or end to the playhead |
| Shift… | | Moves every cue, or the picked ones, earlier or later |
| Scale… | | Retimes cues made for one frame rate (25) to play at another (23.976) |

**Edit as Text** shows the whole list as SRT or WebVTT text to edit freely. **Apply** reads it back;
if a line does not read, the errors are listed with their line numbers and nothing changes until it
does. **Copy** puts the list on the clipboard as SRT or WebVTT.

Every edit is one step of Undo.

## On the Timeline and in the Viewer

A Subtitles layer's bar on the Timeline shows a block for each cue. Click one to pick it; drag it to
move the cue, or drag its edge to move its start or end. The edge snaps to the playhead and to the
other cues' edges and lands on a frame; hold `Shift` to snap to frames only. `Delete` deletes the
picked cue. The bar's own ends still trim the layer.

With the layer selected, the Viewer outlines the safe area and each cue showing, with its number.
Click a cue to pick it, drag it to move the subtitles (their Offset), and double-click it to edit its
text in place (`Ctrl+Return` to finish, `Escape` to cancel).

When Burn in is off, the Viewer still shows the cues as a guide, white on a dark plate with a dashed
outline, so you can time them. Turn it off with View > Guides > Show Track-Only Subtitles. The guide
is never part of a RAM Preview or an export.

## What a cue shows

A cue shows from its start up to, but not including, its end. Its text may use WebVTT markup:

- `<i>`, `<b>` and `<u>` for italic, bold and underline;
- `<c.yellow>` and the other WebVTT color names (white, lime, cyan, red, yellow, magenta, blue,
  black) for a colored run, and `<c.bg_black>` and the like for a colored box;
- `<c.name>` with one of the file's own styles;
- `<v Name>` for a speaker.

A font without an italic face is slanted by 12 degrees instead; a font without a bold face draws at
its regular weight and shows the missing-font warning. The built-in font has neither.

Bloom lays text out without shaping, so scripts that need it (Arabic, Hebrew, the Indic scripts,
Thai and others) will not read correctly. Each cue holding such text gets a warning that names it:
"Cue 4 contains Arabic text; Bloom lays text out without shaping, so it will not read correctly".

## Style

- **Font**, **Size** (pixels), **Fill**, **Outline color** and **Outline width** (pixels). The
  outline is drawn around each letter, under its fill.
- **Box**: Off, Per line (a box behind each line) or Block (one box behind the whole cue), with its
  color, padding and corner radius (both in em, fractions of the size).
- **Line height**, **Letter spacing** and **Opacity**.

Size, colors, outline width, line height, letter spacing, opacity and Offset can be animated.

## Layout

- **Safe area**: Title safe or Action safe (from Composition Settings), Full frame, or Custom with
  your own margins. The cues are placed inside it.
- **Anchor**: one of nine points of the safe area; bottom center by default. **Offset** moves the
  subtitles from there, in pixels.
- **Max width**: the longest a line may be, as a fraction of the safe area's width.
- **Wrap**: Balanced (lines of even length, the longer one at the bottom), Greedy (fill each line in
  turn) or Explicit only (break only where the text has a line break). Lines break where Unicode
  allows, so Chinese and Japanese text wraps between characters.
- **Max lines** and **Overflow**: with Shrink to fit, a cue that needs more lines is drawn smaller,
  down to 70 % of its size; with Show all lines it keeps its size and shows every line.
- **Text alignment**: Left, Center or Right.
- **Use cue positions**: a cue placed by its file (a WebVTT position or line, an ASS `\pos`,
  alignment or margins, a TTML region) is drawn there.
- **Use file styles**: a file's colors, sizes and fonts apply to the cues that use them.

When several cues show at once, they stack away from the anchored edge: at the bottom, the earliest
cue is lowest and later ones sit above it.

## Timing and track

- **Fade in** and **Fade out**, in seconds, fade each cue as it starts and ends.
- **Show speaker**: Name prefix shows "NAME: " before a cue's text.
- **Burn in**: off draws nothing; the cues still export as a track and still feed the node's
  outputs.

## Using the cues elsewhere

Besides its picture, the node outputs the cue list (to feed another Subtitles node with a different
style), the text showing now, the index of the cue showing now (-1 for none) and how far through it
the frame is (0 to 1). Connect them to a Text node, a Text Animator or any value node to animate
captions.

## Checks

Each cue is checked against the node's Checks settings: characters per line, reading speed
(characters per second), lines, minimum duration, the gap to the next cue (in frames), overlaps,
cues past the composition's end, and scripts drawn without shaping. A check never changes the
picture. The findings are listed with each cue in the cue list's Checks column and in the
`subtitles` read of MCP and Python.

## Limits

A frame draws at most 16 cues at once and 8,192 glyphs. A frame past either is not drawn, and the
render says when and which cues: "At 00:00:04.000 17 cues show at once (cues 3, 4, ...); at most 16
may". A cue holds at most 4,096 bytes of text.

The picture is the same in the Viewer, in every export and at a reduced render scale. Frames inside one cue reuse one rendered result, so a long cue costs one frame of work.

Scaling or rotating the Subtitles layer redraws the captions at the new size and angle, so they stay
as sharp as unscaled captions instead of being enlarged as a picture.
