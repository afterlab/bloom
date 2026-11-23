# Changelog

All notable changes to Bloom are written here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and Bloom follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0-alpha.4] - 2026-11-24

This alpha lets exports handle many audio and movie layers, keeps the true-peak limit in delivered
AAC and Opus files, makes MCP exports work from a project file, and improves the Windows update.

### Added

- Export frame reuse. With **Use cached frames** on, an export output reuses frames an earlier
  export in the same session already rendered with the same settings (another output of the job,
  or the same export run again), and the file is identical to a fresh render. Viewer and RAM
  Preview frames are not used. A render request from a script or an agent always renders fresh.
- A render or export from Python, `bloom-cli` and MCP covers the composition's whole range when
  you give no first and last frame.
- For agents over MCP: a project-file session can export a movie, with progress, wait and stop on
  that file's own export queue. When the file is the project open in Bloom, the call attaches to the
  open project and says whether it differs from the file, so you can reload it.
- When a movie layer has to wait for a free source copy during an export, the export log says which
  layer waited and for how long.

### Changed

- Every release is now published as the newest release on the releases page.
- The true-peak limit now holds in the delivered file. Bloom measures the AAC or Opus file it
  wrote, lowers the limiter when the encoder pushes a peak over the limit, and renders the file again
  with more headroom rather than publish one that is still over, on every export route. The export
  log reports the true peak measured on the delivered file.
- `bloom.export.add-job` takes outputs as objects. `bloom-cli` prints a failed export's class the
  way `bloom-cli render` does.
- A ranged image-sequence import is named after the sequence's base name and reports its frames.
- When a source file changes or disappears while an export runs, the export stops and says so,
  instead of saying it hit a bug in Bloom.
- An export that is refused for lack of memory names the asset, the bytes it needed and the limit.
- The Windows update window shows the release notes as dark text on a light page in a clear font,
  and names your installed version in full (for example 0.1.0-alpha.3). A progress window stays on
  screen from the moment Bloom closes until the new version opens, Bloom reopens by itself, and the
  previous version is kept beside the new one. If you start Bloom by hand during an update, it
  brings the update window to the front and ends at once instead of running the version being
  replaced. When an update cannot install, the version you had starts again with a notice that
  says why.

### Fixed

- An export of a composition with many audio layers no longer fails with "Audio backing admission
  refused". Bloom opens only the sounds a range plays and releases the ones that are finished.
- An export of a composition with many movie layers no longer runs out of video-snapshot quota.
  Bloom reads movies a frame needs, releases a layer's movie when it leaves the range, and shares
  one copy between captures of the same source.
- MP3 voice-over files that carry an encoder delay, such as variable-bit-rate files with a tag,
  export instead of failing with "its audio stream stopped decoding". A truncated sound file is
  refused by name instead of playing silence for its missing part.
- A movie whose audio starts after its video exports its audio.
- Export job ids stay exact in JSON, so stopping or waiting for an export no longer reports an
  unknown job.
- An MCP export without first and last frames no longer fails with "Expected a nonnegative
  integer".
- `bloom.layer.add-from-assets` works on a composition whose Output is fed through effect nodes
  after the Merge.
- An MCP export on the export-queue route returns the same preservation report as a scripted
  render.
- The layer bounds of a rotated, trimmed path are the box of its ink, not a far larger box.
- A project-file MCP session is no longer refused as already open while the window shows an older
  copy of the file.

## [0.1.0-alpha.3] - 2026-11-23

This alpha adds subtitles, styled and animated text, sharper scaling and Image Switch, and makes
exports state what they wrote.

### Added

- Subtitles. A Subtitles node reads SRT, WebVTT, TTML and ASS files into cues (ASS and SSA are
  read as a reported subset of their styling) and draws them. You can import or drop a subtitle
  file as a linked layer, edit cues in Properties, the Timeline and the Viewer, and export
  subtitles as sidecar files or as tracks embedded in MOV, MP4 and WebM. An export checks the
  written tracks by reopening the file. Agents reach the same features from MCP, Python and
  `bloom-cli`.
- Styled text. Text Source takes markup for bold, italic, underline and colors, an outline with
  joins, and a background box per line or for the whole block. Its styling rows are grouped and
  hide when nothing reads them.
- Text Animator. A node that animates characters, words and lines with four range selectors and
  nine properties, reveals text without reflowing it, and has a cursor. Fixed-width digits keep
  counters from jittering.
- Image Switch and Channel Set Switch nodes. Eight inputs, an animatable index that clamps or
  wraps, and a selected output. Only the chosen branch is evaluated.
- Sharper scaling. Transform 2D, Resize, Fit, Layer and Corner Pin filter an image when they
  shrink it, and a layer has a Filter choice. Snap and a pixel-art mode keep hard pixel edges.
- Coordinate spaces. A space is a value that Function Plot, Axes and Axis Tick can use, with
  handles in the Viewer. Agents can read a node's outputs as values over MCP and Python.
- Path Stroke dash patterns of up to three dash and gap pairs, with round dots, and exact import
  of SVG `stroke-dasharray` values of up to six lengths. Strokes are identical on every platform.
- An export background. An export can render over a transparent background or the composition
  color, from the Output Module, MCP, Python and `bloom-cli`.
- Export presets state their container and codecs, and every export result lists the streams read
  back from the written file.

### Changed

- When Bloom cannot measure the computer's capacity before an export, such as on Linux without
  `/sys`, it says what it assumed in the export log and the Profiler instead of stalling.
- A movie or scripted render that uses the CPU reference path now says why.

### Fixed

- ProRes Proxy, LT and 422 exports were written as ProRes HQ. Each profile is now written as
  chosen.
- A GPU job no longer overwrites the Viewer's recorded need for the graphics device.
- A crash report no longer fails to read the stack of a thread that was exiting on macOS.
- Font downloads end before Bloom exits, and a preset that could not be saved is kept and
  retried.

## [0.1.0-alpha.2] - 2026-11-20

This alpha makes Bloom dependable for large projects and for agents that build them, and records
crashes so they can be fixed.

### Added

- Crash reports. When Bloom, `bloom-cli` or `bloom-mcp` quits unexpectedly, a short text report
  and a minidump are written to a crash folder on your computer. Crash reports stay on your
  machine: Bloom never sends them anywhere. The next time you start Bloom, it says that Bloom quit
  unexpectedly and offers to open a prefilled GitHub issue, copy the report or open the crash
  folder, so you decide what to share. Bloom keeps the 20 newest crashes, none older than 30 days.
- Quiet agent work. Renders, exports and captures an agent starts over the MCP server play no
  sound and post no notification one by one, and an agent's refused edit plays no warning. Bloom
  tells you once what the agent did, while you are in another application. **Sounds and
  notifications for agent activity** in Preferences turns this summary off.
- Agent sessions. An agent can open a session that keeps its work quiet, refreshes the Viewer when
  the session ends and makes the whole session one undo step. Your own edit during a session ends
  the grouping safely.
- For agents over MCP: a long edit can run as a job with progress and cancellation, a repeated
  request with the same key is answered once, a busy answer says when to try again, change
  events can be followed as a stream instead of polled, and one operation can key many
  parameters at once, also from Python and `bloom-cli`.
- Bloom tells an agent every limit on its requests when it connects, and a refused request names
  the limit, its value and how far the request went past it.

### Changed

- The window stays responsive while an agent builds a large project. Edits are prepared away from
  the window, adding keyframes no longer slows down as a project grows, and panels redraw at most
  once per frame during a burst of agent edits.
- Undo keeps many more steps in large projects, because a step stores only what it changed. When
  older steps have to go, Bloom says so once, without a warning sound.
- Requests over the MCP server are much faster: a small request is answered almost at once, and
  listing the layers of a large composition takes a fraction of a second instead of tens of
  seconds.

### Fixed

- Save and Save As failed on very large projects, such as one with about 274,000 keyframes. They
  now save and reopen. Bloom refuses an edit that would make a project too large to save, at the
  time of the edit and with the reason, so a project you can build always saves.
- A failed save, Save As or Save a Copy now says at which step it failed and why, in the window
  and to an agent. On Windows, a save to a path that is a folder now says so.
- A save started by an agent no longer opens a dialog over the window or leaves Bloom busy for the
  agent's next request.
- Corner Pin no longer smears a long vertical streak when a layer is turned almost edge-on.
- A Crop on an image taller or wider than its composition no longer makes every frame fail. A
  Crop that removes the whole picture names its node, layer and parameter.
- A capture or export no longer fails every frame with "could not be published" after the
  system clears Bloom's cache folder, and a frame that cannot be published says why.

## [0.1.0-alpha.1] - 2026-11-17

This is the first alpha. Expect rough edges, and keep copies of projects that matter to you.

### Added

- Motion graphics and compositing in one workspace: a composition is both a layer stack and a
  node graph, so you can work in whichever view suits the job.
- Keyframe animation with easing, and every edit, including a whole drag, is one undo step.
- A large set of nodes for sources, color, blur, stylize, warp, channels, masks and mattes, paths
  and shapes, fields, number lists and audio.
- Text layers, shapes with animated strokes, and SVG files that import as one layer or as
  editable shapes.
- Color management with OpenColorIO and the built-in Bloom Neutral configuration, LUT assets, and
  a Viewer that shows your picture through the display transform on the graphics card or the
  processor.
- Images, image sequences, video and audio import. Footage is read in separate worker processes,
  off the main window, so the interface stays responsive.
- Export of stills, image sequences and video through an Export queue with presets, checks of the
  finished file and audio mixing, including loudness (LUFS) targets.
- RAM Preview for real-time playback of what you have built.
- A Scopes panel with Waveform, Parade, Vectorscope, Histogram and CIE chromaticity. Scopes
  measure the picture shown in the Viewer, so opening the panel or scrubbing a cached preview
  never renders a frame again.
- A disk cache that keeps local copies of footage from slow drives and network shares. You set
  only how much disk space to keep free. Bloom uses the rest, gives it back when other programs
  need it, and shows the live figures in the status bar and in Preferences. Movies with audio,
  thumbnails and sound analysis read through the same cache.
- Python scripting, the `bloom-cli` program for rendering without a window, and an MCP server so
  agents can work with your projects.
- Panels you can arrange freely: Viewer, Timeline, Nodes, Properties, Assets, Jobs, Export,
  Scopes, Profiler, MCP Server and Script, with workspace presets.
- Installers for every platform:
  - macOS (Apple silicon): a disk image to drag Bloom to Applications, and a package for
    machines that render without a window.
  - Windows (64-bit): an installer for your own account that needs no administrator rights, and a
    portable zip.
  - Linux (64-bit): an install script, a tar archive, an AppImage and a ready package recipe for
    Arch Linux.
- Updates inside Bloom. **Help > Check for Updates** looks for a new version on request. Bloom
  also checks once a day, unless you turn that off in Preferences, and it sends nothing but the
  request. Every download is verified before it is installed. The previous version is kept, and
  Bloom goes back to it by itself when a new version cannot start.
- **Help > Install Command-Line Tool** on macOS, and an **Add bloom-cli to PATH** choice in the
  Windows installer, so `bloom-cli` works from a terminal.
- A new application icon and new pictures for the macOS disk image and the Windows installer.

[Unreleased]: https://github.com/afterlab/bloom/compare/v0.1.0-alpha.4...HEAD
[0.1.0-alpha.4]: https://github.com/afterlab/bloom/releases/tag/v0.1.0-alpha.4
[0.1.0-alpha.3]: https://github.com/afterlab/bloom/releases/tag/v0.1.0-alpha.3
[0.1.0-alpha.2]: https://github.com/afterlab/bloom/releases/tag/v0.1.0-alpha.2
[0.1.0-alpha.1]: https://github.com/afterlab/bloom/releases/tag/v0.1.0-alpha.1
