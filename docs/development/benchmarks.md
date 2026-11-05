# Performance benchmarks

Status: working

Bloom's standard performance benchmark measures RAM-preview fill, playback and export on a fixed,
regenerable set of footage, and checks colour and format accuracy with a procedural chart. The
tools live in [`tools/bench`](../../tools/bench). The media is never committed: the repository
holds the generators, the manifest that pins and checksums the media, the project builder and the
runner.

| Tool | Runs in | Purpose |
|---|---|---|
| `blender_scene.py` | Blender 4.5 LTS (`blender -b`) | The benchmark scene, rendered with Cycles |
| `chart.py` | Python 3 + numpy | The colour and format accuracy chart and `chart_truth.json` |
| `bench_media.py` | Python 3 | Render, encode, catalogue (`media_manifest.json`) and verify the media |
| `build_projects.py` | Bloom (`bloom-cli run`) | The benchmark projects, built through the operation registry |
| `run_bench.py` | Python 3 | The runner: measurements, results, baseline comparison, chart checks |
| `chart_check.py` | Python 3 + numpy | Chart import, sampling and export round trips (used by the runner) |

## Media

The media root is `--root`/`--media-root`, else `$BLOOM_BENCH_MEDIA`, else `./bench-media`
(ignored by Git). Everything below is relative to it.

### Footage

One deterministic Cycles scene (fixed seed 1729, OptiX GPU rendering, OpenImageDenoise, 128
samples with adaptive threshold 0.02, Standard view transform, transparent film). The camera dollies
across a ground of fine bricks and checks, a frequency-sweep wall, pillars and 360 small saturated
shapes (some emissive, some spinning), past alpha cut-out cards (some moving) and a 45% transparent
veil. A screen-space film grain of about ±7% keeps files from compressing trivially.

| Tier | Size (16:9, square pixels) | Frames at 24 fps | Image sequences | Video |
|---|---|---|---|---|
| `720p` | 1280×720 | 240 | all five | all seven |
| `1080p` | 1920×1080 | 240 | all five | all seven |
| `2160p` | 3840×2160 | 240 | all five | all seven |
| `4320p` (8K, reduced) | 7680×4320 | 48 | PNG16, EXR half PIZ, multilayer EXR | ProRes 4444, HEVC 10-bit |

The 8K tier exists to stress the streaming/memory invariant (output far larger than memory) and the
per-image GPU ceiling; it is generated last and run as its own suite.

One render per frame writes every image format:

- `png8`, `png16`: PNG RGBA 8-bit and 16-bit, sRGB transfer, straight alpha;
- `exr_half_piz`, `exr_float_zip`: OpenEXR RGBA half PIZ and float ZIP, linear Rec.709, premultiplied;
- `exr_multilayer`: the Cycles multilayer EXR (half ZIP; depth, normal, Cryptomatte and the value AOV
  in float): `Composite.Combined`, `ViewLayer.Combined`, `Depth`, `Normal`, `CryptoObject00`/`01`
  with its manifest, AOVs `bench_color` and `bench_id`, and `Noisy Image`. It has no root color
  channels, so its projects select the `Composite.Combined` color layer on every Image source
  (`bench_spec.COLOR_LAYER`, ADR 0026); without it the footage has no Image output and the cell
  measures an empty frame. Projects built before this selection existed need `--rebuild-projects`.

Video is encoded locally with FFmpeg from the 16-bit PNG master. Codecs without alpha are flattened
over black. All are tagged BT.709 primaries, transfer and matrix, limited range, left chroma siting
(the usual delivery tagging; colour accuracy is the chart's job):

| Format | File | Encoder |
|---|---|---|
| `h264` | `.mp4` | libx264 High, 8-bit 4:2:0, CRF 18 |
| `hevc10` | `.mp4` | libx265 Main 10, 4:2:0, CRF 20 |
| `prores422hq` | `.mov` | prores_ks HQ, 10-bit 4:2:2 |
| `prores4444` | `.mov` | prores_ks 4444 with 16-bit alpha |
| `dnxhr_hqx` | `.mov` | DNxHR HQX, 10-bit 4:2:2 |
| `av1` | `.mp4` | SVT-AV1 preset 8, CRF 30, 10-bit 4:2:0 |
| `vp9` | `.webm` | libvpx-vp9 CRF 31, 8-bit 4:2:0 |

Sizes of the stored set: 720p 8.2 GB, 1080p 17.7 GB, 2160p see `media_manifest.json`
(`totals.bytes` covers everything recorded). The budget is about 200 GB for the footage plus
60-100 GB for the 8K tier.

### Colour and format chart

`chart.py` draws an original procedural chart; every pixel comes from a formula, so every expected
value is exact. Tiers 1080p, 2160p and 4320p, 24 frames each, with a square moving one step per frame
so codecs see motion. Regions, each labelled on the chart:

- 75% colour bars with 40% grey pillars, then 100% primaries, secondaries, white and black;
- grey steps 0-100% and range codes 0, 1, 2, 4, 8, 16 / 235, 239, 247, 251, 254, 255;
- ramps: signal-linear, light-linear, red, green, blue;
- a 24-patch reference set from the published sRGB 8-bit values of the classic 24-patch chart;
- pixel structures in device pixels: 1 px lines, 1 and 2 px checkerboards, a zone plate reaching
  Nyquist, and 1×1/2×2 saturated squares on even and odd columns and rows (chroma siting);
- EXR-only values above 1.0 and below 0;
- alpha ramps and steps (straight in PNG, premultiplied in EXR, an alpha plane in ProRes 4444).

Outputs: PNG16, float EXR (ZIP), and video with explicit tags: H.264 BT.709 limited, BT.709 full
and BT.601 limited; HEVC 10-bit BT.709 limited, BT.2020 PQ and BT.2020 HLG; ProRes 422 HQ;
ProRes 4444; DNxHR HQX (no H.264 at 8K). SDR video carries the same sRGB-transfer signal as the PNG
and is tagged `iec61966-2-1`; HDR video maps linear 1.0 to 203 cd/m² (BT.2408). Chroma is
subsampled by `chart.py` itself (co-sited horizontally, 2-tap vertical average for 4:2:0), so the
Y'CbCr codes are exact before compression.

[`tools/bench/chart_truth.json`](../../tools/bench/chart_truth.json) records, per tier and patch,
the rectangle and an inset sample rectangle, the sRGB signal, linear Rec.709, PNG16 codes, the
premultiplied EXR value and the `[Y, Cb, Cr]` codes of every video variant, plus the formulas of the
ramps and pixel patterns and suggested tolerances. Regenerate it with `chart.py --write-truth`.

### Generating and verifying

```sh
export BLOOM_BENCH_MEDIA=/path/to/bench-media
python3 tools/bench/bench_media.py render --tier 720p --blender /path/to/blender   # Cycles
python3 tools/bench/bench_media.py encode --tier 720p                              # FFmpeg
python3 tools/bench/bench_media.py chart                                           # all chart tiers
python3 tools/bench/bench_media.py manifest --tier 720p \
    --provenance "Blender 4.5.x LTS, Cycles OptiX, <GPU>"
python3 tools/bench/bench_media.py verify
```

`render` needs Blender 4.5 LTS and a Cycles GPU device (`--device OPTIX` by default; `CUDA`,
`HIP`, `METAL`, `ONEAPI` or `CPU` also work but change the result bits). A 240-frame 720p tier takes
about six minutes on an RTX 5080. `encode` needs FFmpeg with libx264, libx265, prores_ks, dnxhd,
libsvtav1 and libvpx-vp9.

The manifest pins the Blender series and the render settings, records the render machine without
host or user names, the FFmpeg version and every encode's arguments, and for each asset its frame
count and numbering, dimensions, format details (PNG depth and channels, EXR compression and channel
list, video codec, profile, pixel format, tags and frame count), size and SHA-256. An asset's
SHA-256 is that of its `sha256sum` listing (one line per file, sorted); the per-file listings are
kept in `<root>/.checksums/`. `remoteObjectStorageUrl` is reserved for a future object-storage mirror
and is unused.

`verify` reports, per asset:

- `verified-exact`: the SHA-256 matches; this is the stored media;
- `regenerated-compatible`: the checksum differs but the frame count, numbering, dimensions and
  format all match. Cycles output differs in its last bits across GPUs, drivers and Blender builds,
  so media regenerated on another machine is acceptable for benchmarking; results record it;
- `incompatible` or `missing`: not usable; the runner skips it.

## Benchmark projects

`build_projects.py` builds two projects per media item through the same operations as the UI,
Python and MCP (it runs inside `bloom-cli`):

- **A, footage only (decode-bound):** a composition made from the footage, one layer.
- **B, a typical shot (compute-bound):** a background solid; the footage enlarged with a Gaussian
  blur; two transformed copies of the footage (scaled, rotated, positioned, one at 92% opacity), the
  smaller one through an alpha track matte from a rotated solid; and an animated text layer.

- **L<n>, many layers (per-layer cost):** `L10`, `L25` and `L50` stack n layers over the 1080p
  EXR half footage: a background solid, the blurred footage, then layers cycling through that
  footage, 1080p PNG16, ProRes 422 HQ and H.264, and 2160p EXR half and ProRes 422 HQ (each
  scaled, rotated and positioned on a grid, with its own opacity and one of six blend modes;
  every fifth layer is a small solid), and an animated text layer on top. They carry only the
  OpenEXR export job.

Each A and B project carries three export jobs (ProRes 422 HQ MOV, H.264 MP4, OpenEXR sequence) written to
`exports/<project>/` next to it, replacing earlier runs and evaluating every frame
(`useCachedFrames` off). The runner builds the projects into its work directory on first use, per
media root, so a run against another root never reuses projects that point elsewhere.

## Running

Build `bloom-cli` (with Python) and the packed RAM-fill harness `bloom_ui_gpu_packed_viewer_test`
in the build directory, then:

```sh
python3 tools/bench/run_bench.py run --suite standard --build-dir build/pw-desktop \
    --media-root "$BLOOM_BENCH_MEDIA" --work-dir bench-work [--gpu-lock-file <lock>]
```

The runner never uses a display: Bloom processes run with `QT_QPA_PLATFORM=offscreen` and without
`DISPLAY`/`WAYLAND_DISPLAY`. Keep the machine otherwise quiet; on a shared machine pass
`--gpu-lock-file` so every Bloom process holds the GPU lock.

| Suite | Tiers | Media | Measurements |
|---|---|---|---|
| `quick` | 1080p | PNG16, EXR half, ProRes 422 HQ, H.264 | A and B; Full and Quarter, cold and warm; playback; ProRes export of the B EXR project |
| `standard` (default) | 720p, 1080p, 2160p | all | A and B; Full, Half, Quarter; cold and warm; playback; all exports of the B projects of PNG16, EXR half, ProRes 422 HQ and H.264 |
| `full` | all four | all | everything, exports for every project |
| `vfx8k` | 4320p | all | everything |
| `layers` | 1080p | EXR half (plus the L mix) | L10, L25, L50; Full, Half, Quarter; cold and warm; cached playback after every fill; playback; no exports |

`--tier`, `--format`, `--variant`, `--resolution`, `--cache`, `--export`, `--no-export` and
`--no-playback` narrow a suite. `--playback-only` measures cold playback alone (no fills, no
exports); `--playback-resolution` (repeatable, default `Full`) picks the playback measurements'
resolutions and `--playback-repeat` how many fresh processes each gets. `--workspace` runs the harness with the application's window
instead of a bare Viewer: `MainWindow` with every foundation editor (Timeline, Properties, Node,
Assets, ...), plus the application's `AssetController`, `AudioPlaybackSession`, live-session
adapter and playback read-ahead, so per-tick work outside the Viewer is measured too. The harness
flag `--bench-workspace-no-assets` leaves the `AssetController` out, to separate its cost.

### What is measured

Every measurement is a fresh process.

- **RAM-preview fill** (`kind: fill`): the harness's `--bench-project` mode opens the project
  through `ProjectHost` and wires the preview like the desktop application (application scheduler
  and budget view, process memory ledger and `MemoryBudgetController` with the application's own
  memory wiring `connectApplicationMemoryBudget`, which applies the RAM/VRAM headroom, active-work
  claims and evict-on-demand, the qualified display bootstrap, settings-derived cache budgets, the
  packed GPU preview service, fill width from measured capacity). After the Viewer's first frame it starts the RAM preview and reports
  `fillFps` = prepared frames / fill seconds at the selected resolution (`Full`, `Half`,
  `Quarter`). **Cold**: the footage is dropped from the OS page cache first
  (`posix_fadvise(DONTNEED)`; `coldDropped` records whether the platform could). **Warm**: the
  footage is in the page cache. Bloom's own caches are always empty because the process is new.
- **First frame** (every harness run): `firstFrameSeconds` is the time from showing the Viewer
  until it paints a Current frame, with the service startup inside it (`serviceReadySeconds`, when
  the GPU preview service left `Initializing`). `firstFrameProvider` is the frame's display
  provider, and `firstFrameCpuFallbacks`/`firstFrameCpuFallbacksByReason` count the CPU fallbacks
  up to that paint; it must be 0 on a GPU route (#542).
- **Cached playback** (`cachedPlaybackFps`): when a fill completed, the transport plays from the
  start for `--play-seconds` and counts the distinct frames the Viewer actually showed.
  `cachedPresentedFps` counts every change of the shown frame, so it stays meaningful when a
  short work area loops. `cachedPlayUiBusySeconds`, `cachedPlayMaxSliceMs` and
  `cachedPlayLongSlices` report the UI thread's event-loop time over the run, its longest slice
  and the slices longer than one frame period. `cachedPlayCacheHits`/`Misses`/`StaleDrops`/
  `Evictions`, `cachedPlayRevisionChanges` and `cachedPlaySnapshotRefreshes` say whether a cached
  run really stayed cached, and `lastFrameDiagnostics` carries the last frame's preview
  diagnostics. `realTimeRatio` (playback runs) and `cachedRealTimeRatio` are the session time the
  transport advanced, unwrapped across loop restarts, over wall time: 1.0 means playback held real
  time by dropping frames rather than slowing down.
- **Playback** (`kind: playback`): a cold start with background read-ahead and no RAM fill;
  `playbackFps` = distinct frames shown / seconds, next to the frames the transport asked for. The
  transport waits for the read-ahead's frames as the application's does (#497), so the two counts
  match unless frames the read-ahead cannot cache play through the ordinary playback request.
- **Scrub** (`kind: scrub`, `--scrub N`): after the first frame the harness jumps to N frames
  spread over the work area, alternating between its two halves so no target is near the last one
  or inside the read-ahead window, and times each from `setCurrentTime` until the Viewer shows it
  Current (`scrubUncachedSeconds`, with `Median` and `Max`). It then clears the preview frame cache
  and visits the same frames again (`scrubRevisitSeconds`): what the media caches below the
  finished frames (decoded video, decoded media, source revisions) save.
- **Start of playback** (both playback measurements, prefixed `play` or `cachedPlay`):
  `FirstMotionSeconds` is the time from Play until the Viewer first shows a Current frame other
  than the start frame; `DroppedFirstSecond` and `PaintedFirstSecond` are the footer's dropped
  count and the distinct Current frames shown one second after Play; `DroppedTotal` is the dropped
  count at the end of the run; `ReadAheadWidth` is the read-ahead window at Play; and
  `FrameCachedSeconds` lists, for each of the first 24 frames from the start, when it entered the
  preview cache (-1 when it never did). The playback preroll (#404) adds `PrerollOutcome`
  (`immediate`: the frames were cached at Play; `ready`: the hold ended when they were; `cap`: the
  0.5 s maximum started the clock without them), `PrerollHeldSeconds`, `PrerollFrames` (the window
  the hold waited for), `ReleasedSeconds` (when the clock started, from Play) and
  `DroppedFirstSecondAfterRelease`.
- **Export** (`kind: export`): `bloom-cli export --job <id> --replace`; `secondsPerFrame` = process
  wall time / frames, including start-up. The export's own provenance gives `gpuFrames` and the
  CPU-reference frame count (`cpuFallbackFrames`). An export that cannot run on this
  computer is recorded as `outcome: unavailable`. H.264 and HEVC always have a bundled encoder.
- **GPU stages**: `uploadSubmitSeconds`, `uploadFenceSeconds`, `processingSubmitSeconds`,
  `processingFenceSeconds` and `packedTransferSeconds` are the device owner's wall time per stage
  over the measured window (from the preview service counters), with `residentGraphJobs`,
  `nativeDispatches` and `uploadsAhead` (uploads the scene executor began ahead of their own step,
  beside another native job, #405).
- **GPU owner thread** (#477): `ownerDispatchSeconds`, `ownerStageSeconds`,
  `ownerResidentSeconds`, `ownerServiceSeconds` and `ownerWaitSeconds` split the preview service's
  owner-thread wall time over the measured window by loop phase: scheduler GPU tasks, preview stage
  results and the general native display, the resident scene executor (uploads, recording,
  submission, fence polls and the packed transfer), housekeeping (cache purge, queue timings,
  presentation, capacity refresh), and idle waits for work or a fence. The first four are the
  owner's busy time; busy over `fillSeconds` is its share of the fill. `scenesAhead` counts frames
  whose scene executor began while the previous frame was still displaying or transferring.
- **Device memory** (#477): `deviceMemoryAllocations`, `deviceMemoryFrees` and
  `deviceMemoryAllocatedBytes` are the native device-memory allocations and frees the device
  allocator made over the measured window (each a driver call; reused storage makes none). Divide
  by the frames filled for allocations per frame.
- **Sequence read-ahead** (#477): `readAheadCaptured` counts the image-sequence read-ahead's stored
  captures, `readAheadWithdrawn` the queued captures a frame withdrew to read the file itself, and
  `readAheadAwaited` the captures in flight a frame waited for instead of reading the file again,
  and `readAheadDeferred` the times a worker with queued captures waited for frames' own source
  reads; `readAheadDecoded` the members whose native samples it decoded ahead of their frame and
  `readAheadDecodesSkipped` the decodes it did not run because the transport had passed their
  frame, and `readAheadDecodesYielded` those it did not run because frames' own decodes held the
  cores (#386; process totals).
- **Host preparation** (#422): `windowCpuSeconds` is the process CPU time over the measured window
  (fills and playback), `uploadBytes` the host-to-device bytes uploaded in it (from the GPU
  transfer telemetry), and `decodeStageSeconds` and `colourConvertStageSeconds` the summed wall time
  of the `Decode` and `ColourConvert` telemetry stages over the window, across every thread. Divide
  by the frames filled for per-frame figures.
- **Resources**: `gpuBusyPercent` (and peak) from `nvidia-smi` sampled every 200 ms over the
  measured window (NVIDIA only; absent elsewhere); `fillCoresBusy` = process CPU seconds / wall
  seconds of the fill; `cpuCoresBusy` for exports from process-tree sampling; `peakRssBytes` is the
  harness process peak (`getrusage`, or the peak working set on Windows) and, for exports, the
  sampled peak of the process tree; `cpuFallbackFrames` must be 0; `admissionRefusals` and
  `residentDetail` report GPU admission refusals and the resident path's last detail (the 8K tier's
  per-image ceiling shows up here).
- **Memory attribution** (fills and playback runs): `memoryAtPeak` is the harness's own sample with
  the highest RSS over the measured window, taken every 250 ms. Its figures come from the runtime
  telemetry contract, `ProfilerSnapshot` ([Profiler telemetry feed](../architecture/profiler-feed.md)),
  the same numbers the Profiler panel and MCP `profiler.snapshot` show, under the field names
  earlier results used: `rssBytes` and `anonBytes` (Linux statm resident and resident minus shared;
  anonymous is absent elsewhere), `fileBytes` and `shmemBytes` (absent: the snapshot does not
  split them at the sampling rate), and the ledger's per-pool rows summed as `reusablePoolBytes`
  (every pool on the process memory ledger), `previewCacheBytes` (the Preview frames row),
  `operationCacheBytes` (the two Operation cache rows, with `decodedMediaBytes` the decoded-media
  row) and `otherPoolBytes` (the rest: decoded video, the disk-cache write queue and the asset
  pools). The snapshot also gives `stagingLiveBytes` and `stagingPeakBytes` (host upload staging,
  scratch), `admissionClaimBytes` (host bytes admission granted to running work, outside the
  preview retention) and `unattributedBytes` (anonymous minus pools, pending retirement, staging
  and claims). Pool bytes are host allocations, each distinct allocation counted once, including
  bytes pending asynchronous retirement (`operationPendingBytes`, `reusablePendingBytes`);
  `reusableDeviceBytes` is device-resident pool bytes, outside the host totals
  ([what the pools count](../architecture/evaluation-primitives.md#what-the-pools-count)). Four
  figures the snapshot does not carry are read from their owners as before:
  `operationSharedBytes` (what per-entry accounting would have added for storage several entries
  share) and the processing policy's `hostClaimBytes` (including retained preview frames),
  `hostUsableBytes` (available memory within the OS limit minus the RAM headroom) and
  `deviceClaimBytes`. A value the platform cannot supply is `null`, never 0. Without
  `--bench-profiler` no sampler thread runs (a closed Profiler) and each tick takes one sample on
  the harness thread; with it the sampler thread runs over the window, as an open Profiler does,
  and the harness prints the window's last snapshot as a `BENCH_PROFILER` line in the
  `profiler.snapshot` JSON. `peakDeviceBytes` is the
  process tree's peak device memory as the driver reports it (`nvidia-smi --query-compute-apps`,
  NVIDIA only). Next to it, for device admission (#417): each memory sample also carries
  `deviceOwnedNativeBytes` (the processing root's own native image and buffer storage, what the
  device claims cover), `deviceLiveBytes` (Bloom's live device allocations) and
  `deviceHeapUsageBytes` (the driver's `VK_EXT_memory_budget` heap usage; present only with
  `--bench-profiler`, which runs the sampler that queries it); and the result gives the run's
  high-water marks `peakDeviceClaimBytes` and `peakDeviceOwnedNativeBytes`, and over the released
  native phases `releasedNativePhases`, `releasedPhaseDeviceNeedBytes` and
  `releasedPhaseDevicePeakBytes` (sums of each phase's stated need and of the most storage it held
  at once), `peakPhaseDeviceNeedBytes` and `peakPhaseDeviceAllocationBytes`. The harness also prints a `BENCH_MEMORY` line with the same fields every ten seconds,
  which the run log keeps, so a fill that stops making progress shows where its memory is.
  Fills also report `cachedFrames` (frames still retained when the fill ends) and
  `previewFirstYieldedBytes`: bytes the other reusable pools gave back so a finished preview frame
  was kept instead of an earlier one (preview-first eviction priority during a fill). Over the
  fill alone, the operation cache's lookups (`fillOperationHits`, `fillOperationMisses`,
  `fillOperationPressureDrops`), its byte budget when the fill starts
  (`fillOperationBudgetBeforeBytes`) and the most it was given during the fill
  (`fillOperationMaxBudgetBytes`), and its sampled bytes (`fillOperationMaxBytes`,
  `fillOperationMeanBytes`) say whether a fill's reusable working set stayed cached. For host
  admission (#472), a fill reports the most host memory active work claimed while it ran
  (`fillHostWorkClaimMaxBytes`: claims minus retained preview frames) next to the most anonymous
  memory the process held (`fillAnonMaxBytes`), and every run reports, over the phases that
  finished source preparation, `finishedSourcePhases`, `finishedPhaseSourceNeedBytes` and
  `finishedPhaseRetainedSourceBytes` (sums of the source need each claimed, three times of which
  it held while preparing, and of the source bytes it actually retained),
  `peakPhaseSourceNeedBytes` and `peakPhaseRetainedSourceBytes`.

Process sampling uses `/proc` on Linux and `psutil` where it is installed; without either, CPU and
RSS sampling are reported as absent rather than guessed.

### Results

Each run writes `<work-dir>/results/<UTC timestamp>[-label].json`, a `.md` summary table and a
`.log` with every command and its output. The JSON holds:

- `machine`: CPU, logical cores, RAM, GPU name, driver and VRAM, OS;
- `bloom`: the source commit of the build and whether its tree was dirty;
- `media`: the root and its verification status per asset (`verified-exact` or
  `regenerated-compatible`);
- `measurements`: one row per measurement, keyed by tier, format, variant (A/B), kind, resolution,
  cache and export, with the fields above;
- `comparison` when `--baseline` was given.

Recorded runs: [RTX 4090 / Ryzen 7 9700X / 64 GB, main 6757dacbf against 27c0949c](benchmark-results-2026-10-31.md); [RTX 5080 / Core Ultra 9 285K / 64 GB, main 5ad4dc717](benchmark-results-2026-11-03.md).

### Baselines and storage comparisons

```sh
python3 tools/bench/run_bench.py run --baseline bench-work/results/<old>.json --threshold 10
python3 tools/bench/run_bench.py compare <new>.json <old>.json --threshold 10
```

A regression is a measurement whose fill fps, playback fps or cached playback fps fell, or whose
export seconds/frame, peak RSS or first-frame seconds rose, by more than the threshold (percent).
Any CPU-fallback frame, the first frame's included, is reported separately. Either makes the
command exit non-zero.

To compare storage, run the same suite with `--media-root` pointing at the other location (for
example a network share holding a copy of the media) and a `--label`; the media is verified there
first, and its projects are built separately. Compare the two results files with `compare`.

### Chart checks

```sh
python3 tools/bench/run_bench.py chart --media-root "$BLOOM_BENCH_MEDIA" --tier 1080p
```

For each chart asset it builds a project, renders frame 1 to a linear Rec.709 float EXR and reports
per patch the error in 8-bit sRGB code units (relative error for the EXR-only HDR values), alpha
error, and the siting shift: the centroid of each tiny square against its true centre, in pixels
(`maxAbsDx`, `maxAbsDy`). Tolerances: 0.5 codes for PNG16 and EXR, 3 for 8-bit video, 2 for 10-bit,
0.35 px for siting. From the PNG16 chart it then exports ProRes 422 HQ, ProRes 4444, DNxHR HQX,
H.264 and OpenEXR; each export's tags are probed and its first frame decoded with FFmpeg by those
tags (sRGB transfer, or BT.1886 for BT.709-tagged files) and compared, and the export is imported
back into Bloom and compared again. Results go to `<work-dir>/results/<timestamp>-chart.json` and
`.md`; the command exits non-zero when any import fails a patch or siting check.

## Scenario suite

The media and layer-count benchmarks above measure one cost at a time. The scenario suite
(`run_scenarios.py`, issue #540) builds realistic heavy comp shots and drives them the way an artist
or an agent does, through the public MCP interface only, so a failure that needs a whole shot (an
effect chain that cannot be admitted, a CPU fallback on export) shows up.

| Tool | Runs in | Purpose |
|---|---|---|
| `scenario_spec.py` | Python 3 | The scenarios, as plain data |
| `run_scenarios.py` | Python 3 (numpy for the parity check) | The runner: builds, measures, checks, compares |
| `test_run_scenarios.py` | Python 3 | Its MCP client, rates, comparison, checks and parity gate (`bloom.bench.scenario-suite`) |

### Scenarios

Each scenario is a generic composite of CG render passes made only from the synthetic footage
above (never production media): the multilayer EXR (`Composite.Combined`) stands in for a CG
beauty render, the EXR half and float sequences for further passes and elements, the PNG
sequences for a plate; a procedural layer (a Fields generator) needs no media. Pixel sizes are
given at 1920 wide and scale with the composition (clamped to each parameter's range).

| Scenario | Size | Frames | Shot |
|---|---|---|---|
| `two-layer-glow-2k` | 1920×1080 | 48 | A PNG16 plate under a CG beauty pass with one Physical Glow (size 200): the smallest shot that needs a whole effect chain admitted at once (#539) |
| `cg-comp-2k` | 1920×1080 | 24 | A blurred (48 px), graded plate; the beauty pass with Light Wrap (from the plate), Classic Glow (128), Drop Shadow, Curves and Levels; an EXR half element with Physical Glow (160) and Drop Shadow; an EXR float highlights pass with Streak Glow (400); a nested Merge of two passes (one graded) through a 96 px blur and a Classic Glow; an adjustment layer with Grade and Curves over the stack |
| `cg-comp-4k` | 3840×2160 | 12 | The same comp at 4K |
| `noise-glow-2k` | 1920×1080 | 24 | A PNG16 plate under a full-frame procedural Noise (Spline, Dynamic Twist, 8 octaves, its evolution keyed so every frame is new) screened on top through a Physical Glow (160) (#525; the issue's Noise-driven Displace waits for Displace, #348) |
| `cryptomatte-mattes-2k` | 1920×1080 | 24 | A PNG16 plate under the beauty pass shaped by its own Cryptomatte (#383): the pillars and cards minus the veil (a Mask Combine Subtract of two selections) as a matte layer mask and as the mask input of a warm Grade (then Curves), and the cards as the node track matte of an EXR float highlights pass through a 32 px blur and a Physical Glow (128) |

The work areas are short and the export renders only the first `--export-frames` frames (default
4), so a whole run holds the GPU for minutes, not hours, even when a shot renders slowly. A scenario
lists in `knownFailures` the checks an open issue makes fail (#549: the Cryptomatte shot's GPU busy
over its fill; #386: the Noise shot's GPU busy, which its fill is too short to judge: on 2026-11-06
it filled 24 frames in 0.4 s, so the 200 ms `nvidia-smi` samples, each itself an average over the
driver's own window, caught two or three readings at 31% average and 50% peak; a fair judgement
needs a longer work area, which would change the scenario's baseline, so the marker stays and the
check is not weakened). The light 2K shot's GPU-busy marker was removed when #386's bounded blend
and OCIO batches and fence waits made it pass (64% busy, 2026-11-06). After #549 (2026-11-06, RTX
4090, alternating runs) `cryptomatte-mattes-2k` shows its first frame in 3.0 to 3.1 s (4.4 to 4.9
before), fills at 7.2 to 8.1 fps (2.9 to 3.2) with the GPU 35 to 51% busy (10 to 18%), plays cold
at 16 fps (2.7), scrubs in 0.8 to 0.9 s (2.1) and exports its 4 frames on the GPU at 0.67 s/frame
(1.02). The fill's GPU busy passed the 50% bar in one of three runs: each frame's host work is
now mostly the ZIP decode of the multilayer EXR, once for the beauty and once more for the
Cryptomatte rank channels, so its marker points at #549. Two of those runs failed
`claims-within-factor` (3.02 and 3.18 against 3): the fill's host use fell further than the
preflight claim for a channel read, which #549 did not change ([results](benchmark-results-2026-11-06-549.md)).

When a new heavy node lands (motion blur, depth effects, Fields), add a scenario that uses it and
leave the existing ones unchanged, so their baselines stay comparable.

### The instance

The runner starts a separate Bloom (`apps/bloom/Bloom`) for every measurement, offscreen
(`QT_QPA_PLATFORM=offscreen`, no `DISPLAY`/`WAYLAND_DISPLAY`, no session bus, so no desktop
notifications), in a scratch home of its own: `HOME` and every XDG folder point into
`<work-dir>/scratch`, so its preferences, MCP token, crash recovery and caches are its own and are
deleted when it stops. Its preferences file enables the in-app MCP server on a free loopback port;
the runner reads the token from the instance's own data folder. Sounds, notifications and audio
playback are turned off through `preferences.set`. The GPU driver's shader cache is shared across
the suite's instances (`<work-dir>/driver-cache`), as it is across an artist's sessions; Bloom's own
caches are not. The user's running Bloom, projects and settings are never touched. The isolation
works through the XDG variables, so the runner runs on Linux only and refuses elsewhere.

### What is measured

Per scenario, each step in a fresh instance:

1. **Build**: `project.new`, then one `transact` per operation (import, composition, layers,
   effect chains through `bloom.node.add-fed-by`, the nested Merge through `bloom.node.add` and
   `bloom.node.connect`, the adjustment layer, the work area), then `project.save-as`.
2. **Fill**: footage dropped from the page cache (`coldDropped`), `project.open`,
   `composition.activate`, `viewer.set` (Full, fit), `transport seek 0`, then the first Current
   Viewer frame of frame 0 (`firstFrameSeconds` from `project.open`, `firstFrameProvider`,
   `firstFrameCpuFallbacks`; `--first-frame-timeout`, default 120 s). Then `transport ram-preview`:
   `fillSeconds` until the run stops caching, `fillFps` = cached frames / fill seconds, and the
   cached playback the RAM preview starts for `--play-seconds`. A fill that caches no new frame for
   `--stall-seconds` (default 120) is stopped and recorded as `fillStalled`; one still running after
   `--fill-timeout` (default 600 s) as `fillTimedOut`.
3. **Playback**: cold, `transport play` from frame 0 for `--play-seconds`, no fill.
   Playback and scrub are skipped (and recorded as errors) when the first frame never arrives.
4. **Scrub**: `transport seek` to `--scrub` frames alternating between the work area's halves, each
   timed until the Viewer shows that frame Current (`scrubMedianSeconds`, `scrubMaxSeconds`).
5. **Export**: `render` of the first `--export-frames` frames (default 4) to a float OpenEXR
   sequence (`FlatExrRgba32fLinRec709SceneV1`, the final render route, with a progress token so
   the answer streams; at most `--export-timeout`, default 900 s), `exportSecondsPerFrame` = wall
   time / frames, the job's provenance from `export.status` (`gpuFrames`, `cpuFallbackFrames` = its
   CPU reference frames) and the run log's warnings (`exportLog`).
6. **Parity**: `--reference-frames` frames (default 1, the first exported frame; more are spread
   up to the last exported frame; a 4K CPU reference frame of the comp takes about 9 minutes)
   rendered by `bloom-cli render` with no Vulkan driver to find (`VK_ICD_FILENAMES` and `VK_DRIVER_FILES` point
   at a missing file), so they are CPU reference frames (the CLI's `device` line must say
   `gpu-frames=0 cpu-reference-frames=1`), compared with the exported frames.

A watcher on a second MCP session reads `profiler.snapshot` every 250 ms over each measured window
(schema 5, see [Profiler telemetry feed](../architecture/profiler-feed.md)): the Viewer's displayed
frame (freshness, display provider, frame index), the GPU preview route's cumulative CPU fallbacks
by reason, admission refusals, completions and capacity waits, the RAM Preview's range and longest
contiguous cached run, the host and device admission claims, anonymous memory, Bloom's own device
allocations (`gpu.liveBytes`), the usable host and device capacity, the Preview frames pool and the
running tasks' names. Playback rates come from the playhead (`viewer.playheadFrame`, unwrapped
across loops) and the dropped-frame count: `playTransportFps` is the transport's rate,
`playbackFps` the frames it showed (advanced minus dropped) per second; the profiler's own playback
rate counts only while a Profiler panel is open. `gpuBusyPercent` over the fill and the export is
`nvidia-smi` utilization sampled every 200 ms (machine-wide, NVIDIA only).

### Pass criteria

Each check is `pass`, `fail`, `not-measured` (a figure the platform or a failed step did not give)
or `known-failure` (it did not pass, and the scenario lists it with the open issue that causes
it). `fail` and `not-measured` fail the run; `known-failure` is reported with its issue and does
not. A listed check that passes is reported as passing, with a note that its entry can go. Each
check's detail carries a `message` with the measured figures.

| Check | Passes when |
|---|---|
| `no-cpu-fallback` | Over the fill, cached playback, playback, scrub and export: zero GPU preview CPU fallbacks (the first frames included), every displayed frame from a GPU scene evaluation (`gpuResident` or `gpuPreparedPacked`; `gpuNeutral` is a CPU-evaluated scene), no task named as a CPU reference or fallback, and zero export CPU reference frames |
| `claims-within-factor` | In every measured window, the peak device claims are at most `--claim-factor` (default 3) times the peak of Bloom's own device allocations, and the peak host claims at most that factor times the peak anonymous memory. The default follows the stated device bound (own allocation plus two phases' needs; measured 1.1 to 2.1 times, [GPU backend](../architecture/gpu-backend.md)) |
| `viewer-and-fill-progress` | The Viewer showed a first frame in every measurement within `--first-frame-timeout`, every scrub target within `--scrub-timeout`, and the fill never went `--stall-seconds` without caching a frame ("the fill cached no frame for 120 s (0 of 12 cached)") |
| `claims-below-capacity` | Device claims never stayed above `--claim-capacity-percent` (default 90) of the usable device capacity for longer than `--claim-capacity-seconds` (default 60) in any measured window ("device claims stayed above 90% of usable capacity for 600 s"): work that waits for room instead of running |
| `fill-completes` | The fill ended by itself: not stalled and not timed out |
| `gpu-busy-during-fill` | The fill's mean GPU busy is at least `--gpu-busy-percent` (default 50) |
| `fill-contiguous-from-playhead` | The fill's longest cached run starts at the playhead (frame 0) at every sample and, when the fill ends, holds every cached frame (`maxOutOfOrderFrames` reports the most frames cached ahead of the run while frames were in flight) |
| `ram-preview-usable-memory` | The Preview frames pool never holds more host memory than the usable memory, its ceiling follows the usable memory (at least 90% of it: no fixed cap), and a fill that cached fewer frames than its range had reached at least 90% of the usable host memory or of the usable device capacity |
| `export-gpu-complete` | Every exported frame published, all of them GPU frames, zero CPU reference frames |
| `export-rate` | The fill completed, and the export's seconds per frame are at most `--export-fill-factor` (default 4) times the fill's seconds per cached frame plus `--export-allowance` (default 0.5 s, the float EXR write): a final render is not far slower than the preview of the same shot. A fill that stalled or timed out gives no preview rate, so the check fails |
| `export-cpu-parity` | Every compared frame passes the documented reference-parity gate ([GPU backend](../architecture/gpu-backend.md)): each finite RGB component within 2e-6 absolute or relative, alpha exact, no finite CPU value non-finite. A shot that keys alpha from tolerance-qualified colour holds alpha to the same 2e-6 instead (below). The detail reports each frame's max alpha difference and how many alpha samples are not exact |

The alpha gate follows the reference-parity profile's one exception (#545, [GPU
backend](../architecture/gpu-backend.md)): alpha that an effect derives from colour a device route
matches within 2e-6 rather than bit for bit is held to that 2e-6. The suite compares whole frames,
so it cannot attribute an alpha sample to the effect that produced it. It applies the 2e-6 alpha
gate to a shot when `scenario_spec.keyed_alpha()` finds such a key in the shot's spec: a Glow
(whose Luma Key derives coverage from colour) after a Gaussian blur, Light Wrap, Glow or Drop
Shadow, or after a Grade with a gamma or contrast or a Levels gamma, in the same chain, the nested
Merge it filters, or, for an adjustment layer, the stack below. The check's `gate` names those
keys. Every other shot keeps exact alpha. `two-layer-glow-2k` keys decoded footage, which both
routes produce bit for bit, so its alpha stays exact. `cg-comp-2k` and `cg-comp-4k` key the
beauty's colour after its Light Wrap and the nested passes' colour after a graded, blurred Merge,
so their alpha is held to 2e-6, and `cryptomatte-mattes-2k` keys the highlights' colour after
their blur. On 2026-11-05 (RTX 4090) frame 0 measured a max alpha difference
of 5.96e-8 at 2K (5 alpha samples not exact) and at 4K (6), and `two-layer-glow-2k` an exact alpha.

### Running a scenario suite

Build `Bloom`, `bloom-cli` and the media first, then, under the benchmark lock on a shared machine:

```sh
python3 tools/bench/run_scenarios.py run --build-dir build/pw-desktop \
    --media-root "$BLOOM_BENCH_MEDIA" --work-dir bench-work/scenarios [--scenario cg-comp-2k] \
    [--baseline <old>.json --threshold 10] [--label 4090]
python3 tools/bench/run_scenarios.py compare <new>.json <old>.json --threshold 10
python3 tools/bench/run_scenarios.py list
```

Results go to `<work-dir>/results/<UTC timestamp>-scenarios[-label].json`, `.md` and `.log` (every
instance's output and every reference render). The JSON holds the machine, the Bloom commit, the
limits used and per scenario its build, every measurement row and every check. The exported and
reference frames are deleted after each scenario unless `--keep-outputs` is given. With
`--baseline`, `fillFps` falling or `firstFrameSeconds` or `exportSecondsPerFrame` rising by more
than the threshold (percent, default 10) is a regression and fails the run. The suite runs on the
RTX 4090 workstation and, when it is used, on the RTX 5080 bench host.

Recorded runs: [RTX 4090 / Ryzen 7 9700X / 64 GB, main 8626d3918, after #544 and after its follow-up](benchmark-results-2026-11-05-scenarios.md);
[the same machine, before and after #386](benchmark-results-2026-11-06-386.md);
[the same machine, before and after #549](benchmark-results-2026-11-06-549.md).

## Node cost

Every new processing node ships with a cost case: `run_bench.py nodes` runs each native node
bench listed in `bench_spec.NODE_BENCHES` under the GPU lock and writes
`<work-dir>/results/<UTC timestamp>-nodes[-label].json` and `.md`. A node bench builds each case
as a real document and runs it through the production compiler, scene builder (the packaged shader
tools and the media context) and executor on the device, at 1080p and 2160p by default:

| Field | Meaning |
|---|---|
| Cold ms | The first run on a fresh scene cache and executor: uploads, programs and every dispatch |
| Warm ms | The same frame again on that executor: the output-cache hit a RAM preview serves (no dispatch) |
| ms/frame | The median rerun on one executor whose cache retains nothing, so every pass dispatches again, the tier's cases interleaved round robin so a change of device clock moves every case alike |

Each case names a cheap reference at the same resolution (a Grade) and, for a new node, its
closest existing comparable; the ratios of ms/frame to theirs are held to the budgets in
`bench_spec.NODE_COST_BUDGETS` (by default at most 4x the Grade and 2x the comparable; a case whose
cost is a count, such as N masks, carries its own). `run_bench.py nodes` and `run_bench.py compare
new.json old.json --threshold 10` fail on a case over budget, a case that did not run (except
`NODE_COST_KNOWN_FAILURES`) and, against a baseline, a case whose ms/frame or ratio grew by more
than the threshold.

```sh
python3 tools/bench/run_bench.py nodes --build-dir build/pw-desktop --media-root "$BLOOM_BENCH_MEDIA" \
    [--tier 1080p] [--repeats 9] [--baseline old-nodes.json]
```

### Matte workflow (#383)

`bloom_runtime_gpu_matte_node_bench` (a 64x64 smoke run is the CTest
`bloom.runtime.gpu_matte_node_bench_smoke`). Its references and comparables are a Grade, Luma Key
into Image Mix, path layer masks, a layer track matte, N Image Mix nodes each by its own Mask node,
and the Cryptomatte node's Image output; the Cryptomatte cases read the bench multilayer EXR and
its `CryptoObject` layer. Measured on 2026-11-06 (RTX 4090, 9 reruns, ms/frame):

| Case | 1080p | vs Grade | vs comparable | 2160p | vs Grade | vs comparable |
|---|---|---|---|---|---|---|
| Grade (reference) | 3.77 | | | 13.43 | | |
| Image to Mask into Image Mix | 5.36 | 1.42x | 1.20x Luma Key | 20.05 | 1.49x | 1.20x Luma Key |
| Mask Combine (Difference of two Mask nodes) into Image Mix | 10.04 | 2.67x | 2.24x Luma Key | 32.57 | 2.42x | 1.95x Luma Key |
| Grade with a Mask node on its mask input | 6.11 | 1.62x | 1.62x Grade | 19.91 | 1.48x | 1.48x Grade |
| 1 node Mask as a layer mask | 6.64 | 1.76x | 1.24x Image Mix chain | 21.31 | 1.59x | 1.13x |
| 4 node Masks as layer masks | 12.15 | 3.23x | 1.06x | 42.06 | 3.13x | 1.08x |
| 16 node Masks as layer masks | 36.17 | 9.60x | 1.02x | 120.07 | 8.94x | 1.01x |
| Node track matte (a Mask node) | 5.40 | 1.43x | 1.31x layer track matte | 17.56 | 1.31x | 1.01x |
| Cryptomatte, 1 ID, as a layer mask | 11.83 | 3.02x | 1.01x Cryptomatte Image | 74.94 | 6.27x | 1.06x |
| Cryptomatte, 16 IDs, as a layer mask | 11.65 | 2.98x | 1.00x | 74.91 | 6.27x | 1.06x |
| Cryptomatte, 64 IDs, as a layer mask | 11.67 | 2.98x | 1.00x | 74.85 | 6.26x | 1.06x |
| Cryptomatte, 256 IDs, as a layer mask | 11.66 | 2.98x | 1.00x | 75.94 | 6.35x | 1.07x |
| Cryptomatte, 16 IDs, on a Grade's mask input | 13.03 | 3.33x | 1.12x | 60.03 | 5.02x | 0.85x |

Cold runs took 2.3 to 10 ms at 1080p for the solid cases and warm runs under 0.01 ms with no
dispatch. The routes add one Multiply Alpha or Image Mix per matte: N node Masks cost N Mask nodes
plus one combine each, the same as N Image Mix nodes by those Masks (a path mask list stays one
kernel, about 0.8 of a Grade for any count). The Cryptomatte cases cost what the #364 node costs on
its own and are held to its Image output; their reference is the Grade over the same EXR (3.92 ms
at 1080p, 11.95 ms at 2160p). The Cryptomatte rows were re-measured for #549 (2026-11-06, two
alternating runs of 5 reruns each, means): the node now uploads each of the EXR's two rank layers
once per frame as one frame-sized data image (66 MB at 1080p, 265 MB at 2160p, all of it float
data that cannot cross as binary16) and runs one coverage pass per layer, so it costs about 3 EXR
Grades at 1080p (36.4 ms, 9.3x, before) and about 6 at 2160p, where the stacked rank upload of
the earlier layout was over the per-image ceiling and the cases did not run. At 2160p each 133 MB
layer's staging buffer is a fresh allocation every frame (about 11 ms of the frame). Their budget
is 7x the EXR Grade and 1.5x the Image output, and no case is a known failure. The Cryptomatte IDs
picked do not change the cost measurably up to 256.

## Platform notes

The tools are Python and run on Linux, macOS and Windows. Cold runs need `posix_fadvise` (Linux);
elsewhere they are recorded with `coldDropped: false`. GPU busy needs `nvidia-smi`. Process
sampling needs `/proc` or `psutil`. The remote-render orchestration used to produce the stored set
is not part of the repository; `bench_media.py render` is the supported way to regenerate locally.
