# Hardware use results: RTX 4090 profile, #457

Status: record, 2026-11-06 (paired before/after runs of the #457 changes)

This page records the measurements behind #457: the packed display transfer, path coverage and
OCIO LUT uploads reuse the device generation's command pool, buffer and fence instead of creating
them per frame, per shape and per upload; the preview owner keeps one `GpuDisplayTransfer` across
frames; a LUT upload waits once on its own fence; and every pipeline creation goes through one
`VkPipelineCache` per device generation, persisted in the per-user cache directory
([GPU backend](../architecture/gpu-backend.md), "Reused Command Sets And The Pipeline Cache"). Raw
results stay in the runner's work directory, outside git.

- **Builds:** before, main `71bbdc2d9`; after, the #457 change on it. Both RelWithDebInfo, clang,
  run alternately (before, after, before, after) on the same machine state.
- **Machine:** AMD Ryzen 7 9700X (8 cores, 16 threads), 64 GB RAM, NVIDIA GeForce RTX 4090 24 GB
  (driver 615.71.09), Arch Linux, Linux 7.2. A workstation in use: another Bloom was resident and
  another build lane ran between measurements. Runs of one cell vary by about 10%.
- **Harness:** `bloom_ui_gpu_packed_viewer_test --bench-project`, offscreen, RAM-preview fill at
  Full, and playback (`--bench-playback`, 8 s), each a fresh process. `--bench-pipeline-cache <dir>`
  gives the after build a pipeline-cache directory: *cold* is an empty directory, *warm* one an
  earlier run of the same project wrote (about 1 MB for `cg-comp-2k`). The before build has no
  pipeline cache. The projects are the `cg-comp-2k` scenario project and the synthetic 2160p PNG16 B
  project ([benchmarks](benchmarks.md#benchmark-projects)).

## Per-job command objects

Creating a command pool, a primary command buffer and a fence and destroying them after the job
took 184 to 189 µs on this driver (three runs of 5000, on the device owner thread); taking a parked
set back (a pool reset and a fence reset) took 0.03 µs. Before, every packed preview frame paid this
once for its display transfer, every path-coverage shape once, and every LUT upload once. A steady
preview frame now creates no command pool, fence or staging allocation for its transfer: the
staging buffer is the parked native storage of the frame before it (#477).

The saving is below the run-to-run noise of the preview owner's whole frame (owner busy time is
dispatch + stage + resident + service time from the service counters, per prepared frame):

| Cell | Measurement | Before | After |
|---|---|---|---|
| 2160p PNG16 B | Fill fps | 18.2, 19.9 | 21.0, 21.4 |
| | Owner busy per frame (ms) | 10.1, 11.1 | 10.3, 9.9 |
| | Packed transfer per frame, submit to host (ms) | 4.35, 4.84 | 4.21, 3.90 |
| `cg-comp-2k` | Fill fps | 8.48, 8.31 | 8.31, 8.02 (cold); 7.72, 8.09 (warm) |
| | Owner busy per frame (ms) | 25.0, 19.3 | 22.2, 20.5 (cold); 18.8, 20.9 (warm) |

Every run counted zero CPU-fallback frames.

## First frame after launch

`firstFrameSeconds` (showing the Viewer to a Current frame, the service start-up inside it) and
`serviceReadySeconds` on `cg-comp-2k`, whose first frame creates the pipelines of its 32-node
graph's OCIO programs and Bloom kernels:

| Driver shader cache | Measurement | Before | After, cold | After, warm |
|---|---|---|---|---|
| on (the driver default) | First frame (s) | 4.56, 4.45 | 4.58, 4.57 | 4.52, 4.61 |
| | Service ready (s) | 1.29, 1.26 | 1.28, 1.29 | 1.26, 1.28 |
| off (`__GL_SHADER_DISK_CACHE=0`) | First frame (s) | 5.83, 6.12 | 5.77, 5.74 | 4.67, 4.57 |
| | Service ready (s) | 1.33, 1.35 | 1.34, 1.36 | 1.24, 1.29 |

- **The NVIDIA driver already keeps its own disk cache of compiled pipelines** (shared by every
  process of the user), so with it on, a warm Bloom cache changes nothing on this machine: the
  pipelines were never recompiled at launch. The rest of the first frame is the OCIO shader
  compile on CPU workers and the media decode, which the pipeline cache does not cover.
- **Without the driver's cache, the Bloom cache saves 1.2 to 1.5 s of the first frame** (5.8 to
  6.1 s before and cold, 4.6 to 4.7 s warm), the time the driver spent compiling this project's
  pipelines. That is the launch on a driver without its own persistent cache, or after the
  driver's cache was dropped, and also each route rebuild on one device generation, which now hits
  the in-memory cache.
- A cold cache costs nothing measurable: the after build with an empty directory matches before.

## Playback

2160p PNG16 B, cold playback, 8 s, from a fresh process:

| Measurement | Before | After |
|---|---|---|
| Playback fps | 22.0, 22.1 | 22.0, 22.0 |
| First motion after Play (s) | 0.47, 0.46 | 0.46, 0.47 |

## Scenario suite

`run_scenarios.py run --scenario two-layer-glow-2k --scenario cg-comp-2k --export-frames 4`,
alternating builds (before, after, before, after). The suite gives every Bloom instance its own
empty cache folders, so the after build runs with a cold pipeline cache throughout:

| | `two-layer-glow-2k` before | after | `cg-comp-2k` before | after |
|---|---|---|---|---|
| Fill fps | 23.4, 19.9 | 22.9, 24.3 | 5.45, 5.71 | 5.29, 5.58 |
| First frame (s) | 3.03, 1.70 | 2.65, 2.56 | 5.76, 5.00 | 5.06, 4.99 |
| Export s/frame (GPU frames of 4) | 0.53, 0.55 (4) | 0.49, 0.51 (4) | 1.20, 1.27 (4) | 1.22, 1.24 (4) |
| Cold playback fps | 22.8, 22.8 | 22.9, 22.9 | 10.2, 10.4 | 6.8, 10.6 |
| GPU busy during the fill, mean (%) | 53.6, 79.4 | 53.3, 42.0 | 83.3, 76.6 | 73.0, 78.3 |

Every run counted zero CPU-fallback frames and passed CPU parity. Each build failed one check once,
both within the known noise: `cg-comp-2k` `export-rate` (before 1.27 s/frame and after 1.24 s/frame
against limits of 1.20 and 1.22 s, both derived from that run's own fill), and the after build's
second `two-layer-glow-2k` `gpu-busy-during-fill` (42% over a 2 s fill, about ten 200 ms GPU
samples). The after build's first `cg-comp-2k` cold playback (6.8 fps) is a single outlier; the
other three runs are 10.2 to 10.6.
