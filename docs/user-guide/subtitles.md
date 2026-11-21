# Subtitles

A Subtitles layer draws a list of timed cues over the picture, styled and placed the way subtitles
are, and keeps the cues so they can also be exported as a subtitle track or file. The cues come from
an SRT, WebVTT, TTML or ASS file, from text you type, or from another node's cue list. The full
specification is [Subtitles](../architecture/subtitles.md).

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
picture. The findings are listed with each cue in the `subtitles` read of MCP and Python.

## Limits

A frame draws at most 16 cues at once and 8,192 glyphs. A frame past either is not drawn, and the
render says when and which cues: "At 00:00:04.000 17 cues show at once (cues 3, 4, ...); at most 16
may". A cue holds at most 4,096 bytes of text.

The picture is the same in the Viewer, in every export and at a reduced render scale. Frames inside one cue reuse one rendered result, so a long cue costs one frame of work.
