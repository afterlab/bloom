# Benchmark results: RTX 5080 profile, main 5ad4dc717 (#395)

Status: record, 2026-11-03 (complete: all 86 cells of the queue ran, 86 ok, 0 failed)

This page records the standard benchmark suite ([benchmarks](benchmarks.md)) on the second hardware
profile, the RTX 5080 bench host, and compares it with an earlier run of the same queue on the same
host under a 20G memory cap. The RTX 4090 profile is on [its own page](benchmark-results-2026-10-31.md).
The raw results JSON stays in the runner's work directory, outside git.

- **This run:** main `5ad4dc717e92b30f25dc2cd341b9909ef8f2bd3b` (clean tree), 2026-11-03, cell cap `MemoryMax=56G`.
- **Archived run:** main `7ca80c84`, 2026-11-02, cell cap `MemoryMax=20G` (same 86 cells).

## Hardware profile and setup

| | |
|---|---|
| Profile | **RTX 5080 / Core Ultra 9 285K / 64 GB**: Intel Core Ultra 9 285K (24 logical cores), 64 GB RAM (61 GiB visible), NVIDIA GeForce RTX 5080 16 GB (driver 595.99.02), Ubuntu 26.04, Linux 7.0 |
| Media | `<media-root>`; every asset `verified-exact`. The 4320p tier has no repository manifest entry: the queue encoded its ProRes 4444 and HEVC 10-bit videos from the 4320p PNG16 master (already present) and recorded them in a private manifest copy |
| Harness | `bloom_ui_gpu_packed_viewer_test --bench-project --require-device`, headless (`QT_QPA_PLATFORM=offscreen`, no display), GPU lock around every Bloom process, 600 s harness timeout |
| Memory | One runner per cell in `systemd-run --user --scope -p MemoryMax=56G -p MemorySwapMax=0` (host 61 GiB). Bloom sizes its caches from the cgroup limit, so the cap is part of the measured configuration. The archived run used 20G, as the RTX 4090 profile did |
| Safety | Watchdog: kill the cell above 90 C GPU or below 3 GiB host `MemAvailable`; it never fired. Observed peak GPU temperature 62 C; lowest host `MemAvailable` 40 GiB |
| Codecs | FFmpeg 8.1.2 with the bundled codecs |
| Build | RelWithDebInfo, clang-22, system Qt 6.10, repository dependency superbuild prefix (not rebuilt for this run) |
| Machine state | Dedicated host, no other Bloom session, no other builds during the queue |
| Repeats | One run per cell, except the 1080p ProRes 422 HQ B recheck (three runs). Fills cover 240 frames (48 at 4320p); playback is 6 s from a cold start |
| Wall time | 7 h 42 min for the queue (11:05 to 18:47 UTC), 27,712 s summed over cells. The archived 20G run summed 28,754 s |

How to read the tables: values are `fillFps` (prepared frames / fill seconds) or `playbackFps`
(distinct frames shown / seconds); `–` means not measured in that cell. Video fills prepare 239 of
240 frames, so the last video frame is never prepared; that is not marked. Other incomplete fills
show `(prepared/total)` and ended at the 600 s harness timeout. GPU busy is the mean `nvidia-smi`
utilisation over the Full fills and playback; peak RSS is the largest harness RSS over all
measurements of the cell; CPU-fallback frames is the largest `cpuFallbackFrames` over all fills and
playback. Scope peak is the cgroup `memory.peak` of the whole runner scope (includes page cache and
child processes), so it can exceed the harness RSS. Half and Quarter columns are `–` for the control
cells, which run Full only.

Unit-test check on the built tree: 163 of 167 passed (the nvenc test is skipped). Failures on this
host: `bloom.host.export-queue-service` (its HEVC main10 export step reports "The encoder failed"),
`bloom.media.encode`, `bloom.ui.network-share-paths` (sidebar share mounts); `bloom.media.hevc-depth`
did not run. The bench targets built, and the 86 cells use pre-encoded media, so these did not
block the run, but the HEVC main10 encoder failure on this host is worth a follow-up.

## Results

All 86 cells ended `ok` with exit code 0. Every cell used the production GPU path:
CPU-fallback frames are 0 in every cell except the 4320p tier (see below).

### 720p

| Cell | Fill Full cold | Fill Full warm | Fill Half cold | Fill Quarter warm | Playback | GPU busy % | Peak RSS GB | CPU-fallback frames | Scope peak GB | Outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| 402rows-720p-exr_multilayer-A | 51.1 | 50.9 | 50.4 | 50.8 | 22.8 | 6.5 | 1.7 | 0 | 5.0 | ok |
| 402rows-720p-exr_multilayer-B | 44.3 | 44.7 | 38.7 | 39.3 | 22.0 | 6.3 | 1.8 | 0 | 5.1 | ok |
| 402rows-720p-h264-A | 139.4 | 138.2 | 145.6 | 146.9 | 23.0 | 6.4 | 1.4 | 0 | 1.2 | ok |
| 402rows-720p-h264-B | 23.8 | 23.8 | 23.0 | 23.4 | 22.0 | 6.4 | 1.6 | 0 | 1.5 | ok |
| 402rows-720p-hevc10-A | 51.3 | 51.5 | 52.0 | 52.1 | 22.8 | 7.3 | 1.4 | 0 | 1.2 | ok |
| 402rows-720p-hevc10-B | 14.7 | 14.4 | 13.8 | 12.8 | 22.0 | 5.8 | 1.6 | 0 | 1.4 | ok |
| 402rows-720p-av1-A | 50.6 | 51.5 | 50.1 | 50.0 | 22.7 | 8.6 | 1.4 | 0 | 1.2 | ok |
| 402rows-720p-av1-B | 18.1 | 18.2 | 17.9 | 11.2 | 22.0 | 6.3 | 1.6 | 0 | 1.4 | ok |
| 402rows-720p-vp9-A | 136.5 | 136.4 | 143.8 | 150.3 | 23.0 | 5.4 | 1.4 | 0 | 1.2 | ok |
| 402rows-720p-vp9-B | 29.8 | 29.8 | 28.0 | 27.9 | 22.0 | 6.6 | 1.6 | 0 | 1.4 | ok |
| controls-720p-png8-A | 113.8 | 114.1 | – | – | 23.0 | 5.9 | 1.4 | 0 | 1.6 | ok |
| controls-720p-png8-B | 72.3 | 73.0 | – | – | 22.0 | 10.5 | 1.6 | 0 | 1.7 | ok |
| controls-720p-png16-A | 89.2 | 87.5 | – | – | 22.8 | 4.6 | 1.5 | 0 | 2.3 | ok |
| controls-720p-png16-B | 61.6 | 62.1 | – | – | 22.0 | 9.4 | 1.6 | 0 | 2.5 | ok |
| controls-720p-exr_half_piz-A | 131.2 | 131.8 | – | – | 23.3 | 3.5 | 1.5 | 0 | 1.8 | ok |
| controls-720p-exr_half_piz-B | 73.5 | 74.5 | – | – | 22.3 | 7.1 | 1.6 | 0 | 1.9 | ok |
| controls-720p-exr_float_zip-A | 113.9 | 116.7 | – | – | 23.2 | 5.1 | 1.5 | 0 | 3.3 | ok |
| controls-720p-exr_float_zip-B | 81.9 | 82.6 | – | – | 22.3 | 10.4 | 1.6 | 0 | 3.4 | ok |
| controls-720p-prores422hq-A | 49.6 | 48.7 | – | – | 22.8 | 7.7 | 1.4 | 0 | 1.4 | ok |
| controls-720p-prores422hq-B | 17.7 | 24.4 | – | – | 22.0 | 7.6 | 1.6 | 0 | 1.6 | ok |
| controls-720p-prores4444-A | 44.2 | 47.4 | – | – | 22.8 | 8.7 | 1.4 | 0 | 1.9 | ok |
| controls-720p-prores4444-B | 22.6 | 22.5 | – | – | 22.0 | 6.1 | 1.6 | 0 | 2.1 | ok |
| controls-720p-dnxhr_hqx-A | 52.5 | 51.9 | – | – | 22.8 | 5.8 | 1.4 | 0 | 1.5 | ok |
| controls-720p-dnxhr_hqx-B | 26.2 | 26.1 | – | – | 22.0 | 6.1 | 1.6 | 0 | 1.5 | ok |

### 1080p

| Cell | Fill Full cold | Fill Full warm | Fill Half cold | Fill Quarter warm | Playback | GPU busy % | Peak RSS GB | CPU-fallback frames | Scope peak GB | Outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| 402rows-1080p-exr_multilayer-A | 22.8 | 23.1 | 21.8 | 22.2 | 21.8 | 7.0 | 3.0 | 0 | 10.1 | ok |
| 402rows-1080p-exr_multilayer-B | 20.9 | 21.6 | 19.4 | 18.7 | 18.2 | 10.0 | 3.1 | 0 | 10.2 | ok |
| 402rows-1080p-h264-A | 78.9 | 77.0 | 86.8 | 86.6 | 22.8 | 12.3 | 2.7 | 0 | 2.6 | ok |
| 402rows-1080p-h264-B | 14.2 | 14.5 | 14.4 | 14.1 | 22.0 | 12.4 | 3.0 | 0 | 2.9 | ok |
| 402rows-1080p-hevc10-A | 24.0 | 23.5 | 23.3 | 23.1 | 16.8 | 7.0 | 2.6 | 0 | 2.5 | ok |
| 402rows-1080p-hevc10-B | 5.3 | 5.0 | 6.2 | 6.7 | 16.0 | 7.4 | 3.0 | 0 | 3.0 | ok |
| 402rows-1080p-av1-A | 23.5 | 23.1 | 22.0 | 22.0 | 14.3 | 6.3 | 2.6 | 0 | 2.5 | ok |
| 402rows-1080p-av1-B | 8.3 | 6.6 | 7.5 | 7.1 | 13.7 | 7.8 | 3.0 | 0 | 3.0 | ok |
| 402rows-1080p-vp9-A | 76.9 | 78.1 | 86.7 | 97.7 | 22.8 | 12.5 | 2.7 | 0 | 2.5 | ok |
| 402rows-1080p-vp9-B | 19.6 | 20.0 | 17.2 | 20.3 | 22.0 | 14.1 | 3.0 | 0 | 2.8 | ok |
| controls-1080p-png8-A | 71.0 | 70.7 | – | – | 22.7 | 12.6 | 2.7 | 0 | 3.3 | ok |
| controls-1080p-png8-B | 44.5 | 44.7 | – | – | 21.5 | 20.8 | 3.0 | 0 | 3.6 | ok |
| controls-1080p-png16-A | 49.2 | 49.2 | – | – | 22.5 | 10.5 | 2.8 | 0 | 5.0 | ok |
| controls-1080p-png16-B | 38.6 | 38.8 | – | – | 20.7 | 20.4 | 3.0 | 0 | 5.2 | ok |
| controls-1080p-exr_half_piz-A | 74.3 | 74.0 | – | – | 23.0 | 9.1 | 2.8 | 0 | 3.8 | ok |
| controls-1080p-exr_half_piz-B | 49.2 | 48.8 | – | – | 22.0 | 16.5 | 3.0 | 0 | 4.1 | ok |
| controls-1080p-exr_float_zip-A | 62.6 | 62.6 | – | – | 22.8 | 10.1 | 3.0 | 0 | 7.1 | ok |
| controls-1080p-exr_float_zip-B | 45.2 | 46.3 | – | – | 22.0 | 20.9 | 3.2 | 0 | 7.4 | ok |
| controls-1080p-prores422hq-A | 22.7 | 23.0 | – | – | 21.7 | 5.5 | 2.6 | 0 | 2.8 | ok |
| controls-1080p-prores422hq-B | 8.4 | 7.6 | – | – | 21.5 | 7.5 | 3.0 | 0 | 3.3 | ok |
| controls-1080p-prores4444-A | 20.3 | 20.2 | – | – | 19.7 | 6.8 | 2.6 | 0 | 4.0 | ok |
| controls-1080p-prores4444-B | 7.2 | 12.9 | – | – | 17.8 | 7.0 | 3.0 | 0 | 7.1 | ok |
| controls-1080p-dnxhr_hqx-A | 24.6 | 24.7 | – | – | 22.3 | 4.5 | 2.6 | 0 | 2.8 | ok |
| controls-1080p-dnxhr_hqx-B | 9.2 | 10.3 | – | – | 21.5 | 7.9 | 3.0 | 0 | 3.3 | ok |

The three ProRes 422 HQ B rechecks are in [Other cells](#other-cells).

### 2160p

| Cell | Fill Full cold | Fill Full warm | Fill Half cold | Fill Quarter warm | Playback | GPU busy % | Peak RSS GB | CPU-fallback frames | Scope peak GB | Outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| 2160p-h264-A | 17.2 | 17.4 | 20.3 | 19.0 | 5.7 | 14.4 | 9.9 | 0 | 9.9 | ok |
| 2160p-h264-B | 5.8 | 3.9 | 4.2 | 4.3 | 2.0 | 16.3 | 11.0 | 0 | 11.0 | ok |
| 2160p-hevc10-A | 6.4 | 6.5 | 6.3 | 6.2 | 0.7 | 5.4 | 9.3 | 0 | 9.5 | ok |
| 2160p-hevc10-B | 1.3 | 1.5 | 1.3 | 2.9 | 0.2 | 9.5 | 11.0 | 0 | 11.1 | ok |
| 2160p-prores422hq-A | 5.6 | 5.6 | 5.5 | 5.4 | 3.5 | 8.0 | 9.3 | 0 | 10.8 | ok |
| 2160p-prores422hq-B | 1.9 | 2.6 | 2.1 | 2.2 | 5.0 | 12.3 | 11.1 | 0 | 12.6 | ok |
| 2160p-prores4444-A | 5.1 | 5.1 | 5.0 | 4.9 | 5.8 | 8.5 | 9.4 | 0 | 14.7 | ok |
| 2160p-prores4444-B | 2.4 | 2.7 | 1.8 | 1.7 | 0.2 | 11.0 | 11.1 | 0 | 16.4 | ok |
| 2160p-vp9-A | 17.6 | 17.7 | 21.1 | 19.5 | 3.8 | 14.1 | 9.9 | 0 | 9.7 | ok |
| 2160p-vp9-B | 5.0 | 7.7 | 8.5 | 5.0 | 2.5 | 19.5 | 10.9 | 0 | 10.8 | ok |
| 2160p-png8-A | 16.7 | 17.1 | 19.6 | 18.4 | 10.3 | 15.2 | 9.4 | 0 | 12.1 | ok |
| 2160p-png8-B | 9.7 | 9.8 | 13.3 | 14.4 | 3.2 | 22.6 | 10.1 | 0 | 12.8 | ok |
| 2160p-png16-A | 13.1 | 13.0 | 12.2 | 12.2 | 7.0 | 12.9 | 9.6 | 0 | 18.7 | ok |
| 2160p-png16-B | 9.4 | 9.6 | 11.6 | 11.1 | 4.3 | 24.2 | 10.2 | 0 | 19.4 | ok |
| 2160p-exr_half_piz-A | 15.6 | 15.5 | 15.0 | 14.5 | 8.8 | 11.1 | 9.7 | 0 | 14.3 | ok |
| 2160p-exr_half_piz-B | 10.7 | 10.7 | 14.7 | 13.7 | 4.3 | 23.6 | 10.3 | 0 | 14.9 | ok |
| 2160p-exr_float_zip-A | 11.3 | 11.3 | 10.5 | 9.3 | 4.3 | 9.7 | 10.3 | 0 | 27.5 | ok |
| 2160p-exr_float_zip-B | 9.5 | 9.6 | 9.2 | 9.1 | 3.7 | 20.9 | 10.4 | 0 | 27.7 | ok |
| 2160p-exr_multilayer-A | 4.2 | 4.3 | 4.2 | 4.2 | 0.8 | 6.2 | 10.4 | 0 | 35.6 | ok |
| 2160p-av1-A | 6.0 | 6.0 | 5.8 | 5.8 | 0.3 | 6.2 | 9.3 | 0 | 9.6 | ok |
| 2160p-exr_multilayer-B | 4.0 | 3.9 | 3.8 | 3.8 | 0.2 | 8.1 | 10.7 | 0 | 35.8 | ok |

### 4320p (reduced tier)

| Cell | Fill Full cold | Fill Full warm | Fill Half cold | Fill Quarter warm | Playback | GPU busy % | Peak RSS GB | CPU-fallback frames | Scope peak GB | Outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| 4320p-exr_half_piz-A | 2.6 | 2.6 | 2.6 | 2.7 | 1.5 | 7.9 | 11.4 | 0 | 14.6 | ok |
| 4320p-exr_half_piz-B | 0.0 (23/48) | 0.1 (30/48) | 2.3 | 2.5 | 0.2 | 7.8 | 12.3 | 772 | 21.2 | ok |
| 4320p-exr_multilayer-A | 0.6 | 0.6 | 0.6 | 0.6 | 0.2 | 2.9 | 16.2 | 0 | 29.1 | ok |
| 4320p-exr_multilayer-B | 0.0 (14/48) | 0.0 (14/48) | 0.6 | 0.6 | 0.2 | 2.6 | 16.8 | 225 | 26.1 | ok |
| 4320p-png16-A | 3.0 | 3.0 | 2.9 | 2.9 | 0.2 | 11.7 | 11.4 | 18 | 23.0 | ok |
| 4320p-png16-B | 0.0 (22/48) | 0.0 (23/48) | 2.5 | 2.7 | 0.2 | 7.6 | 15.2 | 917 | 22.7 | ok |
| 4320p-prores4444-A | 1.2 | 1.2 | 1.1 | 1.1 | 1.5 | 5.1 | 10.5 | 0 | 35.0 | ok |
| 4320p-prores4444-B | 0.0 (16/48) | 0.0 (17/48) | 0.6 | 0.6 | 0.2 | 3.7 | 11.9 | 217 | 24.5 | ok |
| 4320p-hevc10-A | 1.5 | 1.6 | 1.5 | 1.5 | 0.5 | 4.9 | 10.7 | 0 | 30.2 | ok |
| 4320p-hevc10-B | 0.0 (18/48) | 0.0 (18/48) | 0.8 | 0.5 | 0.2 | 4.2 | 11.7 | 253 | 17.4 | ok |

All 4320p B-variant cells fail their Full fill: it runs into the 600 s harness timeout with
14 to 23 of 48 frames prepared (0.0 fps) and 200 to 900 CPU-fallback frames, while the A variants
(a single layer) complete at 0.6 to 3.0 fps. Half and Quarter fills complete for every cell. The
cell outcome stays `ok` because the runner exit code is 0; the incomplete fill is a result, not a
runner failure.

### Other cells

| Cell | Fill Full cold | Fill Full warm | Fill Half cold | Fill Quarter warm | Playback | GPU busy % | Peak RSS GB | CPU-fallback frames | Scope peak GB | Outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| prores1080b | 8.2 | 11.2 | 7.5 | 9.4 | – | 4.8 | 3.0 | 0 | 3.3 | ok |
| prores1080b-rep2 | 7.5 | – | – | – | – | 4.5 | 3.0 | 0 | 3.2 | ok |
| prores1080b-rep3 | 10.6 | – | – | – | – | 6.6 | 3.0 | 0 | 3.3 | ok |
| exports1080-png16-B | – | 38.5 | – | – | – | 19.9 | 2.9 | 0 | 5.8 | ok |
| exports1080-exr_half_piz-B | – | 50.1 | – | – | – | 17.8 | 2.9 | 0 | 6.2 | ok |
| exports1080-prores422hq-B | – | 7.7 | – | – | – | 4.7 | 3.0 | 0 | 10.2 | ok |
| exports1080-h264-B | – | 14.5 | – | – | – | 7.9 | 3.0 | 0 | 6.8 | ok |

## Memory

Peak memory per cell is the Peak RSS and Scope peak columns above. Across the run:

- Largest scope peak: **35.8 GB** (2160p multilayer EXR B), then 35.6 GB (2160p multilayer EXR A)
  and 35.0 GB (4320p ProRes 4444 A). All are well under the 56G cap.
- **Zero** `oom_kill` events and zero `memory.max` hits in all 86 cells.
- The harness RSS of the 2160p video and image-sequence cells is 9 to 11 GB, the same as in the
  20G run: Bloom's own working set for these cells is far below 20G. The cap mattered for the
  scope peak (likely page cache and decoder processes; not separately measured), not for the harness.
- The cells that sat at the 20G limit in the archived run now peak at 14.7 to 35.8 GB (EXR float
  ZIP A/B, multilayer EXR A/B at 2160p, 4320p multilayer EXR A/B, 4320p PNG16 A, 4320p ProRes 4444 A).

## Comparison with the archived 20G-cap run

Different main commits (`7ca80c84` vs `5ad4dc717`) and different caps, so differences mix both.

### 2160p

| Cell | Fill Full cold (20G run → 56G run) | Playback fps | Scope peak GB | Outcome (20G run) |
|---|---|---|---|---|
| 2160p-h264-A | 19.4 → 17.2 | 4.7 → 5.7 | 10.0 → 9.9 | oom-killed-at-20G |
| 2160p-h264-B | 4.8 → 5.8 | 0.2 → 2.0 | 9.4 → 11.0 | oom-killed-at-20G |
| 2160p-hevc10-A | 6.4 → 6.4 | 0.8 → 0.7 | 9.5 → 9.5 | ok |
| 2160p-hevc10-B | 2.6 → 1.3 | 0.2 → 0.2 | 9.1 → 11.1 | ok |
| 2160p-prores422hq-A | 5.6 → 5.6 | 2.0 → 3.5 | 10.8 → 10.8 | ok |
| 2160p-prores422hq-B | 2.0 → 1.9 | 0.2 → 5.0 | 10.6 → 12.6 | ok |
| 2160p-prores4444-A | 5.1 → 5.1 | 3.3 → 5.8 | 14.7 → 14.7 | ok |
| 2160p-prores4444-B | 2.2 → 2.4 | 0.2 → 0.2 | 15.9 → 16.4 | ok |
| 2160p-vp9-A | 19.0 → 17.6 | 3.8 → 3.8 | 9.7 → 9.7 | ok |
| 2160p-vp9-B | 6.5 → 5.0 | 0.2 → 2.5 | 8.9 → 10.8 | ok |
| 2160p-png8-A | 18.0 → 16.7 | 10.8 → 10.3 | 12.0 → 12.1 | ok |
| 2160p-png8-B | 10.3 → 9.7 | 0.2 → 3.2 | 12.8 → 12.8 | ok |
| 2160p-png16-A | 12.9 → 13.1 | 6.8 → 7.0 | 18.9 → 18.7 | ok |
| 2160p-png16-B | 9.8 → 9.4 | 0.2 → 4.3 | 19.4 → 19.4 | ok |
| 2160p-exr_half_piz-A | 15.7 → 15.6 | 9.7 → 8.8 | 14.3 → 14.3 | ok |
| 2160p-exr_half_piz-B | 11.5 → 10.7 | 0.2 → 4.3 | 15.0 → 14.9 | ok |
| 2160p-exr_float_zip-A | 10.9 → 11.3 | 5.7 → 4.3 | 21.5 → 27.5 | ok |
| 2160p-exr_float_zip-B | 9.7 → 9.5 | 0.2 → 3.7 | 21.5 → 27.7 | ok |
| 2160p-exr_multilayer-A | 4.3 → 4.2 | 0.3 → 0.8 | 21.5 → 35.6 | ok |
| 2160p-av1-A | 6.1 → 6.0 | 0.2 → 0.3 | 9.6 → 9.6 | ok |
| 2160p-exr_multilayer-B | 3.6 → 4.0 | 0.2 → 0.2 | 21.5 → 35.8 | ok |

- **The two 2160p H.264 cells were not memory-killed.** The archived run labelled them
  `oom-killed-at-20G`, but their cgroup counters show 0 `oom_kill` events and 0 `memory.max` hits,
  scope peaks of 9.4 and 10.0 GB (half the cap), exit code 0 and complete result files (A: 19.4 fps
  Full cold, 239/240 prepared). The label came from a journal text match in the cell runner, not
  from the cgroup counters, and is a false positive. At 56G the same cells finish ok with the same
  numbers (A 17.2 fps, B 5.8 fps) and the same peaks (9.9 and 11.0 GB). The label check would need
  fixing (use `oom_kill` from `memory.events` alone) before the old label is trusted in any report.
- **8 cells hit `memory.max` in the 20G run** (EXR float ZIP A/B, multilayer EXR A/B at 2160p, 4320p
  multilayer EXR A/B, 4320p PNG16 A, 4320p ProRes 4444 A; 122 to 24,113 hits each). At 56G there are
  none. Their fill rates did not change materially (2160p EXR float ZIP A 10.9 → 11.3 fps, multilayer
  A 4.3 → 4.2 fps), so lifting the cap did not speed up these fills (the scope peaks reach 27 to 36 GB, which suggests the extra room went to page cache and decoder processes; that is an inference, not measured).
- **2160p multilayer EXR B: playback is still 0.2 fps** with the cap lifted (fill 3.6 → 4.0 fps,
  scope peak 21.5 (capped) → 35.8 GB, 0 CPU-fallback frames, 0 OOM, 0 cap hits). The 0.2 fps is
  therefore not a memory-cap effect. The A variant plays at 0.8 fps (0.3 before). The cell finished in 432 s with no
  stall and no host problem, so the earlier RTX 4090 host crash is not reproduced here.
- **Uncached playback of the B variants improved a lot but not uniformly.** The archived run showed
  0.2 fps on almost every 2160p B playback; now H.264 B 2.0, ProRes 422 HQ B 5.0, png8 B 3.2, png16
  B 4.3, EXR half B 4.3, EXR float B 3.7, VP9 B 2.5 fps. HEVC 10-bit B, ProRes 4444 B, AV1 A and
  multilayer EXR B still sit at 0.2 to 0.3 fps. Playback is one 6 s sample per cell and the two runs
  also differ in main, so treat the exact values as indicative.
- Fill rates are mostly close to the archived run (see the table). The visible moves: HEVC 10-bit B
  2.6 → 1.3 fps and VP9 B 6.5 → 5.0 fps (slower), H.264 B 4.8 → 5.8 fps (faster). With one run per
  cell, no variance estimate and two different main commits, these are not attributed to the cap.

### 4320p

| Cell | Fill Full cold fps (20G → 56G) | Prepared frames at timeout (20G → 56G) | Scope peak GB (20G → 56G) |
|---|---|---|---|
| 4320p-exr_half_piz-A | 0.9 → 2.6 | complete | 14.5 → 14.6 |
| 4320p-exr_half_piz-B | 0.0 (14/48) → 0.0 (23/48) | 14 → 23 | 10.3 → 21.2 |
| 4320p-exr_multilayer-A | 0.2 → 0.6 | complete | 21.5 → 29.1 |
| 4320p-exr_multilayer-B | 0.0 (8/48) → 0.0 (14/48) | 8 → 14 | 21.5 → 26.1 |
| 4320p-png16-A | 1.0 → 3.0 | complete | 21.5 → 23.0 |
| 4320p-png16-B | 0.0 (13/48) → 0.0 (22/48) | 13 → 22 | 11.8 → 22.7 |
| 4320p-prores4444-A | 1.1 → 1.2 | complete | 21.5 → 35.0 |
| 4320p-prores4444-B | 0.0 (0/48) → 0.0 (16/48) | 0 → 16 | 13.1 → 24.5 |
| 4320p-hevc10-A | 1.6 → 1.5 | complete | 17.7 → 30.2 |
| 4320p-hevc10-B | 0.0 (1/48) → 0.0 (18/48) | 1 → 18 | 7.4 → 17.4 |

The 4320p A fills are the same or faster (PNG16 A 1.0 → 3.0, EXR half A 0.9 → 2.6, multilayer A 0.2 → 0.6
fps). The B Full fills still time out, but they now prepare more frames within the 600 s harness
timeout (for example PNG16 B 13 → 22 of 48, ProRes 4444 B 0 → 16, HEVC 10-bit B 1 → 18), with
higher RSS (11.7 to 16.8 GB, against 5 to 7 GB). CPU-fallback frames in the B cells are in the same range in both runs (about 220 to 920 in this run; 28 to 30 for three of them and about 230 for two of them in the archived run).

## Conditions

- Sequence: the queue ran the 86 cells back to back with one runner scope each; no cell was
  retried or skipped, no watchdog fired, and the host stayed reachable throughout.
- Run order of the queue: 2160p video A/B, then the clean re-runs of the RTX 4090 cells (#402
  rows, ProRes recheck, 2160p image sequences, multilayer EXR A), the 720p/1080p controls, the 1080p
  exports, the 4320p suite, and finally 2160p AV1 A and 2160p multilayer EXR B.
- The multilayer EXR projects were rebuilt with `Composite.Combined` as the colour layer before the
  run (all eight projects).
