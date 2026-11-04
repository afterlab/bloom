# ADR 0037: Staging Is Scratch, Media Stays Compact, Memory Follows The Working Set

Status: accepted

Date: 2026-11-01; amended 2026-11-02 (source caches hold the playback window)

Issues: #421 (supersedes #419 and #420); #413 (finished frames evicted last); #386 (headroom);
#422 (GPU-side colour conversion, separate)

## Context

Playing a 9.8 MB, 10 s 720p H.264 file took 4.93 GB of host memory; an 822×480, 1 min 47 s clip
took about 18 GB, and an 8 GB machine could not play a small file. Measured on main f54a2628b
(offscreen, one 10 s pass of 720p H.264):

| Store | Bytes |
|---|---|
| Process anonymous memory | 4.93 GB |
| Prepared GPU-upload host copies (full-resolution RGBA32F per frame, kept after upload) | 3.52 GB |
| Finished preview frames (all 240, display-packed 8-bit) | 0.89 GB |

For each decoded frame the GPU route converted to a full-resolution RGBA32F host image (16 bytes per
pixel: 14.7 MB per 720p frame against 1.4 MB decoded 8-bit 4:2:0) and, after the upload, stored that
copy in a prepared-upload cache bound into the shared operation cache with no byte or entry cap
(ecf3bab3b). It was bounded only by the operation share (60%) of an automatic total capped at 50% of
physical memory (8e2e3d952). Linear playback never reused those copies. Neither rule was requested
by the owner.

## Decision

Three rules govern every store that holds pixels:

1. **Staging is scratch, never cache.** A host buffer that exists to move pixels to the GPU is
   released once the upload completes. Nothing keeps a host copy of data the GPU already holds.
2. **Media stays in its smallest faithful form while retained.** Retain decoded source samples
   (YUV, half, 8/16-bit) where scrubbing needs them, never expanded float32 copies.
3. **Memory follows the working set.** Playback holds its read-ahead window. Intermediates are
   retained only where reuse is measured.
- **Finished frames are the exception** (the owner's layer-based compositor model): frames rendered
     during playback and RAM Preview stay cached up to the headroom and are evicted last (#413).
   - **Source caches hold the playback window**, not every frame played: the decoded video frames
     and the image-sequence source revisions keep a frame's source only while that frame is inside
     the window around the playhead (below), or while a render in flight still needs it. Finished
     frames cover replays; re-rendering an evicted finished frame decodes its source again.
   - Free memory is room for what the user keeps, not a target to fill.
   - The headroom preference is the only ceiling: usable = measured RAM − headroom (#386).

What changed:

- **Prepared-upload retention is removed** for video and image sources on every route: interactive
  preview, RAM Preview, export and headless. `GpuPreparedUploadCache`, the operation cache's
  `PreparedUpload` kind with its byte/entry subset policy, candidate promotion and accounting
  fields, the media context's `preparedUploadCache`, the upload hit/miss statistics, the
  purge-controller hook and the bench harness field are gone. The converted image lives only while
  the prepared scene's upload command holds it. What in-flight correctness needs stays: the pool's
  explicit host retirement owner (`OperationCache::bindHostRetirement`, so evicted entries free
  after the GPU work that reads them) and the irreversible producer close (`OperationCache::close`).
- **Fixed fractions are removed.** The automatic total is usable memory: measured physical capacity
  minus the RAM headroom preference (its default share until a preference is applied), with the
  historical 3 GiB fallback only while capacity is unmeasured. Gone: `kDefaultTotalPhysicalPercent`
  (50%), the 60/40 operation/preview split and its 2 GiB preview floor, the decoded-video automatic
  target of one sixteenth of physical memory (256 MiB to 4 GiB), the retained decode-session budget
  of a quarter of physical memory, the prepared-upload share (1/16 of the operation budget) and the
  GPU scene cache's retained share (1/8 of the host operation budget). Each automatic pool may use
  all usable memory; they compete inside it under the existing priority rules, finished frames
  evicted last. Observed cgroup and OS limits still apply as measured constraints. The GPU scene
  cache is device memory; without an explicit override a final-render evaluator sizes it from its
  own device's resident capacity plan, exactly as the preview route's resolver does, never from
  host memory.
- **Kept, because they size one piece of in-flight work, not retention:** the producer image
  ceiling (a quarter of the assigned operation budget for one native image), the per-request
  preview allowance (a quarter of the assigned preview budget for one request's live GPU set), the
  aggregate GPU request-owned reservation default, and the preview frames-in-flight sizing (a
  quarter of free capacity, at most one frame per hardware thread for a RAM fill and eight for
  playback read-ahead; see the frames-in-flight amendment below). The headroom preference's default share and the
  pressure hysteresis margin (an eighth of the headroom) are the headroom itself, not a fraction of
  physical memory.

### The playback window (amendment, 2026-11-02)

`SourceWorkingSet` (`src/runtime/include/bloom/runtime/source_working_set.hpp`), owned by the
evaluator's operation cache and shared with its decoded-video cache:

- Every render request (a GPU scene build or a CPU evaluation; a nested build joins its parent's)
  registers its composition frame for its lifetime and tags the sources it stores or finds with
  that frame.
- A composition's window spans its requests in flight and the most recently started one, plus the
  image-sequence read-ahead's width ahead (eight frames, the existing constant, now
  `SourceWorkingSet::kReadAheadFrames`). Behind the playhead it reaches only the frames still in
  flight. Read-ahead captures are tagged with the frame they are captured for.
- When a request ends, both caches release tracked entries outside the window (counted as
  evictions in the Profiler's pool rows). Operation-kind entries, finished frames and sources used
  outside any request (tools, tests) keep their ordinary retention. There is no new limit,
  preference or fraction; the budgets above are unchanged.
- A composition with no request in flight keeps its window until a request for another
  composition begins; then that window is dropped (its entries are already inside it), so only
  compositions in use keep one (#462).

### Frames in flight follow the hardware threads (amendment, 2026-11-04)

The preview frames-in-flight ceiling of eight frames is removed (#477). Each frame decodes and
prepares its footage serially on one CPU worker, so a fill runs at frames in flight divided by that
per-frame time: with eight frames a 2160p PNG16 fill kept 8.2 of 16 hardware threads busy while the
GPU owner thread was busy 46% of the time and idle the rest. A RAM preview fill now keeps at most
one frame per hardware thread in flight, still sized to a quarter of the free capacity at the start
of a run (at least one frame when one fits). Playback read-ahead keeps at most eight frames
(`BackgroundPreviewController::kMaximumReadAheadFrames`): its frames prepare concurrently, and with
sixteen the frames nearest the playhead landed later, so cold playback ran slower and dropped more
frames than with eight. The playback preroll window stays at most eight frames.

### Reused native storage holds no content (amendment, 2026-11-04)

The preview device parks a frame's released native images and staging buffers and gives them to the
next allocation of the same shape instead of allocating again (#477): a 2160p PNG16 frame made 17.3
device-memory allocations and as many frees, each a driver call, and makes 0.15 with reuse. This
keeps storage, not data: nothing
reads what a parked object held, so rule 1 stands (no host copy of data the GPU holds is kept), and
the upload staging still leaves the live staging line when its upload completes. Parked storage has
no fixed size or count; it is freed least recently parked first whenever work waits for device or
host memory or the device's free memory falls below the VRAM headroom, ahead of every cache.

## Audit of pixel-holding stores

Bytes per frame are measured at 720p (1280×720) on the GPU route unless noted. "Reuse measured"
names the measurement that shows a later read of the retained bytes.

| Store | Form held | Bytes per frame | Scratch or cache | Reuse measured | Origin | Outcome |
|---|---|---|---|---|---|---|
| Prepared uploads (`GpuPreparedUploadCache`) | RGBA32F, full resolution | 14.7 MB | was cache | none: 3.52 GB per 10 s pass, never read by linear playback | 919d5f55b (#299); bound uncapped in ecf3bab3b | removed: scratch, held only by the scene's upload command (and shared inside one build) |
| Decoded video (`DecodedVideoCache`) | decoded planes | H.264 1.38 MB; ProRes 422 HQ 7.4 MB | playback window | scrub revisit after a preview purge: H.264 29 ms against 69 ms uncached, ProRes 27 against 39 ms (medians) while it held every frame; none in linear playback | 17b43cf24 | kept, compact (rule 2); its 1/16-of-physical target removed. Since the 2026-11-02 amendment it holds the playback window only (H.264 0.33 → 0.001 GB, ProRes 1.77 → 0.007 GB after 10 s; flat in clip length) |
| Operation-cache intermediates (`Operation` kind) | values, metadata; RGBA32F only on the CPU route | 0.06 GB total, flat in clip length (GPU route) | cache | static layers and value results across frames; an export's linear pass keeps only keys that recur | f5be51089 | kept |
| Image-sequence source revisions (`DecodedMedia` kind) | encoded file bytes | PNG16 4.6 MB; EXR half PIZ 2.5 MB | playback window | warm edit decodes with zero I/O, hash and probe at the playhead (tests); none in linear playback | a7d693d88 (#305) | kept, compact. Since the 2026-11-02 amendment it holds the playback window and the read-ahead captures ahead of it only (PNG16 1.11 → 0.06 GB, EXR half 0.60 → 0.003 GB after 10 s; flat in clip length) |
| Image-sequence read-ahead (`ImageSequenceReadAhead`) | encoded file bytes | next 8 members | scratch window | consumed by the next frames | f23c0cdc3 | kept: holds its window; captures land in the source-revision row above |
| Preview frames (`PreviewFrameCache`) | display-packed RGBA8 | 3.7 MB | cache: finished frames | cached playback | 80024f837 (#139) | kept: the exception, evicted last |
| GPU scene cache (`GpuSceneCache`) | device images | device memory only | cache | an unchanged warm rebuild dispatches nothing (native tests) | 919d5f55b (#299); 1/8 share 04ca9ef2d | kept; the host-derived 1/8 share removed, device plan sizes it |
| Proxies (asset thumbnails) | packed thumbnails | thumbnail-sized | cache | thumbnail redisplay; not exercised by the playback bench | 8e2e3d952 | kept; absolute 8 MiB/512-entry cap, no fraction |
| Decoded audio (`AssetController`) | PCM | not a frame store | cache | audio playback; not exercised by the video bench | 8e2e3d952 | kept; absolute cap, no fraction |
| Media disk cache write queue | decoded image awaiting its write | one frame per queued write | scratch queue | not a reuse store | c6c3ff961 (#234) | kept; 64 entries and 256 MiB |

## Measurements

RTX 4090, Ryzen 7 9700X, 64 GB; offscreen; `tools/bench/run_bench.py` harness, fresh cold process
per row, inside a 20 GiB cgroup (the 8 GB rows inside an 8 GiB cgroup, whose 0.8 GiB default headroom
matches); 720p bench footage, the 1080p L10 stack; main 22d9c3f21 before, this change after. Peak
anonymous RSS during the measured window; exports report the process tree's sampled peak RSS. Every
row reports 0 CPU-fallback frames.

| Workflow | Before | After |
|---|---|---|
| 720p H.264 playback 10 s | 4.93 GB | 1.44 GB |
| 720p H.264 playback 2.5 s / 40 s | 1.35 / 17.58 GB | 0.51 / 5.09 GB |
| 720p ProRes 422 HQ playback 10 s | 6.35 GB | 2.82 GB |
| 720p ProRes 422 HQ playback 2.5 s / 40 s | 1.70 / 18.92 GB | 0.82 / 10.80 GB |
| 720p PNG16 playback 10 s (2.5 s) | 5.68 (1.57) GB | 2.16 (0.73) GB |
| 720p EXR half playback 10 s (2.5 s) | 5.19 (1.44) GB | 1.66 (0.59) GB |
| 1080p L10 playback 10 s | 8.50 GB at 0.7 fps | 9.06 GB at 2.2 fps |
| RAM Preview, 240 frames (H.264 / ProRes / PNG16 / EXR / L10) | 1.01 / 1.04 / 1.10 / 1.08 / 3.49 GB | 1.09 / 1.07 / 1.11 / 1.07 / 3.90 GB |
| Export H.264 job, H.264 10 s / 40 s | 0.56 / 0.58 GB | 0.56 / 0.59 GB |
| Export H.264 job, ProRes 10 s / 40 s | 0.49 / 0.56 GB | 0.48 / 0.53 GB |
| Export ProRes job, PNG16 2.5 s / 10 s | 0.53 / 0.55 GB | 0.52 / 0.54 GB |
| Export ProRes job, EXR half 2.5 s / 10 s | 0.54 / 0.56 GB | 0.53 / 0.56 GB |
| Export EXR job, L10 10 s | 2.83 GB | 2.99 GB |
| 8 GB profile: H.264 playback 10 s / 40 s | 3.81 / 7.28 GB | 1.40 / 5.08 GB |
| 8 GB profile: ProRes / PNG16 / EXR playback 10 s | 5.92 / 3.44 / 3.42 GB | 2.82 / 2.19 / 1.74 GB |

- Playback held real time on every 720p row (23 to 23.6 fps over 10 s, no dropped frames), before
  and after, including the 8 GB profile.
- RAM Preview kept all 240 frames in every row. Its memory is the finished frames: 0.85 GB for
  240 display-packed 720p frames (3.7 MB each) plus about 0.2 GB of process base.
- Export memory does not grow with length.
- Scrubbing to an uncached frame stays responsive through the compact decoded-frame path. Median
  and worst uncached scrub, 24 targets: H.264 107 ms and 334 ms (before 102 and 334); ProRes 43 ms
  and 167 ms (before 40 and 170). A revisit after a preview purge is served by the decoded frame:
  40 ms H.264, 30 ms ProRes (before, from the removed float copy: 29 and 23 ms).
- Playback memory is flat in clip length except for the finished frames (the exception) and two
  compact caches that keep every frame played, the decoded video frames and the sequence source
  revisions. Excluding finished frames, 720p H.264 holds 0.32 / 0.57 / 1.57 GB for 2.5 / 10 / 40 s
  (decoded frames 1.38 MB each), ProRes 0.60 / 1.93 / 7.26 GB (7.4 MB each), PNG16 0.52 / 1.27 GB
  (4.6 MB source revision each). The L10 stack's higher peak is the same retention at three times
  the frame rate.

### Amendment measurements (2026-11-02)

Same machine and harness, inside a 20 GiB cgroup; before is main with the Profiler counters
(3bfc954f8), after adds the playback window. Peak anonymous RSS and source-cache bytes (decoded
video for H.264 and ProRes, decoded-media entries for PNG16 and EXR) are the harness sample with the
highest RSS; finished frames are the frames the Viewer painted (every one stays cached). The media
is 10 s, so sequences compare 2.5 s and 10 s, and the 40 s video rows use a 40 s file. Every row
has 0 CPU-fallback frames and 0 dropped frames.

| Workflow | Peak anon | Source cache | Painted (all cached) | fps |
|---|---|---|---|---|
| H.264 10 s | 1.40 → 1.05 GB | 0.33 → 0.001 GB | 234 / 235 | 23.4 / 23.5 |
| H.264 40 s | 5.06 → 3.73 GB | 1.33 → 0.004 GB | 955 / 954 | 23.9 / 23.8 |
| ProRes 422 HQ 10 s | 2.82 → 1.08 GB | 1.77 → 0.007 GB | 234 / 234 | 23.4 / 23.4 |
| ProRes 422 HQ 40 s | 10.80 → 3.71 GB | 7.08 → 0.007 GB | 954 / 954 | 23.8 / 23.8 |
| PNG16 2.5 s | 0.70 → 0.48 GB | 0.28 → 0.03 GB | 53 / 53 | 21.2 / 21.2 |
| PNG16 10 s | 2.17 → 1.11 GB | 1.11 → 0.05 GB | 233 / 233 | 23.3 / 23.3 |
| EXR half 2.5 s | 0.57 → 0.47 GB | 0.15 → 0.03 GB | 56 / 56 | 22.4 / 22.4 |
| EXR half 10 s | 1.74 → 1.09 GB | 0.60 → 0.003 GB | 235 / 236 | 23.5 / 23.6 |

- Source-cache bytes are flat in clip length; what remains of playback memory is the finished
  frames (3.5 GB for about 955 frames at 40 s) and the process base.
- Scrub medians, 12 targets, preview frame cache cleared before each revisit:

| Scrub | H.264 | ProRes | PNG16 | EXR half |
|---|---|---|---|---|
| Far, uncached | 69 → 72 ms | 39 → 39 ms | 90 → 87 ms | 86 → 55 ms |
| Far revisit (source decoded earlier) | 29 → 65 ms | 27 → 36 ms | 59 → 62 ms | 47 → 40 ms |
| Near, one frame ahead | 70 → 40 ms | 39 → 38 ms | 63 → 63 ms | 56 → 41 ms |
| Near, one frame back | 30 → 148 ms | 27 → 36 ms | 61 → 64 ms | 53 → 40 ms |

  A far revisit and a step back now decode the source again when the finished frame is gone, as
  decided; in normal use the finished frame is still cached and serves both. Stepping back one
  H.264 frame without its finished frame re-decodes from the previous keyframe.

## Not done in this decision

- GPU-side colour conversion (uploading native YUV or 16-bit samples instead of a converted
  float image) is #422. Until then the per-frame RGBA32F conversion still runs; it is scratch.
- Device-memory partition fractions of the resident route (`gpu_preview_resident_capacity.hpp`)
  are device policy, not host retention, and are unchanged.
- Device-side caches (the GPU scene cache) are sized by the device plan and are not part of the
  playback window.

## Consequences

- Playback memory no longer carries a float32 copy of every frame played. A warm rebuild of the same
  frame converts again from the compact decoded frame or the retained encoded source.
- Automatic pools are no longer sized by fixed fractions; usable memory is the only ceiling, and
  explicit per-pool preferences are still exact.
- Statistics and harness fields for prepared uploads are gone; tests that asserted prepared-upload
  hits now assert that a warm build converts again.
