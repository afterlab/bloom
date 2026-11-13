# Bloom Documentation

Status: canonical

Updated: 2026-11-01

This directory is Bloom's current source of truth. Historical notes outside this repository may
provide research or rationale, but they are not binding until adopted here.

## Reading Order

1. [`product/foundation.md`](product/foundation.md) — product identity, current decisions, v0 proof,
   and non-goals.
2. [`product/quality-bar.md`](product/quality-bar.md) — professional artist experience, reliability,
   responsiveness, and VFX expectations; [`product/node-catalogue.md`](product/node-catalogue.md) —
   planned artist-facing node families and delivery levels.
3. [`standards/strategy.md`](standards/strategy.md) — standards-first pipeline and interoperability
   policy.
4. [`ux/compositing-workspace.md`](ux/compositing-workspace.md) — the UI sketch as an interaction
   contract; [`ux/visual-language.md`](ux/visual-language.md) — iconography and typography;
   [`ux/interaction-model.md`](ux/interaction-model.md) — the keyboard and pointer bindings,
   including blank-project startup and the New Composition duration units;
   [`ux/sound.md`](ux/sound.md) — UI sound feedback, render notifications, and the event audit.
5. [`architecture/overview.md`](architecture/overview.md) — boundaries, state ownership, and
   dependency direction.
6. [`architecture/module-system.md`](architecture/module-system.md),
   [`architecture/layer-graph-model.md`](architecture/layer-graph-model.md),
   [`architecture/value-node-library.md`](architecture/value-node-library.md),
   [`architecture/fields.md`](architecture/fields.md),
   [`architecture/path-value.md`](architecture/path-value.md),
   [`architecture/number-list.md`](architecture/number-list.md),
   [`architecture/vector-artwork.md`](architecture/vector-artwork.md),
   [`architecture/animation-and-time.md`](architecture/animation-and-time.md),
   [`architecture/layer-alignment.md`](architecture/layer-alignment.md),
   [`architecture/evaluation-primitives.md`](architecture/evaluation-primitives.md),
   [`architecture/task-system.md`](architecture/task-system.md),
   [`architecture/workspace-layout.md`](architecture/workspace-layout.md),
   [`architecture/project-format.md`](architecture/project-format.md),
   [`architecture/fonts.md`](architecture/fonts.md),
   [`architecture/project-session.md`](architecture/project-session.md),
   [`architecture/color-management.md`](architecture/color-management.md),
   [`architecture/frame-output.md`](architecture/frame-output.md),
   [`architecture/media-io.md`](architecture/media-io.md),
   [`architecture/dependency-intake.md`](architecture/dependency-intake.md),
   [`architecture/scripting-and-addons.md`](architecture/scripting-and-addons.md),
   [`architecture/gpu-backend.md`](architecture/gpu-backend.md),
   [`architecture/motion-blur.md`](architecture/motion-blur.md),
   [`architecture/image-statistics.md`](architecture/image-statistics.md),
   [`architecture/platform-support.md`](architecture/platform-support.md),
   [`architecture/ui-feedback.md`](architecture/ui-feedback.md), and
   [`architecture/profiler-feed.md`](architecture/profiler-feed.md) — execution and platform
   contracts and extension boundaries.
7. [`roadmap.md`](roadmap.md) — proof-oriented implementation sequence.
8. [`decisions/`](decisions/) — accepted architecture decision records, among them the memory
   design ([ADR 0037](decisions/0037-memory-follows-the-working-set.md): staging is scratch, media
   stays compact, memory follows the working set).

The issue #304 Linux implementation and local qualification cover processing/presentation allocation
ownership, immutable source revisions, bounded native command batches, repeated fill/purge and
sustained packed Viewer playback. Admission now uses measured capacity minus the RAM/VRAM headroom
preferences and each frame's actual need, and service-backed RAM fill keeps as many frames in flight
as fit (see [GPU admission](architecture/gpu-backend.md#processing-phase-admission-by-measured-capacity-and-actual-need)). See the
[current Linux qualification record](plans/gpu-304-linux-qualification.md) and the historical
[execution checkpoints](plans/gpu-reliability-and-preview.md#execution-checkpoint) for exact limits.

## User Guides

- [The Export panel](user-guide/exporting-video.md) — the render queue, outputs, destinations and replacing files.
- [Denoise & Smooth](user-guide/denoise.md) — Median, Bilateral, Spatial and Temporal noise reduction.
- [Vector Artwork](user-guide/vector-artwork.md) — keying a Path node from its card, Properties and the timeline.
- [Four-Color Gradient](user-guide/four-color-gradient.md) — a source blending four colors anchored at four points.
- [Gaussian Blur and Transform 2D extras](user-guide/blur-and-transform.md) — separate axes, per-channel blur, skew and opacity.
- [Organizing Assets](user-guide/assets.md) — folders, display names, tags, search and drag order.
- [Interpret Footage](user-guide/interpret-footage.md) — frame rate, start timecode, alpha, fields,
  pixel aspect, loop, missing frames and input color for imported media.
- [Import from network shares](user-guide/network-shares.md) — mounting an SMB share and
  importing from it on Linux, macOS and Windows.
- [Preferences](user-guide/preferences.md) — global preferences, which changes take effect immediately,
  which need a restart, and what is project or session state instead.
- [Memory](user-guide/memory.md) — the RAM and VRAM headroom preferences, how work and caches use
  the rest of the machine, and what actual OS limits apply.
- [The Toolbar](user-guide/toolbar.md) — the tools, the active tool's options, and aligning
  and distributing layers.
- [Workspace presets](user-guide/workspace-presets.md) — the Workspace picker, your own layouts and how the Export panel opens.
- [Arranging panels](user-guide/panels.md) — splitting, joining, closing and full-screen panels.
- [The status bar](user-guide/status-bar.md) — the labelled groups along the bottom of the window.
- [Ways to apply an effect](user-guide/ways-to-apply-an-effect.md) — every Merge entry is a layer,
  adjustment layers, Layer nodes, nested Merges and the graphs that give the same pixels.
- [The Viewer](user-guide/viewer.md) — moving layers directly, including the Shift axis lock.
- [The Image Preview node](user-guide/image-preview.md) — a node-card thumbnail of any point in the
  graph, its display choices, and showing it as the node editor background.
- [Grade, Levels, Curves and Hue/Saturation](user-guide/color-grading.md) — colour wheels and
  primaries, the Levels histogram handles, per-channel grades for colour and render passes, clamp,
  the curve graph, hue curves, HDR curves, normalizing a depth pass, and Colorize (fill, tint,
  tritone, gradient map and black and white).
- [Fields](user-guide/fields.md) — noise, cells, patterns, distances and forces as procedural
  fields, their image and mask outputs, Rasterize Field, Field Sample, and the fractal noise
  controls under their familiar names.
- [Depth effects](user-guide/depth-effects.md) — Z Defocus and Depth Fog from any depth or mist
  pass: the depth channel and encoding, reading a pass as data, fog before defocus, the lens modes
  and bokeh, picking the focus in the Viewer and rack focus.
- [Strokes and glowing paths](user-guide/strokes-and-glowing-paths.md) — Path Stroke's write-on,
  tapers, dashes and brush stamps, stroking masks and letters with Mask Path and Text Outline,
  outlining an image's alpha with Alpha Contour, and the ready-made Saber, Beam, Vegas, Write-on and
  Stroke glows.
- [Lens Flare](user-guide/lens-flare.md) — the element stack, presets, placing the light in 2D, from
  a tracker or in 3D, and following the picture with source brightness and an occlusion matte.
- [Film Grain](user-guide/film-grain.md) — seeded, repeatable grain: size, each channel's amount,
  monochrome grain, the response to brightness, a Sensor Noise mode, and holding or redrawing it over
  time.
- [Frame Rate Hold](user-guide/frame-rate-hold.md) — holding frames to a lower rate, with an
  optional seeded irregular cadence of doubled and dropped frames.
- [Auto Exposure](user-guide/auto-exposure.md) — a gain that follows the footage's brightness and color
  with an attack and a release, so a light that dims or flares makes the picture hunt like a phone camera.
- [Color tools](user-guide/color-tools.md) — Tone Map: highlight roll-off, shadow recovery and gamut
  compression for HDR scene-linear color; Color Matrix: a channel mixer and color-model conversion; HSL Qualifier: a Mask of a
  hue, saturation and luma range; Edge Extend: solid color pushed under a soft alpha edge.
- [Audio Effects](user-guide/audio-effects.md) — Equalizer, Compressor, Auto Level, Mono and Hiss for an
  audio layer, heard the same in playback and export.
- [Audio analysis](user-guide/audio-analysis.md) — numbers and pictures read from any sound so
  motion can follow it: frequency bands, the spectrum and waveform, hits, brightness, stereo, the
  beat and the pitch classes, and baking any of them to keyframes.
- [Codec Degrade](user-guide/codec-degrade.md) — block quantisation, chroma bleed and keyframe
  smearing: quality, block size, GOP length and P-frame persistence.
- [Stabilize](user-guide/stabilize.md) — follow a feature through a shot and hold the picture still, or
  carry an element along with it.
- [Inner Shadow and Inner Glow](user-guide/inner-shadow-and-glow.md) — shade or light the inside of a
  shape along its edges: colour, direction, blur, choke, size and Edge or Center.
- [Dither](user-guide/dither.md) — break up the banding of an 8- or 10-bit export with a blue-noise or
  Bayer pattern measured in steps of the target bit depth.
- [Quantize](user-guide/quantize.md) — cut the picture into a black-and-white threshold (luma or per
  channel, hard or soft) or a posterize of a few levels per channel.
- [Tile & Mirror](user-guide/tile-and-mirror.md) — repeat the picture in tiles, scroll it with wrap,
  reflect it about a line, or fold it into a kaleidoscope.
- [Procedural Warp](user-guide/procedural-warp.md) — twirl, bulge or pinch, spherize, ripple, wave,
  polar coordinates, magnify, warp styles, bend, split, turbulent or drizzle the picture, one mode at a
  time.
- [Corner Pin, ST Map and Displace](user-guide/warp-nodes.md) — pin a picture to four corners with
  perspective, or move it by a map: absolute positions (ST Map) or offsets (Displace).
- [Lens Distortion](user-guide/lens-distortion.md) — undistort or redistort a picture with the standard
  radial and tangential lens model, and an ST map of the same warp.
- [Chromatic Aberration](user-guide/chromatic-aberration.md) — pull red and blue apart radially about a
  centre, growing towards the edges, with green left in place.
- [Rolling Shutter](user-guide/rolling-shutter.md) — skew and wobble from a sensor that reads its rows in turn,
  following a Camera Shake exactly or an Optical Flow.
- [Light Flicker](user-guide/light-flicker.md) — a seeded flicker and dropout generator with a gain and a
  stops output that drives Exposure or any parameter.
- [Test Card](user-guide/test-card.md) — sixteen procedural test patterns (bars, patch, ramps, geometry, sharpness,
  motion, countdown, alpha), the size rules, the Patch's expected display value, and the sources of every
  standard value.
- [Light Rays](user-guide/light-rays.md) — volumetric rays that radiate from a point, driven by the
  picture's luminance or alpha, with length, falloff, color and boost.
- [Motion blur](user-guide/motion-blur.md) — which motion blur to use, the shared shutter, sub-frame
  Motion Blur, Transform 2D's shutter, Vector Motion Blur and its renderer conventions, Optical Flow,
  Optical Flow Motion Blur with mattes, Echo and Posterize Time, and the Viewer handles of
  Directional, Spin and Zoom Blur.
- [Camera Shake](user-guide/camera-shake.md) — seeded handheld, impact and gate-weave shake with
  loop, auto overscan, motion blur and offset and roll outputs for other parameters.
- [Text layers](user-guide/text-layers.md) — what a text layer's Position names for Left, Center and
  Right alignment, and how older projects keep their placement.
- [Sharpen](user-guide/sharpen.md) — the unsharp mask: amount, radius and threshold, and what it does
  to HDR highlights and alpha edges.
- [Separate Channels and Combine Channels](user-guide/channels.md) — splitting an image into
  channels, rebuilding it from other images' channels, Luma and the alpha mode.
- [Keyframe interpolation, easing and layer names](user-guide/keyframe-easing.md) — the standard
  easing curves, Easy Ease, shy layers and layer names that follow their source.
- [Scripting Bloom with Python](user-guide/python.md) — Python package, transactions and embedding.
- [Using Bloom from an agent](user-guide/mcp.md) — stdio MCP tools and request limits.
- [When a composition cannot render](user-guide/compile-failures.md) — the status bar's Details popup and what to do.
- [MCP Server panel](user-guide/mcp-server-panel.md) — start or stop the in-app MCP server, port, token and clients.
- [The Profiler panel](user-guide/profiler.md) — Bloom's memory, GPU, CPU, workload, pressure and
  helper processes over time.
- [Headless scripting](user-guide/scripting.md) — `bloom-cli`, JSON transactions, and headless
  frame/range rendering.

## Current Checkpoint

- The UI kit polish release is complete: one control look and height, the shared popup window, the
  Inter/Geist type system, the contextual tool bar ([ADR 0031](decisions/0031-contextual-tool-bar.md),
  [user guide](user-guide/toolbar.md)) and a UI grammar gate with an empty allowlist. What
  shipped, the validation evidence and the macOS/Windows gaps are in the
  [release record](plans/ui-kit-polish-release.md).
- The deterministic CPU preview vertical slice through Batch 3 is implemented and locally verified;
  qualified shared and desktop CI on all three product platforms will remain the cross-platform
  qualification authority. The current automated merge gate is Linux native/core only.
- Batch 4's rational-time, animation, and interaction contracts are accepted. Durable curves,
  authoring commands, exact sampling, CPU evaluator integration, exact session time, and the
  one-active/one-newest preview-request gate are implemented. The 16 ms pointer cadence, timeline
  key projection, and direct manipulation remain active work.
- Persistence, project-session, color-management, frame-output, and dependency-intake contracts now
  define Batches 5–7. Initial persistence foundations now include budget-enforced I/O allocation,
  strict text/decimal/Base64 primitives, bounded canonical JSON layout writing, canonical manifest
  encoding and schema checks, manifest dependency validation, durable allocator and extension
  state, application-wide publication ordering, a Linux staged-artifact foundation with mandatory
  close/reopen verification, and synchronous Qt-free project-session ownership. Complete JSON
  parsing/document encoding, the document schema, ZIP, format-specific semantic verification,
  non-Linux publication, and async Open/Save remain pending. Standalone durable color-setting
  values and versioned OCIO content-revision primitives are implemented without inventing a default
  digest; Project ownership waits for the qualified Bloom Neutral v1 asset and profile. ADR 0019
  accepts the dependency mechanism, while concrete prefixes and profiles remain pending
  qualification.
- A shared GPU scene prepares every current compiled pixel operation under a strict fail-closed
  operation and render-route coverage contract, with no pixel exception. The resident preview and
  final-render routes both evaluate through it, including the general GPU output-color production
  context, a 6000x4000 media+CST+text export with locally verified native execution, and
  capacity-aware resident sizing. The CPU reference evaluator remains the correctness oracle and
  explicit supported fallback; GPU execution has been exercised on Linux, macOS and Windows
  ([platform support](architecture/platform-support.md)). Vulkan/MoltenVK remains a working GPU direction
  until its cross-platform parity and lifecycle spike passes.
- Ordinary Viewer preview now uses genuine GPU scene/display preparation followed by an asynchronous
  final RGBA8 transfer and QWidget painting, shown in the status bar as "GPU render". Bounded
  private Linux checks qualify clipping, ROI, coherent outlines, host-only Full/Half 100-frame reuse,
  real animated text manipulation and short useful cold progress. Channel remapping is a bounded
  CPU worker with explicit provenance. Native preview/final GPU coverage remains required. These
  Linux qualification now includes ten Half100 fill/purge cycles, 60 seconds of retained painted
  playback and bounded command batching in the [current record](plans/gpu-304-linux-qualification.md).
  Cross-platform native execution, physical scanout and whole-process memory guarantees remain
  unqualified.
- Shared preview completion now uses callback-free neutral runtime subscriptions and one coalesced
  queued UI drain. RAM/background refill and typed Viewer/asset/bootstrap/export/purge consumers
  receive completed results independently of Jobs progress snapshots. Bounded private tests qualify
  burst coalescing, held-worker UI activity and GPU Half100 refill; native acknowledgements,
  separate text-layout/ProjectHost I/O polling and platform gates remain separate. The Linux
  sustained packed playback result is recorded above.
- Playback now derives cache-independent targets from elapsed rational wall/audio time, using checked
  valid-grid origins, live reanchoring and absolute intent-time deadlines. Five competing 60-second
  CPU-reference sessions show zero exact desired-grid mismatches; private GPU animation pixels
  separately qualify short visible progress. This is clock/image correctness, not painted FPS or
  real-audio/sustained/platform qualification.

- Reusable cache retention now uses off-UI process-aware sampling, pairing visible Linux cgroup v2
  limits/usage with zero reclaimable credit. Explicit retention preferences are authoritative;
  only observed absolute OS limits constrain their combined allowances. Cold zero retention preserves
  foreground preparation and separate live export reservations. Saved requests, configured ceilings
  and effective reusable allowances remain distinct; unsupported platforms disclose host-only fallback.
  Bounded policy/lifetime tests and a private capped helper pass. This is not an aggregate process/OOM
  guarantee. Broader reservations and remaining G4–G10 gates stay open.

- Typed task results now belong to live handles rather than retained terminal history. Weak writers,
  admission pins and pointer-only lock exchange preserve take-once delivery while releasing abandoned
  results outside locks. CPU/I/O and mock GPU forced/late/reentrant lifetime gates pass; scheduler
  history, accounting and completion ordering remain unchanged. Processing-phase admission is
  documented separately and adds no whole-process memory guarantee beyond the headroom.

- Processing-phase admission uses measured capacity minus the RAM/VRAM headroom preferences
  (default 2 GiB each) and each phase's actual need: explicit shared roots, checked host/device/count
  claims, typed waits that resume on release, refusal only when one frame cannot fit the whole usable
  capacity, strict Viewer priority for exports, retirement/quarantine lifetime and recovery of a
  generation latched by an unproven retirement; see the
  [owning GPU contract](architecture/gpu-backend.md#processing-phase-admission-by-measured-capacity-and-actual-need).

- Rooted packed-host RGBA8 payload ownership and conservation are accepted within
  [a separate bounded slice](architecture/gpu-backend.md#rooted-packed-host-payload-ownership-bounded-ownership-slice).
  Its off-UI published-retirement follow-on implements charged worker freeing with explicit
  shutdown ownership; core/GUI, headless, Linux CPU-only and all 103 affected strict checks pass.
  No whole-process accounting is implied.

The detailed implementation status and next merge gates live in [`roadmap.md`](roadmap.md).

The [performance and reliability sprint](plans/performance-reliability-sprint.md) tracks the ordered
node, recovery, RAM preview, cache, audio, media and document work under #306, with
[local milestone validation](development/local-validation.md). Performance and colour/format
accuracy are measured with the [benchmark suite](development/benchmarks.md).

The [production workflows release](plans/production-workflows-release.md) records what landed under
#374 (timeline, footage, masking, EXR, composition settings and Assets) and the work still open.

See [The Export panel](user-guide/exporting-video.md) for the render queue and
[Export presets](user-guide/exporting-video.md) for presets, audio, verification and cancellation.

## Status Vocabulary

- `accepted`: a binding current decision.
- `working`: the current direction, expected to be tested and refined.
- `proposed`: an option under discussion.
- `superseded`: retained for history but no longer current.

Documents should separate accepted decisions from working assumptions and open questions. Avoid
presenting deferred product scope as an implementation commitment.

## Documentation Rules

- Update the owning document rather than duplicating a decision.
- Record architecture changes as decision records when they affect multiple subsystems.
- Keep implementation-specific detail close to the code it governs.
- Never include machine-specific paths, credentials, tokens, or private environment details.
