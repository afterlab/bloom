# Subtitles

Status: canonical

Updated: 2026-11-22

Issue #752. A Subtitles node (`bloom.subtitles`) draws a list of timed cues. This document is the
specification of the cue list value, the file formats Bloom reads and writes, the edits, the linked
file, the node's layout, rendering and checks, and how agents and scripts reach all of them. The project file arm is in
[project-format.md](project-format.md), "Cue Lists".

## Module Boundary

`src/subtitles` (`bloom/subtitles/subtitles.hpp`, `bloom/subtitles/webvtt.hpp`) holds the readers
and writers. It is Qt-free and depends on `src/core` and `src/document`. Only `src/host` and
`src/scripting` link it; `src/mcp` reaches it through them. Expat is linked privately and statically; see
[dependency-intake.md](dependency-intake.md), "Expat In The Subtitle Readers".

A load is pure: bytes go in and a `document::CueListValue` comes out, with a report. The callers
own file access and where the result goes. Every file read runs off the UI thread:

- a scripting factory reads on the client thread, as one cancellable task;
- the linked-file monitor reads on a blocking-I/O task;
- the export preflight samples on a worker.

## The Cue List Value

`document::CueListValue` (`bloom/document/cue_list.hpp`) is the value of the `cue-list` socket and
parameter kind. It is labeled **Cues**, connects only to its own kind, and is constant or driven,
never keyed.

| Member | Meaning |
| --- | --- |
| `language` | BCP 47 tag, `und` when unknown |
| `nextCueId` | the next id a new cue takes; ids are never reused within a list |
| `provenance` | `authored`; `file {portability, path, format, encoding, digest, size, modified}`; or `producer {name, version, parametersDigest}` |
| `styles` | at most 256 named style overrides, sorted by name; colors are straight display sRGB 0 to 1, sizes fractions of the frame height |
| `cues` | sorted by `(start, end, id)` |

A cue has:

- `id`, a positive integer stable within the list;
- `start` and `end`, exact rational seconds on the node's clock, `0 <= start < end`;
- `text`, UTF-8 cue markup;
- optional `speaker` and `style`;
- optional `placement {anchor, positionX, positionY, lineOffset, width, textAlign}`.

The end is exclusive: a cue shows on frame k when `start <= t_k < end`.

Cues are held by shared immutable pointers. A copy of a list, such as a history entry, a draft or
a compiled plan, shares every cue it did not change. An edit allocates the changed cues and one
pointer per cue. Two lists compare equal when their cues do, wherever the cues are held.

### Cue Markup

Cue text is WebVTT cue text (W3C WebVTT, "WebVTT cue text parsing rules"):

- the elements `<c.class> <i> <b> <u> <ruby> <rt> <v name> <lang tag>`;
- timestamp tags such as `<00:00:01.500>`;
- HTML character references (the full named-reference table, `html_named_references.inc`);
- `\n` line breaks.

Where a cue enters the document its text is held to the strict rule (`cue_markup::strictProblem`):
nothing the parser would ignore. The WebVTT default color classes (`white lime cyan red yellow
magenta blue black` and their `bg_` forms) are built-in styles that every list may name.

### Limits

| Limit | Value | MCP name |
| --- | --- | --- |
| cues per list | 50,000 | `subtitleCues` |
| UTF-8 bytes of text per cue | 4,096 | `subtitleCueTextBytes` |
| bytes per file or pasted text | 16 MiB | `subtitleFileBytes` |
| styles per list | 256 | `subtitleStyles` |
| cues one frame draws at once | 16 | `subtitleActiveCues` |
| lines per cue before wrapping | 16 | |
| bytes per speaker or style name | 256 | |

MCP advertises the named limits under `capabilities.experimental.bloom.limits`. A list that
breaks one is not a valid list: an edit or load that would leave one is refused with its first
problem, such as "17 cues show at once at 00:00:04.000; at most 16 may". A list another node
supplies is not checked at the edit, so the `subtitles` read warns on each cue past the
active-cue limit.

## Formats

| Format | Read | Write |
| --- | --- | --- |
| SubRip `.srt` | yes | yes |
| WebVTT `.vtt` | yes, by the W3C parser algorithm, checked against the web-platform-tests `webvtt/parsing` cases | yes |
| TTML / IMSC 1.1 Text (`.ttml`, `.dfxp`, `.xml` with the TTML namespace) | yes, through the locked Expat | yes (IMSC 1.1 Text) |
| ASS / SSA | yes, a reported subset | no |
| SCC, EBU STL, SBV, LRC, PGS, VobSub, SAMI | refused, by name and reason | no |

**Sniffing.** Without a `format`, the text decides:

1. `WEBVTT` means WebVTT;
2. `<` followed by the TTML namespace means TTML;
3. `[Script Info]`, `[V4+ Styles]`, `[V4 Styles]` or `[Events]` means ASS;
4. a `-->` line means SRT;
5. otherwise the file's extension decides.

Binary formats are refused from their first bytes, the others by their text or extension.

**Encodings.** A UTF-8 or UTF-16 byte order mark is followed; otherwise the text must be valid
UTF-8. An `encoding` argument may also name UTF-8, UTF-16 LE/BE, or a WHATWG single-byte encoding
by any of its labels: windows-1250 to 1258, ISO-8859-x and KOI8. Those are decoded from the WHATWG
index tables (`WHATWG-ENCODING-PROVENANCE.md`). Nothing is guessed, and multibyte legacy encodings
are refused. A decode failure names the line and byte offset.

**Reports.** Every reader reports what it skipped, dropped or approximated, as a row
`{line, column, code, message}`:

| Code | Meaning |
| --- | --- |
| `skipped-cue` | the cue could not be read |
| `zero-length-cue` | start equals end; the cue could never show |
| `dropped` | the feature was dropped, such as ASS `\move` or `\t` |
| `approximated` | the feature was drawn differently |
| `counter` | an SRT counter was missing or out of order |
| `markup` | markup the strict rule does not allow was ignored |

Report rows become operation warnings, written `line L, column C: [code] message`. With
`strict`, any row refuses the whole load. A load that keeps no cue is refused with what was
expected.

**Writing.** SRT and WebVTT times are truncated to the millisecond, so a cue keeps its first frame
and stays off the frame after its last. TTML counts in frames when every time is a whole frame of
the composition's rate, else in ticks. What a format cannot carry, such as styles in SRT, is
reported per cue. `subtitles::writeFile` writes atomically: the text goes to a temporary file in
the same folder, which is renamed over the target. An existing file is refused unless
`overwrite`.

## Operations

Each operation is one transaction and one undo step, except `write-file`, which changes nothing
in the project.

| Operation | What it does |
| --- | --- |
| `bloom.layer.add-subtitles {composition, cues? \| file? \| text?, format?, encoding?, link?, name?}` | a Subtitles layer; outputs `subtitlesNode` and one `cue` per cue |
| `bloom.subtitles.set-cues {node, cues, styles?, language?, provenance?}` | replaces the whole list; a cue naming an id the list has keeps it |
| `bloom.subtitles.insert-cue {node, start, end, text, speaker?, style?, placement?}` | adds one cue with the next id |
| `bloom.subtitles.edit-cue {node, cue \| index, start?, end?, text?, speaker?, style?, placement?}` | a partial edit of one cue |
| `bloom.subtitles.remove-cues {node, cues \| indices}` | removes cues |
| `bloom.subtitles.shift-cues {node, cues?, by}` | moves cues; none may start before 0 |
| `bloom.subtitles.scale-cues {node, cues?, factor: [n, d], origin?}` | `t -> origin + (t - origin) * factor`, exactly |
| `bloom.subtitles.load {node, file \| text, format?, encoding?, link?, strict?}` | replaces the list with one read; outputs one `cue` per cue (the cue count) |
| `bloom.subtitles.reload {node}` | reads a linked list's file again |
| `bloom.subtitles.unlink {node}` | the list stops following its file |
| `bloom.subtitles.write-file {node, path, format: srt \| vtt \| ttml, overwrite?}` | writes the list atomically |

**Ids.** Every edit keeps the ids of the cues it does not remove. Its outputs name the changed and
added cues (`cue`) and the removed ones (`removedCue`), so change events carry the changed cue ids.
A load or reload gives the new cues the ids of the cues they match in the current list
(`document::reconcileCueIds`). The passes match the same start, end and text, then the same start
and end, then the same text. Each old cue is matched at most once, and the other cues take fresh
ids.

**Paths.** The `file` of `add-subtitles` and `load` and the `path` of `write-file` are absolute, or
rooted as `bloom.asset.import` takes them: `/deliveries/episode.srt` is accepted on every platform
and on Windows names that folder on the current drive. A relative path (`episode.srt`, or
`C:episode.srt` on Windows) is refused on every platform.

**Refusals.** A list another node supplies, through an edge into the Cues input or a driver, is
refused with the name of that node. A linked list is refused by every edit except load, reload and
unlink: "These cues follow <path>. Edit the file, or Unlink to edit here." A cue id or index the
list lacks is refused with the way to read the list.

**Footprint.** A cue edit changes only the `cues` parameter. When nothing reads the node's Cues or
Index ports and the language and styles are unchanged, the edit's proven render footprint is the
union of the changed cues' old and new spans (`render_footprint.cpp`). RAM Preview frames outside
it stay warm. Any other edit invalidates the layer's range, as any parameter edit does.

## Linked Files

A list loaded with `link=true` records the file's locator, format, encoding, SHA-256 digest, size
and modification time. The locator is project relative when the file lies under the project's
folder, else absolute. The cues are still held in the project, so a render never reads the file.

**Desktop.** `host::LinkedFileMonitor` (`bloom/host/linked_subtitle_files.hpp`) is polled about
once a second by `ui::ProjectHost`. Each check runs on a blocking-I/O task:

1. It compares the file's size and modification time.
2. Only when those moved, it reads the file and compares the digest.
3. Only when the digest differs, it loads the file.

A finished reload is applied as one transaction, "Reload Subtitles", with origin `host`, and the
status strip shows a notice. Ids are kept. A failed reload keeps the cues and says why. A missing
file is reported once until it returns.

**Headless.** `bloom-cli render` and the export preflight never reload. A linked file that
changed, is missing or cannot be read is warned as `bloom.subtitles.linked-file-changed`, and the
render uses the cues saved in the project. In the export queue it is a preflight warning that never
blocks Start.

## The Node

`bloom.subtitles` (schema 1, `bloom/document/subtitles_nodes.hpp`) holds a cue list in its `cues`
parameter or reads one on its `cues` input. Its outputs are:

- `image`, the cues active at the frame;
- `cues`, the resolved list, so one source can feed several differently styled nodes;
- `text`, the active cues' plain text in list order, joined by line feeds, with the speaker prefix
  when Show speaker is Name prefix; empty when no cue shows;
- `index`, the sorted index of the first active cue, or -1;
- `progress`, 0 to 1 through the first active cue.

The value outputs are evaluated on every frame, also when Burn in is off.

| Group | Parameters (animatable in italics) |
| --- | --- |
| Style | Font; *Size* (em pixels); *Fill*; *Outline color*, *Outline width* (pixels); Box (Off, Per line, Block), *Box color*, Box padding (em), Box radius (em); *Line height*; *Letter spacing* (pixels); *Opacity* |
| Layout | Safe area (Title safe, Action safe, Full frame, Custom with margins as fractions of the width and height); Anchor (nine points); *Offset* (pixels); Max width (fraction of the safe area); Wrap (Balanced, Greedy, Explicit only); Max lines; Overflow (Shrink to fit, Show all lines); Text alignment; Use cue positions; Use file styles |
| Timing | Fade in, Fade out (seconds); Show speaker (Off, Name prefix) |
| Track | Burn in; Export as track; Language; Track name |
| Checks | Max characters per line; Max reading speed (characters per second); Min duration (seconds); Min gap (frames) |

Title safe and Action safe are the composition's safe areas (Composition Settings).

A Subtitles layer made by `add-subtitles` (and so by Import, a drop and Add > Subtitles) sizes its
Size to 5.5 % of the composition's height and its Outline width to 1/360 of it, rounded to whole
pixels with a minimum of one (`kSubtitlesSizeOfHeight`, `kSubtitlesOutlineOfHeight`): 59 and 3 at
1080. A node added on its own from the Nodes Add menu keeps the definition's defaults.

## Layout

One cue is laid out by `runtime::layoutSubtitleCues` (`bloom/runtime/subtitles_layout.hpp`) over
the shared styled text layout (`bloom/render/styled_text.hpp`), which a later shaping engine will
upgrade for Subtitles and Text alike. The Text node's own layout and pixels do not change.

**Styled runs.** The cue's markup becomes runs. Each run has a face, a paint and an underline:

- `<b>`, `<i>` and `<u>` pick the bold, italic or bold italic face and the underline. The four faces
  are resolved once per plan from the node's font, through the font resolver (weight 700 and italic
  variants of the same family).
- A style the font lacks is drawn by the upright face. A missing italic is a synthesized 12-degree
  oblique of the upright outlines (`kSyntheticObliqueSlant`, the same geometry on every route). A
  missing bold draws at the regular weight. The built-in DejaVu Sans has neither, so it always
  synthesizes the italic.
- `<c.name>` colors its run when Use file styles is on: the eight WebVTT color classes, `bg_` classes
  for the box, and the list's own styles by name. The cue's whole-cue style sets its color, outline,
  box color, size and faces.
- A file's colors are sRGB-encoded display values; the layout decodes them to linear values. The
  node's own colors are working-space values. A style size is a fraction of the frame height.
- `<v Name>`, `<lang>` and timestamp tags draw their text; `<ruby>` draws its base text and drops
  its annotation.
- Underline is a rectangle 0.12 em below the baseline, max(0.06 em, 0.5 px) thick.

**Line breaking** follows Unicode UAX #14, revision 16.0.0: the line-break classes are a table
generated from the Unicode Character Database (`tools/text/generate_unicode_text_tables.py`;
provenance in `src/render/UNICODE-PROVENANCE.md`). CJK text breaks between ideographs, no-break
spaces and closing punctuation hold, and a line feed in the text is always a break. A word wider
than the line breaks between characters.

**Wrap.** Every wrap mode measures the line at Max width times the safe area's width (or the cue's
own width when Use cue positions is on and the cue names one).

- **Greedy** fills each line in turn, as the Text node wraps.
- **Balanced** keeps the Greedy line count and picks the breaks that make the widest line as
  narrow as possible. Of equally narrow choices, it keeps the longer line at the bottom.
- **Explicit only** breaks only at line feeds.

**Max lines.** With Shrink to fit, a cue that wraps to more lines than Max lines is laid out again
at 95 %, 90 % and so on of its size, down to 70 %. Past 70 % it draws at 70 % with every line, and
the checks flag it. Show all lines never scales.

**Placement.** The block, with its box padding, is placed in the safe area at the anchor point
(the nine points of the safe area's corners, edges and center), then moved by Offset. With Use cue
positions on, a cue's placement from its file overrides the node's:

- `anchor` picks the point of the block that is placed;
- `positionX` and `positionY` place that point at fractions of the frame (a WebVTT `position` and
  `line` percentage, an ASS `\pos` or margin, a TTML region's origin and extent);
- `lineOffset` moves the block by whole lines away from its anchored edge (a WebVTT line number);
- `width` is the line's width as a fraction of the frame;
- `textAlign` aligns the lines in the block.

**Stacking.** Cues shown together at one anchor stack away from the anchored edge in list order
(start, end, id): the earliest sits on the edge. A cue placed at an explicit position stands alone.

## Rendering

`runtime::CompiledSubtitles` (`bloom/runtime/compiled_plan.hpp`) is the node's compiled
operation. Its operands are the compiled cue list (or the value output that drives it), the styling
operands, the constant settings, the four resolved faces and the composition's frame size and safe
areas.

**Host preparation** (`src/runtime/subtitles_frame.cpp`, shared by every route) selects the cues
active at the operation's time (`start <= t < end`), lays them out together and builds, for each cue
in stack order, its layers:

1. the box: one rounded rectangle around the block (Block) or one per line (Per line);
2. the outline: every glyph's outline stroked at twice Outline width, centered, with round joins
   and caps, so its outer half shows around the fill;
3. the fill: one layer per paint, in the order the paints first appear.

Each layer keeps its author-space paths and stroke, and their reference path coverage
(`render::PathRaster`, 4x4 samples) at the request's proxy scale. Its pixel is the premultiplied
color times Opacity times the cue's fade. This is the audited `prep.parameter.resolution` and
`prep.font.shaping` host work; no pixel is made here.

The node is a **vector leaf**, as Text is: the Layer Output that carries it draws it.

- **Through a layer transform** (a scale, a rotation, a fractional move, a parent, or an identity
  effect between the node and its layer) the Layer Output redraws every layer's paths through the
  full composed matrix at the proxy scales (`PathRaster::transformed`, strokes built in author units
  and then mapped), over the layer's window, and folds them source-over in order before the layer's
  opacity. A scaled or rotated layer therefore stays as crisp as an unscaled one; it is never a
  resample of the node's image. On the CPU this is the Layer Output's vector arm; on the GPU it is
  one coverage command per layer (its bounded `PathRasterCoverageGeometry` rasterized on the
  device), one source-over merge, and one opacity pass when the opacity is not 1 (a single layer
  carries the opacity in its own command).
- **On the integer grid** (identity scale and rotation, a whole-pixel move, no parent) and **where
  an image effect or a Merge reads the node as an image**, the node is its own image: each layer's
  coverage over the cues' ink window at the frame, folded source-over in order over transparent. The
  CPU reference fills those rows; the GPU emits the same coverage commands over the ink window and
  one merge, then the layer's exact integer move and opacity.

There is no per-node readback and no host pixel mask on either path. The node's image covers only
the ink window of its layers, clipped to the frame, and a transformed layer's window follows that
window through its matrix. Its content bounds are the composition frame, so a default layer places
it exactly and the Layer transform moves every cue together. A frame with no active cue, a muted
node or Burn in off publishes no image and its layer draws nothing.

**Fades.** A cue's alpha is the smaller of `(t - start) / fadeIn` and `(end - t) / fadeOut`, each
clamped to 0 and 1, and 1 without fades.

**Coverage.** `CompiledSubtitles` is a Required operation in `gpu_coverage_contract.hpp`, with the
`operation.CompiledSubtitles` and `node.bloom.subtitles` fixtures (italic and colored runs, a block
box, an outline, two stacked cues) and the feature axes `feature.subtitles.box`,
`feature.subtitles.outline` and `feature.subtitles.fade`. There is no exception.

**Cache identity.** The operation key holds the content of the active cues (text, style, speaker
when shown, placement and fade alpha), the resolved styling, the settings, the faces, the list's
styles, the frame, the safe areas and the proxy scales. It never holds the time or the cue ids, so
every frame inside one cue reuses one result, on the CPU and on the GPU (a warm frame dispatches
nothing). A frame in a fade, or a frame that shows other cues, is a new result. A transformed
layer's redraw adds the composed matrix and its window to that content key, so it too is reused on
every frame of a cue while the layer holds still.

**Budgets.** A frame draws at most 16 cues at once and 8,192 glyphs. A frame past either fails, on
every route, with a diagnostic naming the time and the cues: "At 00:00:04.000 17 cues show at once
(cues 3, 4, ...); at most 16 may", or "At 00:00:04.000 the subtitles draw 9000 glyphs (cues 1, 2);
at most 8192 may". An edit never leaves more than 16 cues on screen at once, so the first can only
come from a list another node supplies.

**Cancellation.** Host preparation and every coverage row check cancellation. A cancelled frame
publishes nothing.

**Warnings.**

- A cue holding text in a script that needs shaping (Arabic, Hebrew, the Indic scripts, Thai and
  the others the Unicode Script property names) is a compile warning naming the cue: "Cue 4
  contains Arabic text; Bloom lays text out without shaping, so it will not read correctly". A list
  with more than 16 such cues names the first 16 and counts the rest. Bloom draws such text
  unshaped and left to right.
- A font that cannot be found is the font-missing warning, and the Viewer highlights the layer, as
  for Text.

## Checks

`runtime::checkSubtitleCues` reads each cue against the node's Checks settings. A check is a
readout: it never changes the render. Each cue is laid out on its own at the node's constant
styling (an animated value reads its authored value).

| Code | Flags a cue that |
| --- | --- |
| `characters-per-line` | has a line longer than Max characters per line |
| `lines` | shows more lines than Max lines (after Shrink to fit) |
| `reading-speed` | shows more characters per second than Max reading speed |
| `duration` | shows for less than Min duration |
| `gap` | ends fewer than Min gap frames before the next cue starts |
| `overlap` | overlaps the next or the previous cue |
| `outside-composition` | reaches past the composition's end |
| `unshaped-script` | holds text in a script that needs shaping |

The `subtitles` read adds each check's message to the cue's warnings. The checks need the node as
the composition compiles it, so a node that does not reach the composition's output shows none.

## Export

Burned-in subtitles are pixels and go through every render route like any layer. An export can
also carry the cues as **subtitle tracks**: sidecar files beside the output, and tracks inside the
movie. The output's `ExportOutputSettings::subtitles` (`document::ExportSubtitles`) asks for them:

- `sidecars`: any of SRT, WebVTT and TTML (IMSC 1.1 Text), written for every format, image
  sequences and WAV included;
- `embed`: tracks inside a MOV, MP4 or WebM movie;
- `nodes`: absent for every Subtitles node with Export as track on, or the chosen nodes (whatever
  their Export as track says).

It is absent by default and saved only when set ([project-format.md](project-format.md), "Export
Queue"). The Export panel's Output Module has a Subtitles row (SRT, WebVTT, TTML, Embed in the movie);
MCP `render` and `export` take `subtitles {sidecar, embed, nodes}`; `bloom-cli render` and `export`
take `--subtitles srt,vtt[,ttml]`, `--embed-subtitles` and `--subtitle-nodes`; Python takes the same
arguments; and `export.script` reproduces the setting.

**The track pass.** `runtime::collectSubtitleTracks` (`src/runtime/subtitle_tracks.hpp`) walks the
compiled plan of each output frame in the export's range, with no pixel evaluated, and finds which
cues of which nodes the picture shows: it follows the layer's range, enabled, solo and mute state,
its clock and speed, Time Remap and nested compositions, the same way the render reaches the node.
Each cue's run of output frames becomes a track cue `[first, end)` in output frames; a run that is
interrupted (a trimmed layer, a remap that leaves and comes back) becomes two cues. A track
therefore shows exactly the frames the picture shows. The pass runs on a task, before the first
frame renders, reports progress in frames and stops on cancellation. A muted node, or one that never
shows in the range, exports no track. The export log lists each track, and names a node that
reaches the output but shows no cue in the range, and a named node that does not reach it.

**Sidecars.** `<base>.<lang>.srt|vtt|ttml` beside the output, where `<base>` is the destination's
name without its extension (for a sequence, without its frame number) and `<lang>` is the node's
BCP 47 language. When two exported tracks share a language each gets `.<n>` (1, 2, ...) before the
extension. SRT and WebVTT times are truncated to the millisecond, which keeps a cue on its first
frame and off the frame after its last at any rate; TTML writes exact frame expressions (`<n>f`)
with `ttp:frameRate` and, for a fractional rate, `ttp:frameRateMultiplier`. Each file is written
and parsed straight back in memory before the first frame renders, then, after the output's own
files are published, staged beside the export, read back from the disk, parsed and compared with
its track again, and published atomically under the output's overwrite policy: Replace replaces an existing sidecar,
Don't render never does, and Ask first replaces one only when the output's own files were
confirmed for replacing. A sidecar that would not be replaced fails the output with
`PublicationConflict` before the first frame renders. Each sidecar is listed in the export log and the results with its path, cue count, bytes and
SHA-256 digest.

**Embedded tracks.** MOV and MP4 carry 3GPP Timed Text (`tx3g`), WebM carries WebVTT
(`D_WEBVTT/SUBTITLES`). Each track has the language as ISO 639-2 (the terminology code for MOV and
MP4, the bibliographic code for WebM, `und` for a language Bloom does not know) and the node's
track name as its title, is neither default nor forced (players do not show it over burned-in text
unless asked), and is timed on the video's exact frame rate in MOV and MP4 (Matroska's clock is
milliseconds, truncated as in a sidecar). A `tx3g` sample shows the cues active together as one
text, joined by a line break. The media worker reopens the finished file, reads every track back
and compares the count, languages, titles, cue frames and text with what it was sent; any difference
fails the output. The macOS system encoder writes the same `tx3g` tracks through AVAssetWriter, and
Windows' Media Foundation route muxes through the same FFmpeg code; see
[media-io.md](media-io.md), "Subtitle tracks".

**Preflight.** Asking to embed in MXF, an image sequence or WAV is a blocking preflight issue
(`SubtitleEmbedUnsupported`) that names the sidecar formats instead. The export's results carry a
Subtitles check (`ExportCheck::Subtitles`, `subtitles` in scripting and MCP) with every sidecar and
embedded track, and it fails the output when any of them did not read back as its track.

## Automation

**Time forms.** Every cue time takes Bloom's forms: an integer is a whole frame of the composition
and `[n, d]` is exact seconds. Subtitle times also take a timestamp `"HH:MM:SS.mmm"` or
`"HH:MM:SS,mmm"`, exact in milliseconds, the form an SRT file shows; a leading `-` makes a negative
shift. In Python a time is also a `Fraction` or a float, as exact seconds. Frame counts inside
cue mappings (`cues` of `add-subtitles` and `set-cues`) are converted at the composition's rate
like any other time argument.

**Records.** A cue, a style, a placement and a provenance are mappings: an object that is none of
the other argument shapes, written as `scripting::ValueMapping`. Python spells their members in
snake_case (`position_x`), which reads the same as camelCase.

**Large lists.** A large list travels as an MCP data file (`{"fromFile": "/abs/cues.json"}`, up to
16 MiB) or through `load`.

**Limit refusals.** A list, text, file or style table past its limit is refused before anything
applies. MCP answers with the limit's own code (`bloom.mcp.limit.subtitle-cues`,
`-cue-text-bytes`, `-file-bytes`, `-styles`) and `limit {name, value, actual, unit}`. Python's
`OperationError` diagnostics carry `limit {name, value, actual}`, and `bloom-cli run` names the
limit on the error line.

**The `subtitles` read** (`scripting::readSubtitles`) is shared by MCP `query kind=subtitles` and
Python `bloom.data.subtitles`.

- Without a node it lists the composition's Subtitles nodes.
- With a node it gives the list's summary: count, language, provenance, the linked file's state
  (`none`, `in-sync`, `changed`, `missing` or `unreadable`) and the styles.
- It then gives a page of cues. Each has its id and index, start, end and duration as
  `{frames, seconds}`, a display timestamp, text and plain text, speaker, style, placement, and
  warnings: an undefined style, more than 16 cues showing at once, and the cue's checks.
- `at` keeps the cues showing at a time, and `from` and `to` keep those overlapping `[from, to)`.
- A cursor is valid only at the revision it came from.
- `format` `srt` or `vtt` answers the whole list as that file's text.

## Editing In The Application

The interaction contract is `docs/ux/interaction-model.md`, "Subtitles"; this section is how it is
built.

**Edits.** `src/ui/subtitles_editing.hpp` holds every artist edit (add at the playhead, delete,
split, merge, set start or end, cell edits, shift, scale, Edit as Text, load, reload, unlink). Each
builds one transaction from the operations above, so each is one undo step and every refusal is the
command's own. The Properties cue list, the Timeline cue blocks and the Viewer call the same
functions. Cue times are key times of the cue list (the layer's clock); the surfaces convert them
with the session's key clock and show them in the composition's frames or timecode.

**The picked cue** is session state (`CompositionSession::selectCue`, `cueSelectionChanged`), not
document state, so Properties, the Timeline and the Viewer agree on it and Undo never touches it.

**Off the interface thread.** `ui::SubtitlesInspector`, owned by the session, runs on its own task
scheduler worker. For every Subtitles node of the composition it compiles the snapshot (cached per
revision), places the cues showing at the playhead (`runtime::placeSubtitleCues`, the render's own
layout at the UI-sampled style), checks the linked file's state, and, for the nodes a surface asked
about in detail (the node open in Properties), runs the checks over the whole list. It refreshes
on snapshot, time, composition and live-value changes; a newer request supersedes an older one.
Reading a file for Load File, Import or a drop, and parsing Edit as Text, run on the same worker.
The Viewer's outlines, the Properties Checks column and the node card's file state read its
results.

**The cue list** is a `QTableView` over a model of the node's list: it builds no widget per cue
(only an open cell editor), so a 10,000-cue list costs one view.

**Previews.** A Timeline cue drag previews through the session's value edit on the cue-list
parameter (the whole list, re-sorted), which the preview controller renders as an interactive edit;
the release cancels the preview and commits one Move Cue or Retime Cue. A Viewer drag previews the
Offset the same way.

**Track-only guide.** With Burn in off, the Viewer draws the placed cues as an overlay (white text on
a dark plate, dashed outline) while View > Guides > Show Track-Only Subtitles is on (the default;
the `viewer/overlay/track-only-subtitles` setting). It is an overlay painter, so it is never in the
display buffer, a RAM Preview frame or an export.

**Import.** A subtitle file among the paths of File > Import or a drop (`.srt`, `.vtt`, `.ttml`,
`.dfxp`, `.ass`, `.ssa`; a plain `.xml` is never claimed) is read on the inspector's worker and
becomes one `add-subtitles` with `link=true` at the composition's start. It is not an asset.
