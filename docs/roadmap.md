# Foundation Roadmap

Status: working

Updated: 2026-11-22

This roadmap records what has shipped, what is queued and in what order, and what is parked. It
follows the [Bloom GitHub Project](https://github.com/orgs/afterlab/projects/2), which owns the
priority order and shows what is being built now; the Project wins when the two differ. The focused architecture and product
documents own detailed semantics and must not be duplicated here.

Bloom is in alpha. Download the latest alpha from the
[GitHub releases page](https://github.com/afterlab/bloom/releases) and install it as the
[installing guide](user-guide/installing.md) describes, or build it from source.

## Shipped

Everything in this section is on `main` and covered by tests. Each row names the owning document.

| Area | What shipped | Owning document |
| --- | --- | --- |
| Foundation | C++20 and Qt 6 shell with a replaceable split-tree workspace; Qt-free document, typed transactional commands with revision-safe undo and redo; bounded task runtime with cancellation and progress; staged shutdown | [`architecture/overview.md`](architecture/overview.md), [`architecture/task-system.md`](architecture/task-system.md) |
| Authoring model | One composition that is both a Layer Stack and a node graph; stable IDs; rational time; typed animation curves with exact sampling; direct manipulation as one undo step | [`architecture/layer-graph-model.md`](architecture/layer-graph-model.md), [`architecture/animation-and-time.md`](architecture/animation-and-time.md) |
| Projects | Deterministic `.bloom` container: save, open, Save As, Save Copy, one current document schema and preserved unknown data; Bloom Neutral color settings for new projects | [`architecture/project-format.md`](architecture/project-format.md), [`architecture/project-session.md`](architecture/project-session.md) |
| Color | Project-qualified OCIO color management with the built-in Bloom Neutral configuration, display processing on CPU and GPU, LUT assets | [`architecture/color-management.md`](architecture/color-management.md) |
| Rendering | CPU reference evaluator; one GPU scene covering every compiled pixel operation under the coverage contract, for the Viewer and for final render; capacity-aware admission; RAM Preview | [`architecture/gpu-backend.md`](architecture/gpu-backend.md), [ADR 0037](decisions/0037-memory-follows-the-working-set.md) |
| Media and export | Image, sequence, video and audio import through isolated media workers; still, sequence and video export through the Export queue with presets, verification and audio mixing | [`architecture/media-io.md`](architecture/media-io.md), [`user-guide/exporting-video.md`](user-guide/exporting-video.md) |
| Nodes | The shipped node families listed in the [node catalogue](product/node-catalogue.md): sources, color, blur, stylize, warp, channels, masks and mattes, paths and shapes, fields, number lists, audio | [`product/node-catalogue.md`](product/node-catalogue.md) |
| Scripting and agents | Python package and embedding, `bloom-cli` headless render, the MCP server (stdio and the in-app server) | [`architecture/scripting-and-addons.md`](architecture/scripting-and-addons.md), [ADR 0034](decisions/0034-in-app-mcp-server.md) |
| Platforms | Linux, macOS and Windows builds with packaging, and the parity work that followed | [`architecture/platform-support.md`](architecture/platform-support.md) |
| Panels | Viewer, Timeline, Nodes, Properties, Assets, Jobs, Export, Scopes, Profiler, MCP Server, Script; the contextual tool bar; workspace presets | [`ux/compositing-workspace.md`](ux/compositing-workspace.md) |

### Recently landed

| Group | Issues |
| --- | --- |
| v0.1.0-alpha.3 (in preparation): subtitles, styled and animated text, filtered and pixel-art scaling, coordinate spaces, Path Stroke dash patterns, Image Switch, export backgrounds, and presets that state their codecs, with the reliability fixes before them | #752 #757 #758 #742 #743 #591 #759 #531 #760 #761 |
| v0.1.0-alpha.2 (2026-11-20): large projects that always save, agent work over MCP that stays quiet and responsive at scale, the MCP field-test bugs, and crash reports kept on the user's machine | the rows below, down to the save fix |
| Crash reports: Crashpad's out-of-process handler in every package, a text report and a minidump kept on the user's machine, a notice on the next start, debug symbols kept as release-workflow artifacts | #736 |
| Field-test bugs: a project Bloom lets you build always saves (edits that would make it too large are refused at the edit), Save a Copy names its cause, Corner Pin edge-on streaks, Crop against its picture, capture publication after the cache folder is cleared | #748 #738 #739 #740 |
| MCP at scale: asynchronous `transact` with idempotency keys and retry hints, subscriptions to coalesced change events, keys for many parameters in one operation | #747 |
| MCP at scale: request cost, every request limit advertised and named in its refusal | #745 #741 |
| MCP at scale: commits prepared off the UI thread, keyframe insertion without quadratic scans, undo charged per shared record, panels rebuilt once per frame | #744 |
| Quiet agent work and agent sessions | #746 |
| Save and Save As of a large project, with the stage and cause of a failed save | #737 |
| v0.1.0-alpha.1, the first alpha (2026-11-17): installers on every platform, signed updates checked inside Bloom with the previous version kept, the release workflow, and the new application icon and installer pictures | #734 #670 |
| Scopes counted from the frame on screen at display time; cached display frames at 10 bits per channel | #733 |
| Disk cache headroom: a live headroom instead of a fixed budget, loop-resistant eviction, no dropped writes during a RAM Preview fill, video and audio through the cache | #732 |
| Node catalogue refresh: every shipped node by family, planned nodes by issue, the historical research rows mapped | documentation only |
| Scopes rebuild: panes only with header menus, traces matched to the reference grading application | #730 |
| Scopes panel: Waveform, Parade, Vectorscope, Histogram and CIE chromaticity on CPU and GPU, headless through MCP and Python | #724 |
| Disk cache, audio and fonts: local copies of slow sources on every route, audio effect nodes and LUFS export, audio data as values, number lists, fonts by ID | #718 #728 #569 #570 #471 #329 #627 #626 #630 #631 #632 #633 #634 #635 #636 #489 |
| Test suite trim: small tests that prove behavior; stress, bench and compositor-only tests opt-in | #722 #719 |
| Toolbar, workspace, panels, controls and LUT | #717 #720 #712 #713 #714 #715 #702 #716 #687 #688 #689 #692 #698 #699 #700 #638 #609 #610 #721 #723 |
| Node editor | #711 #694 #695 #701 #703 |
| Bug batch | #682 #676 #683 #680 #685 #686 #690 #691 #693 #696 #697 #704 #705 #706 #707 #708 #709 #710 |
| Platform packaging and follow-ups | #370 #677 #679 #678 #618 #675 #664 #665 #666 #667 #669 #672 #673 #674 #681 #684 |
| Windows parity | #454 #613 #615 #616 #617 #619 |
| Engine bugs and export trust | #643 #586 #650 #608 #553 #641 #623 #379 #465 |
| macOS parity with Linux | #651 #652 #653 #654 #655 #656 #657 #658 #659 #660 #661 #662 #663 #371 #455 #438 #668 |
| Shape animation | #637 #599 #597 #598 |
| Stylize | #528 #519 #527 #510 #511 #647 |
| SVG as one layer or editable shapes | #642 #644 #645 #646 |

The earlier performance and reliability sprint (#306) and the production workflows release (#374)
are recorded in [`plans/performance-reliability-sprint.md`](plans/performance-reliability-sprint.md)
and [`plans/production-workflows-release.md`](plans/production-workflows-release.md).

## Queued

In the tracker's order. Everything starts when the owner gives the go-ahead.

### 1. Node groups

Each group is one coherent area with one validation and one landing.

| Group | Issues | What it is |
| --- | --- | --- |
| 1. Compositing: keying, time, motion, rig and camera | #485 #515 #520 #346 #531 #522 #513 #591 #606 #605 | Channel Key, Chroma Key, Despill; Matte Refine; Transition / Wipe Matte; Time Offset, Frame Hold, Time Remap; Image Switch; Flow Retime; the planar tracker and tracks applied to Corner Pin (the point tracker and Stabilize have shipped); named coordinate spaces; 2D rig and puppet deformation; 3D camera and 3D layer space |
| 2. Procedural and vector: lists, instancing, particles, simulation, text | #628 #629 #526 #521 #594 #345 #592 #529 #593 #607 #601 #596 | Per-item list math; List to Path, Image and Points; Instancer and Clone To Points; Particle System; seekable Simulation; shaped text layout, animator and text on path; text as paths; Text To Shape / Mask; LaTeX math as paths; vector sharpness through nesting; measured bounds; axes with tick labels |
| 3. Platform and agents: MCP, proxies, color picker, title bar | #491 #573 #574 #578 #580 #640 #649 #372 #551 #552 | Blind MCP field test; id maps; names and roles; error messages; frame statistics; audit gaps; capture display and view; proxy generation and proxy-aware playback; color picker system and unified Gradient node; Windows one-row title bar |

The planned nodes and their delivery levels are in the [node catalogue](product/node-catalogue.md);
[`plans/node-delivery-plan.md`](plans/node-delivery-plan.md) holds the delivery rules. Instancing is
designed in [`architecture/instancing.md`](architecture/instancing.md) under
[ADR 0023](decisions/0023-instances-and-per-element-data.md).

### 2. Tests

| Issue | What it is |
| --- | --- |
| #726 | Eight compositor-session GPU UI tests fail when they actually run |
| #727 | macOS: every test pays about 2-3 s at launch in validation runs |
| #731 | Windows: the gpu-shader-compiler hung-child cancellation case fails under parallel load |
| #729 | GPU admission policy overwrites the Viewer's recorded device need with another class's grant |
| #304 | GPU validation: hosted CI, sustained memory tests, LeakSanitizer and native macOS and Windows runs |

### 3. Performance

Waits for benchmarking.

| Issue | What it is |
| --- | --- |
| #405 | Overlap GPU uploads with rendering in the scene executor |
| #422 | Convert decoded video and image samples on the GPU |
| #535 | Sequence read-ahead cost on local storage |
| #476 | Export with hardware video encoders |
| #463 | Viewer memory grows when the window moves between monitors |
| #427 | Profiler: GPU utilization, pressure causes and platform reads |
| #555 | Audit motion blur performance (report only) |

### Owner checks

Not in a group: #612 file dialogs and drag and drop with non-ASCII names on Windows; #614 Viewer
resize, display scaling and monitor moves on Windows; #670 needs the final macOS icon artwork.

## Parked

Not being worked. The last column says what would unpark each item.

| Issue | What it is | What unparks it |
| --- | --- | --- |
| #648 | Export frames fall back to the CPU on a descriptor mismatch | Doesn't reproduce; open until it does |
| #475 | HEVC 10-bit export and three tests fail on the bench host | Needs the bench host |
| #553 | 2160p Cryptomatte budget and consecutive-runs items only | Needs benchmarking |
| #536 | Multi-monitor: new windows, tear-off panels, per-window fullscreen and display transform | Needs a Mac with a second display |
| #556 | Audit Z Defocus performance (report only) | Stopped by the owner; restarts when the owner says |
| #548 | Assistant panel: chat with a local agent harness connected to Bloom's MCP | Parked by the owner |
| #725 | Expand OCIO config support: more official versioned configs and custom `.ocio` and `.ocioz` loading | Parked by the owner |
| #331 | Umbrella: deliver planned nodes in bounded batches | Umbrella only; tracks the node groups above |

## Not on the roadmap

Full nonlinear editing, OpenFX hosting, render farms and distributed rendering, and a stable public
native plug-in ABI are not planned. They are non-goals in
[`product/foundation.md`](product/foundation.md), not permission for current boundaries to make
them impossible.

## Quality Gate For Every Merge

- warnings-as-errors and `clang-format` pass
- repository hygiene and architecture-boundary checks pass
- focused unit tests and the full CTest suite pass
- no Qt types enter core, document, commands, host, render, runtime, project, color, output, or
  platform contracts
- no UI callback waits on futures, workers, I/O, GPU work, or task shutdown
- no generic service locator, `std::any` result bus, duplicated project truth, or speculative public
  ABI enters the slice
- active production files remain cohesive and normally below the repository's 700-line review
  threshold
- ASan/UBSan cover image/project parsing paths; TSan covers scheduler and publication services where
  available
- Linux, macOS, and Windows shared semantics remain green before a feature is called complete

## Selection And Direct Manipulation

Timeline, Viewer, Nodes and Properties keep one primary and contextual selection truth. One drag
creates one understandable undo step, and scrub storms stay bounded and never publish an old frame.

