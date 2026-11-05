# Hardware use results: RTX 4090 profile, #549

Status: record, 2026-11-06 (paired before/after runs of the #549 changes)

This page records the measurements behind #549: the Cryptomatte node uploads each rank layer of its
type once per source frame as one frame-sized data image, shared by every matte of the type, and
runs one `CryptomatteCoverageV1` pass per layer ([evaluation primitives](../architecture/evaluation-primitives.md),
"Cryptomatte"). Raw results stay in the runners' work directories, outside git.

- **Builds:** before, main `b77f07a4e`; after, the #549 change on it. Both RelWithDebInfo, clang,
  run alternately (before, after, before, after) on the same machine state.
- **Machine:** AMD Ryzen 7 9700X (8 cores, 16 threads), 64 GB RAM, NVIDIA GeForce RTX 4090 24 GB
  (driver 615.71.09), Arch Linux, Linux 7.2. A workstation in use: another Bloom was resident and
  another build lane ran between measurements.
- **Media:** the bench multilayer EXR (`exr_multilayer`, its `CryptoObject` type: four ranks in two
  rank layers, Float32).

## Node cost

`run_bench.py nodes` binary, `--tiers 1080p,2160p --repeats 5`, two runs per build, means of the
per-run medians (ms/frame; the ratio is against the Grade over the same EXR):

| Case | 1080p before | 1080p after | 2160p before | 2160p after |
|---|---|---|---|---|
| EXR Grade (reference) | 3.92 | 3.92 | 13.58 | 11.95 |
| Cryptomatte Image output, 16 IDs | 36.40 (9.30x) | 11.66 (2.98x) | did not run | 70.79 (5.92x) |
| Cryptomatte, 1 ID, as a layer mask | 37.11 (9.48x) | 11.83 (3.02x) | did not run | 74.94 (6.27x) |
| Cryptomatte, 16 IDs, as a layer mask | 36.87 (9.42x) | 11.65 (2.98x) | did not run | 74.91 (6.27x) |
| Cryptomatte, 64 IDs, as a layer mask | 36.78 (9.39x) | 11.67 (2.98x) | did not run | 74.85 (6.26x) |
| Cryptomatte, 256 IDs, as a layer mask | 36.87 (9.42x) | 11.66 (2.98x) | did not run | 75.94 (6.35x) |
| Cryptomatte, 16 IDs, on a Grade's mask input | 36.83 (9.41x) | 13.03 (3.33x) | did not run | 60.03 (5.02x) |

Before, every 2160p Cryptomatte case was refused: "the image plus its staging peak exceed the
requested byte budget" (the four stacked RGBA32F rank images, 530 MB, plus their staging against
the executor's 768 MB per-image ceiling on this machine). After, each upload is one 133 MB rank
layer and every case runs on the GPU. Against the Image output every route stays at 0.85 to 1.12x.

Where a frame goes (an instrumented build, not kept): before, the 1080p rank stack's 133 MB staging
buffer took 10 to 17 ms to allocate and 8.5 ms to fill on the device thread each frame. After, at
2160p, each 133 MB layer's staging is a new allocation (about 11 ms) plus a 9 ms copy, so the
265 MB of float rank data a 2160p frame uploads keeps that tier near 6 EXR Grades. The bench passes no
copy workers, so these copies are single-threaded; the application spreads them over its row bands.

## Scenario

`run_scenarios.py run --scenario cryptomatte-mattes-2k` (24 frames, defaults, cold page cache),
alternating:

| | Before (2 runs) | After (3 runs) |
|---|---|---|
| First frame (s) | 4.92, 4.43 | 3.12, 3.06, 3.04 |
| Fill fps | 2.91, 3.17 | 8.03, 8.11, 7.22 |
| GPU busy during the fill, mean / peak (%) | 9.9 / 82, 18.4 / 100 | 46.0 / 92, 35.0 / 71, 50.9 / 100 |
| Cold playback (fps shown) | 2.69, 2.63 | 15.96, 16.25, 16.19 |
| Scrub median (s) | 2.07, 2.12 | 0.80, 0.82, 0.86 |
| Export s/frame (GPU frames of 4) | 1.05 (4), 1.02 (4) | 0.68 (4), 0.67 (4), 0.67 (4) |

Both before runs failed `no-cpu-fallback` (a "GPU preview resident CPU fallback" task ran, with no
CPU frame shown); no after run did. The shot's three Cryptomatte selections now read one gather and
one upload per rank layer per frame instead of three gathers, three EXR decodes of the rank
channels and one 133 MB stacked upload.

`gpu-busy-during-fill` passed in one after run (50.9%) and failed in two (46.0 and 35.0%), so its
known-failure marker stays and now points at #549.
The fill is host-bound: in the node bench's preparation of this EXR, 70% of the CPU time is the
ZIP inflate of its decode, and a frame of the shot decodes the multilayer EXR twice, once for the
beauty and once more for the Cryptomatte rank channels (that second decode reads the file's rank
channels directly, outside the decoded-image cache). The after runs' 24-frame fill lasts about
3 s, ten to twelve 200 ms GPU samples.

Two of the three after runs failed `claims-within-factor`: the fill's host claim over its peak use
was 3.02 and 3.18 (limit 3; 2.85 in the third run, 2.49 and 2.51 before). The claims fell (14.7 GB
peak before, 8.7 GB after) but the use fell further (peak anonymous memory 5.9 GB before, 2.7 to
3.1 GB after), because a frame no longer holds three rank gathers and a stacked upload. Preflight
claims a phase with a channel read (a Cryptomatte or a Shuffle) at the frame's preflight live-image
peak instead of from its sources' extents; that estimate is unchanged by #549. This check is not
marked as a known failure.
