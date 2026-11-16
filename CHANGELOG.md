# Changelog

All notable changes to Bloom are written here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and Bloom follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/afterlab/bloom/compare/v0.1.0-alpha.1...HEAD
[0.1.0-alpha.1]: https://github.com/afterlab/bloom/releases/tag/v0.1.0-alpha.1
