# Export

Exports run from the **Export** panel, a queue saved in the project, much like a compositor's
render queue. Open it by choosing **Export** in any panel's editor picker, or choose **File → Add to
Export Queue** (Ctrl+M). If an Export panel is already open Bloom brings it forward; otherwise the
Timeline's panel becomes Export, and with no Timeline a new panel opens along the bottom ([Workspace
presets](workspace-presets.md#opening-the-export-panel)). The same **Add to Export Queue** item is
on a composition row in Assets, and you can drag a composition from Assets onto the list. Bloom adds
a job for the composition, reveals the panel and selects the new job. A job renders one composition
range to one or more outputs, one job at a time in queue order, in the background while you keep
working. An agent connected over [MCP](mcp.md) brings the panel forward the same way only when it
calls `export.reveal`; its queue edits never reveal the panel on their own.

The panel is laid out like a compositor's render queue. At the top, **Current Render** says in
one line what renders now ("Rendering Shot010 · frame 404 of 2156 · 0:28 elapsed · about 2 min
left", or **Idle**); open it with its caret for a progress bar. Below it is the render list, with
the columns **Render** (a check box: include the job in **Start All**), **#** (queue order),
**Composition**, **Status**, **Started** and **Render Time**. While a job renders, a thin bar sits
in its Status cell. Drag a row to change the order. Each row's **⋯** menu (or a right-click) has
**Start** (or **Pause**, **Resume**, **Stop** or **Render again**, whichever fits), **Details…**,
**Duplicate**, **Remove**, **Move up**, **Move down**, **Copy as bloom-cli command** and **Copy as
Python**. On a narrow panel Started and Render Time give way first.

Open a job with its caret to see its settings, one line each, as in a compositor's render queue:

- **Render Settings:** the range, for example **Work area · 0–239**. Click it (it is blue) for the
  **Render Settings** dialog; the **▾** before it picks **Work area**, **Whole composition** or
  **Current frame** at once.
- **Output Module:** the format, for example **ProRes 422 HQ**. Click it for the **Output Module**
  dialog; its **▾** picks another format, video first, then image sequences. **+** adds an output
  and **−** removes this one (a job keeps at least one output). **Output To:** shows where it saves;
  click it to choose a file, or use its **▾** for **Default name** or a folder you saved to before.

Each line opens with its own caret to one gray line of what it comes to: the frames and timecode,
the first and last file, the size Bloom can estimate (nothing when it cannot) and anything that
blocks it. **Start All** is the one blue button while a checked job is ready, and **Stop All**
shows only while something renders. A control that cannot act now says why in its tooltip.

Rendered output keeps alpha by default. To render the composition's Background Color behind the
picture, set the output's **Background** to **Composition color** (see [Background](#background)).

## Jobs and outputs

An output keeps alpha unless its **Background** is **Composition color**: by default a
composition's Background Color is only shown in the Viewer behind the composition, so exports and
renders of a format that can carry transparency keep it, as in other layer-based compositors.

### Background

**Background** in the Output Module's first group (every format except WAV) chooses what each frame
is composited over:

- **Transparent** (the default) keeps alpha. A format without alpha (H.264, DPX and the like) shows
  the picture as it would look over black.
- **Composition color** composites every frame over the composition's **Background Color** before
  the display or output color transform, in the working color space, so the output is opaque and
  matches what the Viewer shows behind the frame. The Background Color you pick is converted into
  the working space the same way the color chip converts it. Only the exported composition's own
  color is used: a nested composition never draws its background into its parent.

The background is part of the export only. The Viewer, RAM Preview and nested compositions are
unchanged, and a job's **Details…** and log say "Opaque over composition background". It is saved
with the output and with a preset you save from it, and the GPU and the CPU reference render it
alike. The same choice is the `background` argument (`transparent` or `composition`) of the MCP
`render`, `capture` and `export` tools, `background=` in Python's `bloom.render`, and
`--background` in `bloom-cli render`.

A new job renders the composition's work area (or the whole composition when it has none) to one
**H.264 MP4** output at `Exports/{name}.mp4` beside the project. The H.264 encoder ships with
Bloom, so there is nothing to install first. The default is never an image sequence and never the
last preset you used. An unsaved project has no
default destination; Start then says "Choose where to save this output".

Each job has a range, set in **Render Settings** (**Work area**, **Whole composition**, **Custom**
with a start and end frame and their timecode, or **One frame**; the dialog shows where the range
starts and ends and how long it is), and the **Render** check box. Each output has a format, its
quality settings, where it saves, what to do when a file is already there and **Add to Assets when
done**, set in **Output Module**. A dialog's **OK** is one undoable edit, and **Cancel** changes
nothing. Every edit is undoable and saved in the project. Run status, progress, ETA,
logs and verification are not saved: a reopened project shows every job **Ready**. A running job
cannot be edited ("Stop the job to change it; it keeps the settings it started with").

An image output writes one file per frame, or a single still when the range is one frame.

## Presets

Formats are listed in four groups:

- **Video**: H.264 MP4 (default), H.264 MOV, Review H.264 (Rec.709, look on), Vertical video H.264
  (H.264 High in MP4 at 12 Mb/s, for 1080 x 1920 short-form video; set the composition to that
  size with the 1080 x 1920 composition preset), HEVC MP4 and MOV,
  AV1 MP4, AV1 WebM, VP9 WebM, ProRes 422 Proxy, LT, 422, HQ, 4444 and 4444 XQ MOV, DNxHR HQ MXF.
- **Image sequences**: VFX handoff EXR (ACES2065-1, PIZ, from 1001, `####`), OpenEXR, DPX, PNG
  and TIFF sequences.
- **Audio**: PCM WAV/BWF (RF64 when large).
- **My presets**: your own presets, saved per computer outside any project.

To make one, open an output's **Output Module**, set it up, and click **+** beside **Format**
(**Save as preset…**). Name the preset and choose **Save**. A name must not be empty or match a
preset already in the list (ignoring case); saving never replaces a preset. The preset holds the
output's format and its **Quality**, **Numbering** and **Cached frames** settings; the destination,
**If a file is there** and **Post-render** stay with the output.
It is listed under **My presets** in every Format list and chosen for the output you were editing;
**OK** gives the output that preset, and **Cancel** leaves the output as it was but keeps the
preset. Saving a preset is not undoable and does not change the project.

Choosing a format copies its settings into the output, so a project exports the same way on a
computer that lacks the user preset it came from. Every format is listed and chosen the same way;
the panel never asks you to install anything. If a format still cannot run when the job starts, the
job reads **Failed** with a plain sentence saying why, like any other failure.

The **Output Module** dialog is grouped like a compositor's output settings: the preset's own group
(**Format**, **If a file is there**, **Post-render**: **Add to Assets when done**, and **Cached
frames**: **Use cached frames**), **Quality** and, for image sequences, **Numbering** (**Start
number**, or **Use the frame numbers**, and **Digits**). **Quality** shows only what the chosen
format has: **Profile** where a format has several, **Compression** for OpenEXR, **Color space** for
OpenEXR and DPX, **Display** and **View** for movies, PNG and TIFF, **Bake in the look**, and
**Include audio** and **Normalize loudness** for movies and WAV. H.264 and HEVC formats also show **Bit rate**, the target in kb/s
(12000 kb/s is 12 Mb/s); it starts at the codec's default, 30000 kb/s for H.264 and 20000 kb/s for
HEVC, and the file's average rate lands near it. **Normalize loudness** brings the sound to a target loudness. **Target** is in LUFS (-16 is a common
streaming level, -23 a broadcast level) and **True peak** is the ceiling in dBTP that the sound is never
allowed to pass (-1 by default). Bloom first measures the whole mix, then gives it the gain that reaches
the target and holds the ceiling with a limiter, so a loud peak is turned down instead of clipping. The
job's details report the loudness measured before and delivered after, the true peak and how far the
limiter reduced it. Silent audio is left alone. Normalizing renders the sound twice, which adds time to
an export with a lot of audio.
**Color space**, **Display** and **View** are lists of what the
project's color setup offers, with **Project default** first, so a name cannot be mistyped.
**Project default** is the Viewer's starting view (plain sRGB, **Un-tone-mapped** under ACES CG); the
output stores that name, so the result never depends on the configuration's own default view. The
encoder choice, audio sample rate and file-name pattern an output already has are kept and
used; the panel does not edit them.

- **H.264** is encoded by OpenH264, which ships with Bloom on Linux, macOS and Windows; nothing is
  downloaded or installed. On a computer with an NVIDIA graphics driver, Bloom encodes on the GPU
  (NVENC) unless the encoder is set to **Software**. With **Hardware** chosen on Linux, a VA-API
  render node is tried next. When no GPU encoder answers, OpenH264 is used without asking. On macOS
  VideoToolbox encodes first.
- **HEVC** is encoded by Kvazaar, which ships with Bloom on Linux, macOS and Windows. An NVIDIA GPU
  (NVENC) encodes first when its driver answers, then on Linux a VA-API render node that encodes
  HEVC, and Bloom falls back to Kvazaar without asking. Every encoder writes the profile you choose:
  **Main** is 8-bit and **Main 10** is real 10-bit, whether a GPU or Kvazaar encodes it.
- **AV1** is encoded by SVT-AV1 and **VP9** by libvpx, both shipped with Bloom on Linux, macOS and
  Windows. Each has an **8-bit** and a **10-bit** profile, and both write real 10-bit. They are
  encoded on the processor at a fixed quality for web delivery; the bit rate and the encoder choice
  do not apply to them, and the Bit rate field is not shown.
- **ProRes** is written by the system's encoder on macOS. If the system refuses an export, Bloom
  asks whether to write it with its own FFmpeg encoder instead, and does not switch without
  asking. On Linux and Windows, and on macOS with the encoder set to **Software**, ProRes is
  written by FFmpeg; that is not an Apple-authorized ProRes implementation.
- The **VFX handoff** and **Review H.264** deliverables need the built-in ACES CG configuration
  with a pinned revision.
- **DPX** sequences are 10-bit or 16-bit RGB files, one per frame, with no alpha (the color is what
  the frame shows over black). The header carries a linear transfer and Rec.709 primaries or
  unspecified primaries; the output color space is recorded by the job, not the file.

**Include audio** is on by default, and a movie then always carries an audio stream, silent when
the composition has no audio layer, because players and delivery specifications commonly expect a
track. Turn **Include audio** off (the `audio` argument of the `export` tool) for a movie without
one.

### Containers and codecs

Each preset names the container and codecs it writes:

| Container | Video codecs (profile) | Audio |
| --- | --- | --- |
| MP4 | H.264 (High), HEVC (Main or Main 10), AV1 (Main) | AAC-LC at 192 kb/s, at the output's sample rate |
| MOV | H.264 (High), HEVC, ProRes (Proxy, LT, Standard, HQ, 4444, XQ) | PCM 16-bit, or 24-bit when an audio source is deeper |
| WebM | VP9 (Profile 0 or Profile 2), AV1 (Main) | Opus at 192 kb/s, always 48 kHz whatever the output's sample rate |
| MXF | DNxHR (HQ) | PCM 16-bit, or 24-bit when an audio source is deeper |
| WAV | none | PCM 16-bit or 24-bit, as the profile says |

AAC and Opus are lossy; PCM is exact. The MCP `export.presets` tool lists the same facts for each
preset (`container`, `videoCodec`, `videoProfile`, `audioCodec`, `audioProfile`,
`audioBitrateKbps`, `audioSampleRate`, `pcmDepth` and an `audio` sentence), and `help` for
`export` and `export.presets` names the audio codec per container.

After a movie or WAV is written, Bloom reopens it and reads back the streams it actually holds.
A job's log has one **Written stream** line per stream, for example "video prores HQ 1920x1080 24 fps
…" and "audio pcm_s16le 2 ch 48000 Hz …", and the result of the MCP `export` tool and
`export.status`, Python's `bloom.render` result and `bloom-cli export`'s JSON list them as
`streams`: each one's kind, codec, profile, width, height and frame rate (video) or channels and
sample rate (audio), and its bit rate in bits per second. An export whose file holds other streams
than the preset declares fails verification.

Movie containers are tagged from the output's color intent: an sRGB display is tagged with the sRGB
transfer, a Rec.1886/Rec.709 display with BT.709, P3 and Rec.2100 displays with their own code
points.

## Destinations

**Output To** is relative to the project folder (moving a project with its renders keeps it
working) or an absolute file. The default, `Exports/{name}.<ext>`, follows the composition's name;
the panel always shows the name it resolves to (`Exports/Shot010.mov`), never the token. A script may
also use `{job}` (job name), `{format}` (`mp4`, `prores`, `exr`, ...) and `{date}` (frozen when the
job starts), and a sequence's naming pattern `<base>`, `<ext>` and exactly one `####` or `{frame}`;
**Start number** changes file names, not which frames render. The Output Module line's summary shows
the first and last file before you start. Unknown tokens, a name that becomes a folder, or a changed
extension are refused.

Before Start, Bloom checks each destination in the background ("Checking where it saves…"): whether
its folder can be written (missing folders are created), how much space is free, and whether files
already exist. A size estimate appears when the format's size can be estimated honestly; when the
estimate does not fit the free space, Start is blocked.

## Replacing files

**If a file is there** is **Ask first** (the default), **Replace** or **Don't render**. When the
file exists under Ask first, the job reads **Waiting for your OK**, and **Start** or **Start All**
asks once: "Replace Shot010.mov?" with **Replace**, **Choose another name** (the file chooser) and
**Cancel**. Several files are asked about in one question. The confirmation covers exactly the files that were checked; if they change before the
export publishes, Bloom refuses to replace them, and a file that appears after **Start**, such as a
frame another program writes, is not replaced either. **Don't render** holds for every file the
output writes, movies and WAV included. A movie or WAV is checked before it replaces anything, so
one that fails verification leaves the existing file as it was. Files this session itself exported
for the output need no confirmation on a re-run. Bloom never replaces a file silently.

## Running

**Start All** queues every checked job; **Start** in a row's menu queues that job alone. **Pause**
waits for the current frame to finish (**Resume** continues it); **Stop** returns a waiting job to
Ready, or stops a running one. A stopped movie or WAV leaves the destination unchanged; a stopped
sequence keeps the frames it already published. **Render again** starts a finished job again. The Jobs panel shows a running export as one row with **Show in Export**.

The Viewer always comes first. While you scrub, play or build a RAM preview, an export only uses
spare GPU capacity and may wait with no time left shown; it continues at full speed when the Viewer
is idle. That wait is shown only while the Viewer is actually rendering, and the job reads **Paused
while you work in the Viewer** in plain gray, not in a warning color. When a frame does not fit the
memory other active work holds (after Bloom's caches gave back theirs), the job reads **Waiting for
memory** with the amount needed. Waiting and paused time do not count toward the export's time
limits. Neither is a fault.

A composition can have any number of audio layers. An export decodes a sound only while the
exported range plays it: a layer that is silent over the range is never read, and a sound that has
stopped playing gives its memory back a second later. If the memory for a sound is still refused,
the job fails with a message that names the audio asset, the bytes it needed and the limit that
refused them, for example "the processing memory has 1048576 of 134217728 bytes free".

An MP3 plays from its first sound in the export as it does in preview. Many MP3 files, such as
those from text-to-speech services or FFmpeg, declare the encoder's start delay in a header at the
start of the file; the export skips that delay, so the sound neither starts late nor fails to
decode. A sound file that ends before the length its own header declares (a download cut short,
say) fails the export with a message that names the audio asset, instead of publishing the missing
part as silence.

**Worker memory cap** and **Worker threads** in Preferences (Memory & Caches) are opt-in limits on
the helper process that encodes a movie or WAV; see [Preferences](preferences.md#export-workers).
With a cap set, a job that reaches it waits for the worker instead of using more memory, and fails
with a message naming the memory and the cap if the worker still cannot go on. A job's result
reports the worker's peak memory.

Quitting, or closing, creating or opening a project, while exports run asks "Stop the running
exports?" and says how many export jobs are running or waiting. **Stop and Continue** stops them
(files already published stay) and carries on; **Cancel** is the default and leaves everything as it
was. Saving the project during an export is allowed.

## Verification

The old approval prompt is gone. Before Start each output shows its preset's expected
preservation; each frame's analysis is accepted only when it fits that declaration. After
publication the job's **Details…** say in plain lines what was checked: **File opens**, **Right
number of frames**, **Colors tagged as** the color space it was saved in, and **Rendered on the
GPU** or **Some frames rendered on the CPU**. A check that failed or warned (timing, container,
audio, file names, whether the saved file changed, whether it could be put in place) says so in a
sentence with what was expected and found. A frame that had to be evaluated on the CPU is never
shown as a GPU frame.
The **File opens** check decodes the movie and compares it with what Bloom rendered, after taking
the codec's own 4:2:2 or 4:2:0 color sampling into account, so small saturated detail such as thin
colored text passes while damaged pixels or wrong color tags still fail the export.
Under **File opens**, a movie's **Details…** show how far the reopened pixels are from the render,
on a scale of 0 to 65535: the average difference, the largest difference after the top 0.01% of
each frame's samples are set aside (the two the check judges), and the largest single difference,
which is reported but not checked. The tooltip names the tolerance profile, and **Copy details**
adds its digest.

AAC and Opus audio are checked the same way: Bloom decodes the audio it just wrote and compares
it with the audio it gave the encoder. These codecs do not return the same samples, so the check
does not compare sample by sample. It requires three things of every channel:

- The file declares the encoder's start padding, so a player that honors it starts the audio on
  frame 0.
- Below 3 kHz, where these encoders keep the waveform, the difference between the decoded audio
  and the source stays at least 6 dB under the source. Audio that is late or early by a few
  samples, comes from another source, has its channels swapped or is silent fails this, at any
  level.
- In every stretch of 2048 samples, the decoded audio is within 3 dB of the level of the source
  in the same or the neighboring stretch, so added noise, a dropout or a level change fails.
  Sound under -60 dBFS counts as silence.

Loud transients such as drum hits pass, although single samples at a hit can differ a lot. The
sample count and the audio's duration are still exact. The check is tolerance profile
`bloom.aac-f32.v2`; PCM audio in MOV, MXF and WAV is compared exactly.

**Add to Assets when done** adds the checked result to Assets (a movie as a video asset,
a sequence as a sequence, a still as an image, a WAV as audio) with one undoable import. A file
already in Assets is refreshed rather than duplicated. A failed import never fails the export; the
job's **Details…** say "Couldn't add to Assets" and why, on their own line.

## Subtitles

Subtitles you burn in are part of the picture and export like any layer. An output can also deliver
them as tracks a player can turn on and off. In the Output Module, the **Subtitles** row has:

- **SRT**, **WebVTT** and **TTML**: a sidecar file of each kind beside the export, for every
  format, image sequences and WAV included. A file is named after the export and the track's
  language, `Shot010.en.srt`; when two tracks share a language they are numbered,
  `Shot010.en.1.srt` and `Shot010.en.2.srt`.
- **Embed in the movie**: a subtitle track inside an MP4, MOV or WebM movie, with its language and
  the node's **Track name** as its title. A player does not show it unless the viewer turns it on.
  MXF, image sequences and WAV carry no subtitle track, so the choice is unavailable for them with
  that reason; use the sidecar files instead.

Each Subtitles node with **Export as track** on becomes one track (a script can name the nodes
instead). The track shows exactly the frames the picture shows them on: a layer that starts late,
is trimmed, has a time remap or sits in a nested composition moves its cues with it, and a cue that
is cut by a trim ends where the layer does. A muted node, or one that never shows in the range,
exports nothing. The job's log lists each track it wrote, and names a node that reached the export
but showed no cue, or a node the script named that does not reach it. SRT and WebVTT store times in milliseconds, so a cue
starts on its first frame and is off on the frame after its last at any frame rate; TTML stores
whole frames.

The sidecar files follow **If a file is there** like the export itself. Under **Ask first** an
existing sidecar is replaced only when you confirmed replacing the export's own files; otherwise
the job stops before it renders and says which file is in the way.

After the export, Bloom reads every sidecar and every track in the movie back and compares each
cue, its frames and its text with what it wrote. **Details…** then read **Subtitles match the
picture**, with each file in the tooltip; a file or track that differs fails the job with what
differed. The job's log lists every file with its SHA-256.

## Scripting and the command line

`bloom-cli export --project shot.bloom --all` renders the saved queue headlessly and prints the
verification and import results as JSON; `--job <id>` selects jobs, `--replace` confirms existing
destinations, and `--save` writes asset imports back to the project; each movie's and WAV's
written streams are part of the JSON. `bloom-cli render` and `export` take
`--subtitles srt,vtt,ttml`, `--embed-subtitles` and `--subtitle-nodes` for the subtitle tracks,
and `bloom-cli render --background composition` renders over the composition's Background Color. See
[headless scripting](scripting.md) and [MCP](mcp.md). **Copy as bloom-cli Command** and **Copy as Python** on a job's context
menu produce a script that recreates the job and starts it.

## Platforms

Export runs on Linux, macOS and Windows. Each system uses its own media framework where it is
present and falls back to Bloom's bundled FFmpeg otherwise, so every preset works everywhere and
a project prepared on one system exports unchanged on another. The
[media I/O contract](../architecture/media-io.md#time-based-export-media-4) describes preservation
bounds.
