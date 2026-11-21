# Subtitles

Status: canonical

Updated: 2026-11-21

Issue #752. A Subtitles node (`bloom.subtitles`) draws a list of timed cues. This document is the
specification of the cue list value, the file formats Bloom reads and writes, the edits, the linked
file, and how agents and scripts reach all of them. The project file arm is in
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
  warnings: an undefined style, or more than 16 cues showing at once.
- `at` keeps the cues showing at a time, and `from` and `to` keep those overlapping `[from, to)`.
- A cursor is valid only at the revision it came from.
- `format` `srt` or `vtt` answers the whole list as that file's text.
