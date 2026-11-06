# Hardware use results: RTX 4090 profile, #549 one decode per frame

Status: record, 2026-11-06 (paired before/after runs of the second #549 change)

This page records the measurements behind the second #549 change. A frame decodes each multilayer
EXR part once for every channel it needs: the Cryptomatte rank channels and Shuffle channels are
read in the same decode as the part's colour layer, shared in the build and in the operation cache,
and decoded ahead with the sequence's samples ([media I/O](../architecture/media-io.md), "Channels
beside the colour layer"). A phase with a channel read now claims what that read holds, from its
sources' recorded extents, instead of the frame's preflight live-image peak
([GPU backend](../architecture/gpu-backend.md)). The first #549 change is recorded in
[benchmark-results-2026-11-06-549.md](benchmark-results-2026-11-06-549.md). Raw results stay in
the runners' work directories, outside git.

- **Builds:** before, main `9a699bd44`; after, the change on it (`667a0cc06`). Both RelWithDebInfo,
  clang, run alternately (before, after, before, after, before, after) on the same machine state.
- **Machine:** AMD Ryzen 7 9700X (8 cores, 16 threads), 64 GB RAM, NVIDIA GeForce RTX 4090 24 GB
  (driver 615.71.09), Arch Linux, Linux 7.2. A workstation in use: another Bloom was resident and
  other build lanes ran between measurements.
- **Media:** the bench media (`exr_multilayer` ZIP, its `CryptoObject` type of four ranks in two
  rank layers and its `ViewLayer.Depth.Z` channel; a PNG16 plate and its 16-bit mist pass).

## Node cost

`run_bench.py nodes` binary, `--repeats 5`, two runs per build, means of the per-run medians
(ms/frame; the ratio is against the Grade over the same EXR):

| Case | 1080p before | 1080p after | 2160p before | 2160p after |
|---|---|---|---|---|
| EXR Grade (reference) | 3.92 | 3.90 | 11.50 | 11.47 |
| Cryptomatte Image output, 16 IDs | 11.65 (2.97x) | 11.65 (2.98x) | 75.25 (6.55x) | 76.96 (6.71x) |
| Cryptomatte, 1 ID, as a layer mask | 11.83 (3.02x) | 11.83 (3.03x) | 78.66 (6.84x) | 79.97 (6.97x) |
| Cryptomatte, 16 IDs, as a layer mask | 11.64 (2.97x) | 11.64 (2.98x) | 78.95 (6.87x) | 80.70 (7.03x) |
| Cryptomatte, 64 IDs, as a layer mask | 11.65 (2.97x) | 11.63 (2.98x) | 78.79 (6.85x) | 80.34 (7.00x) |
| Cryptomatte, 256 IDs, as a layer mask | 12.16 (3.10x) | 12.15 (3.11x) | 80.65 (7.02x) | 82.28 (7.17x) |
| Cryptomatte, 16 IDs, on a Grade's mask input | 13.51 (3.44x) | 13.03 (3.34x) | 61.82 (5.38x) | 61.39 (5.35x) |
| Z Defocus, 32 px | 21.26 (9.60x) | 21.26 (9.54x) | 67.35 (10.74x) | 67.37 (10.73x) |
| Z Defocus, 128 px | 29.44 (13.29x) | 29.42 (13.21x) | 87.22 (13.90x) | 86.69 (13.80x) |
| Depth Fog, exponential | 3.12 (1.41x) | 3.12 (1.40x) | 9.47 (1.51x) | 9.48 (1.51x) |

The node bench times warm reruns of a prepared graph, whose decode is cached, so it does not see
this change; the other Z Defocus and Depth Fog cases moved by under 1% either way. The 2160p
Cryptomatte layer-mask cases sit at the 7.0x node budget in both builds: one before run put the
256-ID case at 7.05x, and the after runs at 7.03x (one case) and 7.10 to 7.32x (four cases). The
second after run matched the before runs (75.5 to 80.7 ms), so the spread is run to run, on the
budget's edge, not a cost of this change.

## Scenarios

`run_scenarios.py run --scenario <name> --export-frames 4` (24 frames, defaults, cold page cache),
three alternating runs per build:

| `cryptomatte-mattes-2k` | Before | After |
|---|---|---|
| First frame (s) | 3.84, 2.97, 3.02 | 2.82, 2.75, 2.68 |
| Fill fps | 7.35, 7.86, 7.11 | 11.78, 13.39, 12.69 |
| GPU busy during the fill, mean / peak (%) | 25.3 / 100, 37.6 / 70, 46.0 / 100 | 38.6 / 92, 23.6 / 56, 40.0 / 91 |
| Fill host claim over peak use | 3.09, 3.16, 2.94 | 1.70, 1.90, 1.68 |
| Fill host claim / peak anonymous (GB) | 8.72 / 2.82, 8.72 / 2.76, 8.72 / 2.97 | 5.35 / 3.15, 6.52 / 3.44, 6.01 / 3.58 |
| Cold playback (fps shown) | 16.77, 17.44, 17.57 | 20.62, 20.72, 20.97 |
| Scrub median (s) | 0.76, 0.79, 0.81 | 0.52, 0.48, 0.50 |
| Export s/frame (GPU frames of 4) | 0.66 (4), 0.66 (4), 0.66 (4) | 0.61 (4), 0.58 (4), 0.59 (4) |

| `depth-effects-2k` | Before | After |
|---|---|---|
| First frame (s) | 2.98, 2.51, 2.52 | 2.39, 2.39, 2.38 |
| Fill fps | 14.02, 14.38, 13.33 | 15.86, 19.34, 19.63 |
| GPU busy during the fill, mean / peak (%) | 15.4 / 55, 21.4 / 57, 27.8 / 54 | 58.2 / 100, 31.2 / 72, 35.0 / 100 |
| Fill host claim over peak use | 3.19, 3.36, 3.48 | 1.61, 1.66, 1.53 |
| Fill host claim / peak anonymous (GB) | 5.92 / 1.85, 5.92 / 1.76, 5.92 / 1.70 | 3.46 / 2.15, 3.20 / 1.93, 3.17 / 2.07 |
| Cold playback (fps shown) | 20.76, 21.25, 20.76 | 21.97, 22.06, 22.14 |
| Scrub median (s) | 0.54, 0.52, 0.53 | 0.31, 0.28, 0.23 |
| Export s/frame (GPU frames of 4) | 0.53 (4), 0.54 (4), 0.50 (4) | 0.51 (4), 0.51 (4), 0.51 (4) |

Every run exported its 4 frames on the GPU and passed `export-cpu-parity`.

`claims-within-factor` failed in five of the six before runs and in no after run. Before, a phase
with a channel read claimed the frame's preflight live-image peak, the same 8.72 GB (Cryptomatte)
and 5.92 GB (depth) in every run, against 1.7 to 3.0 GB of peak use. After, a Shuffle (the depth
shot's `ViewLayer.Depth.Z`) claims one RGBA32F frame of its source's extent, and a Cryptomatte type
the Cryptomatte default of three rank layers, once however many mattes read it; the fill's host
claim fell to 1.5 to 1.9 times use. The depth shot's `claims-within-factor` known-failure marker is
removed.

`gpu-busy-during-fill` still failed in every Cryptomatte after run (38.6, 23.6 and 40.0%; 50% is
the bar), so its marker stays at #549. The fill rose from 7 to 8 fps to 12 to 13 fps and its CPU
time fell by about a third (below), but the 24 frames now fill in about 2 s, ten 200 ms GPU
samples, and each frame's host work is still the one ZIP decode of the multilayer EXR (beauty and
rank channels together: OpenEXR inflates every channel of a chunk) plus the PNG16 plate. The depth
shot's GPU busy (58.2, 31.2 and 35.0%; #386) passed in one after run and none before; its marker
stays at #386.

One before run and one after run of `cryptomatte-mattes-2k` failed `no-cpu-fallback`: a "GPU
preview resident CPU fallback" task appeared during the export measurement, with no CPU frame shown
and no fallback reason counted, as in the first #549 change's before runs. It is intermittent in
both builds and is not a change of this work.

## Playback

`bloom_ui_gpu_packed_viewer_test --bench-project` over each scenario's project, offscreen, with
`--require-device`: a fill, then an 8 s cold playback. Three runs per build:

| | Before | After |
|---|---|---|
| Cryptomatte fill fps | 8.81, 9.14, 10.60 | 15.98, 16.05, 15.46 |
| Cryptomatte fill CPU time (s) | 26.0, 24.6, 24.7 | 15.4, 15.1, 16.9 |
| Cryptomatte cold play: first motion / all 24 cached (s) | 0.91 / 2.52, 1.04 / 2.43, 0.74 / 2.44 | 0.55 / 1.31, 0.55 / 1.37, 0.54 / 1.37 |
| Depth fill fps | 16.73, 16.72, 17.87 | 24.39, 25.33, 24.47 |
| Depth fill CPU time (s) | 16.4, 17.0, 15.3 | 10.0, 8.9, 10.1 |
| Depth cold play: first motion / all 24 cached (s) | 0.55 / 1.43, 0.55 / 1.40, 0.57 / 1.46 | 0.42 / 0.97, 0.41 / 0.98, 0.43 / 1.06 |

The fill's CPU time fell by 37 to 41% on both shots: the decode a frame used to repeat for its
channels.
