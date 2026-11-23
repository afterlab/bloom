# Foundation Roadmap

Status: working

Updated: 2026-11-24

This roadmap records what has shipped and the plan to 1.0.0. It follows the
[Bloom GitHub Project](https://github.com/orgs/afterlab/projects/2) and its milestones, which own
the priority order and show what is being built now; they win when the two differ. The focused
architecture and product documents own detailed semantics and must not be duplicated here.

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
| v0.1.0-alpha.4 (in preparation): exports with many audio and movie layers, MP3 files, the true-peak limit in delivered AAC and Opus, cached-frame reuse, MCP export from a project file, sequence import names and path bounds, clearer export failures, and the Windows update window | #765 #766 #767 #768 #769 #770 #771 #772 #773 #774 #775 #783 #784 #785 #787 #788 #789 #790 |
| v0.1.0-alpha.3 (2026-11-23): subtitles, styled and animated text, filtered and pixel-art scaling, coordinate spaces, Path Stroke dash patterns, Image Switch, export backgrounds, and presets that state their codecs, with the reliability fixes before them | #752 #757 #758 #742 #743 #591 #759 #531 #760 #761 |
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

## Plan to 1.0

Bloom ships a new alpha every week through alpha.10, then a beta stretch of hardening and 1.0.0.
Features go in early and hardening gets the long stretch, so every alpha starts with the fixes from
testing the previous release. A group that misses its week ships whole the next week. Feature
freeze is beta.1: a feature not placed by then is 1.1 or later. The due dates are those of the
[GitHub milestones](https://github.com/afterlab/bloom/milestones).

| Milestone | Due | Themes |
| --- | --- | --- |
| v0.1.0-alpha.4 | 2026-11-24 | Fixes from production testing: export with many audio and movie layers (#765 #772), MP3 files (#770), the true-peak limit in delivered AAC and Opus (#771), export job ids (#767), cached-frame reuse (#785); MCP export from a project file and range defaults (#766 #768 #769 #775 #783 #784); sequence import names and path bounds (#773 #774); every release published as the newest release |
| v0.1.0-alpha.5 | 2026-11-30 | The fixes from testing alpha.4 first. Word-timed subtitles (#776), anchor tracks and a null layer (#777), diagram graphics (#778), audio ducking, fades and audio-only export (#780), export scale, a parallel export queue and relinking (#781), OCIO names in help and queries (#782), node card native windows (#786), non-ASCII and UNC paths on Windows falsely reported as saved (#612), a movie-export test killed by SIGKILL (#762), the LaTeX Math Source (#593) |
| v0.1.0-alpha.6 | 2026-12-07 | Color and looks: Vibrance, Clarity, Texture, Dehaze, Selective Color, Color Balance by tonal range, Photo Filter, Black & White per hue, Hue/Saturation by range; the full set of familiar layer blend modes on layers and Merge; a perceptual-space option; the system color picker, eyedropper and gradient editor with a unified Gradient node (#551). Procedural and vector work: keyed term groups (#592), axes with tick labels (#596), measured bounds (#601) |
| v0.1.0-alpha.7 | 2026-12-14 | EXR and color per show: multipart and multilayer EXR, reading only the channels in use, data and display windows, missing and partial frames, EXR write options; a locked OCIO or ACES configuration per show and input transforms by rule (#725); MCP capture display and view (#649); proxies and proxy-aware playback (#372); a 3D camera and 3D layer space, including camera import from 3D software (#605). Text On Path (#345), Text To Shape and Mask (#529), vector sharpness through nesting (#607) |
| v0.1.0-alpha.8 | 2026-12-21 | LUTs, animation-composition nodes and time: LUT Extract, Look Match, Bake LUT and .cube export; beauty rebuild and light groups, normal relight, position mask, holdout and depth composite; Time Offset, Frame Hold and Time Remap (#346). Per-item list math (#628), list conversions (#629), Instancer (#526), transitions (#520) |
| v0.1.0-alpha.9 | 2026-12-28 | Reuse at scale: treatments (linked, versioned templates with exposed parameters and per-shot overrides), graph organization (backdrops, search, auto-layout), composition templates (#779); MCP addressing by name and role (#574), duplicate-composition id maps (#573), MCP surface gaps (#640), MCP frame statistics (#580) |
| v0.1.0-alpha.10 | 2027-01-04 | One model, export at volume: nodes, Properties and the timeline as views of one model with overrides shown everywhere; a resumable export queue that skips valid frames and checksums each frame; hardware video encoders (#476); multi-monitor and tear-off panels (#536) |
| v1.0.0-beta.1 | 2027-01-11 | Feature freeze. The project format is frozen with migrations, and the MCP, Python and command-line interfaces are versioned. MCP error messages (#578) and a repeat of the blind MCP field test (#491) |
| v1.0.0-beta (hardening) | 2027-03-22 | Weekly betas of hardening, with rough edges from testing first. GPU scheduling: cold shader compiles stalling GPU work (#753), pipelined uploads (#405), GPU sample conversion (#422), the export CPU fallback (#648). Playback and I/O: sequence read-ahead (#535), motion-blur and Z Defocus audits (#555 #556), Profiler fields (#427). Platforms: Windows GPU preview (#614), the macOS CPU fallback (#755), Viewer memory across monitors (#463), a Viewer stuck on Rendering (#754), HEVC 10-bit export on the bench host (#475). Validation: sustained GPU tests, LeakSanitizer and native macOS and Windows GPU runs (#304), a golden render set for pixel-identical patches, performance targets on 4K multi-pass EXR shots, and documentation |
| v1.0.0 | 2027-04-12 | Release candidates, then 1.0.0 |

The planned nodes and their delivery levels are in the [node catalogue](product/node-catalogue.md);
[`plans/node-delivery-plan.md`](plans/node-delivery-plan.md) holds the delivery rules. Instancing is
designed in [`architecture/instancing.md`](architecture/instancing.md) under
[ADR 0023](decisions/0023-instances-and-per-element-data.md).

## After 1.0

VFX features follow 1.0.0 (milestone post-1.0): keying (#485 Channel Key, Chroma Key and Despill;
#515 Matte Refine), tracking (#513 the planar tracker and tracks applied to Corner Pin) and camera
solve (#764), Flow Retime (#522), and keying research.

## Not scheduled

Particle System (#521), seekable Simulation (#594), 2D rig and puppet deformation (#606), the
Windows one-row title bar (#552) and the Assistant panel (#548) have no milestone yet. Nothing here
is promised for 1.0.0.

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

