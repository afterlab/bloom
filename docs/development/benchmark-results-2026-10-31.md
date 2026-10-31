# Benchmark results: RTX 4090 profile, main 6757dacbf (#395)

Status: record, 2026-11-01 (partial: paused by the owner; the remaining cells continue on a second
bench host)

This page records the standard benchmark suite ([benchmarks](benchmarks.md)) on one hardware profile,
before and after:

- **Before:** part 1 on main 27c0949c (2026-10-30/31), the run behind #402.
- **After:** main 6757dacbf plus a bench-only commit that records the origin/main merge base. That
  commit changes no engine code.

The raw results JSON stays in the runner's work directory, outside git. The second hardware profile
(RTX 5080 / Core Ultra 9) will get its own page.

## Hardware profile and setup

| | |
|---|---|
| Profile | **RTX 4090 / Ryzen 7 9700X / 64 GB**: AMD Ryzen 7 9700X (16 logical cores), 64 GB RAM, NVIDIA GeForce RTX 4090 24 GB (driver 615.71.09), Linux 7.2 |
| Media | `<media-root>`; every asset `verified-exact` against `tools/bench/media_manifest.json` |
| Harness | `bloom_ui_gpu_packed_viewer_test --bench-project --require-device`, headless (`QT_QPA_PLATFORM=offscreen`, no `DISPLAY`/`WAYLAND_DISPLAY`), with the GPU lock held around every Bloom process |
| Memory | Before and after, every runner ran in `systemd-run --user --scope -p MemoryMax=20G -p MemorySwapMax=0`. Bloom sizes its caches from the cgroup limit, so the cap is part of the measured configuration |
| After codecs | FFmpeg 8.1.2 with the bundled codecs |
| Repeats | One run per cell, except the ProRes recheck (three runs). Fills cover 240 frames; playback is 6 s from a cold start |

```sh
python3 tools/bench/run_bench.py run --build-dir build/bench --media-root <media-root> \
    --work-dir <work-dir> --gpu-lock-file <lock> [--timeout 600] \
    --tier <tier> --format <format> --variant <A|B> --no-export
```

How to read the tables:

- Values are `fillFps` (prepared frames / fill seconds) or `playbackFps` (distinct frames shown /
  seconds). `a → b` means before → after; `–` means not measured.
- Video fills prepare 239 of 240 frames in both runs, so the last video frame is never prepared.
  These fills are not marked. Other incomplete fills show `(prepared/total)`.
- `stalled n/240`: the fill stopped making progress at the 20G cap and ended at the 600 s harness
  timeout (see [2160p memory](#2160p-memory)).
- GPU busy is the mean `nvidia-smi` utilisation over the Full fills and the playback. Peak RSS is
  the largest harness peak among them. CPU-fallback frames is the largest `cpuFallbackFrames` over
  all fills and playback. Playback can go above 240 because frames are evaluated again.
- **†** marks cells that ran while the owner's interactive Bloom session was resident (about 15 GB
  RSS at the start, 28 GB by the end). That left less host memory for Bloom's measured budget. The
  2160p png8/png16 Full fills were the hardest hit (host `MemAvailable` fell to 7 GB). Cells without
  † ran with no other Bloom session. The machine was otherwise shared: ComfyUI was idle, and other
  builds overlapped some after cells (up to 13 compiler processes during the 720p multilayer EXR
  cells, up to 9 during the 720p HEVC 10-bit cells, and up to 17 during the 2160p AV1 B and DNxHR
  HQX cells). Part 1 also ran on a shared machine with other builds active, and the
  record doesn't say whether a Bloom session was resident.

## Results

The after-run covers:

- the #402 formats at 720p and 1080p;
- the 1080p ProRes 422 HQ B recheck;
- at 2160p, the PNG and EXR sequences (except multilayer EXR B), DNxHR HQX A/B and AV1 B.

Part 1 did not reach 2160p beyond the EXR sequences, and VP9 did not import on 27c0949c (#398).

### 720p

| Format | Var | Fill Full cold | Fill Full warm | Fill Half cold | Fill Quarter warm | Playback | GPU busy % | Peak RSS GB | CPU-fallback frames |
|---|---|---|---|---|---|---|---|---|---|
| exr_multilayer † | A | 37.8 → 111.4 | 35.6 → 98.8 | 50.2 → 92.5 | 99.5 → 185.0 | 5.0 → 1.5 | 9.7 → 1.4 | 8.4 → 8.5 | 240 → 481 |
| exr_multilayer † | B | 35.2 → 61.9 | 36.0 → 69.0 | 45.0 → 95.2 | 110.6 → 132.4 | 0.2 → 0.2 | 4.5 → 7.3 | 12.6 → 12.7 | 240 → 388 |
| h264 † | A | 6.8 → 115.7 | 64.3 → 110.8 | 45.6 → 113.5 | 14.3 → 97.8 | 0.2 → 21.3 | 15.9 → 11.7 | 5.2 → 5.4 | 0 → 0 |
| h264 † | B | 65.2 → 50.4 | 31.5 → 68.7 | 22.2 → 41.9 | 25.2 → 45.4 | 0.2 → 18.5 | 21.0 → 15.9 | 5.4 → 5.5 | 0 → 0 |
| hevc10 † | A | 5.5 → 8.1 | 2.0 → 8.5 | 1.8 (1/240) → 8.2 | 1.8 → 6.9 | 0.2 → 8.2 | 6.7 → 5.4 | 6.6 → 6.8 | 0 → 0 |
| hevc10 † | B | 2.4 → 6.3 | 1.7 → 6.2 | 2.1 → 7.8 | 1.1 (1/240) → 10.0 | 0.2 → 10.8 | 9.4 → 12.2 | 6.8 → 6.9 | 0 → 0 |
| av1 † | A | 36.6 → 12.5 | 68.4 → 11.6 | 78.7 → 12.2 | 143.4 → 13.5 | 0.2 → 16.8 | 5.8 → 6.4 | 4.8 → 6.8 | 240 → 0 |
| av1 † | B | 41.4 → 16.1 | 34.5 → 13.8 | 61.2 → 17.3 | 71.1 → 12.5 | 0.2 → 9.2 | 13.6 → 13.5 | 8.5 → 6.9 | 240 → 0 |
| vp9 † | A | – → 56.3 | – → 52.6 | – → 55.6 | – → 57.1 | – → 22.7 | – → 8.8 | – → 5.4 | – → 0 |
| vp9 † | B | – → 86.1 | – → 86.4 | – → 66.7 | – → 71.0 | – → 20.7 | – → 20.3 | – → 5.5 | – → 0 |

### 1080p

| Format | Var | Fill Full cold | Fill Full warm | Fill Half cold | Fill Quarter warm | Playback | GPU busy % | Peak RSS GB | CPU-fallback frames |
|---|---|---|---|---|---|---|---|---|---|
| exr_multilayer † | A | 18.0 → 52.5 | 20.2 → 69.7 | 29.5 → 70.6 | 30.3 → 121.0 | 0.2 → 0.2 | 1.5 → 1.7 | 11.7 → 11.5 | 240 → 438 |
| exr_multilayer † | B | 15.6 → 41.8 | 17.2 → 46.0 | 48.7 → 60.8 | 56.6 → 134.9 | 0.2 → 0.2 | 1.3 → 0.5 | 14.1 → 14.2 | 240 → 286 |
| h264 † | A | 64.3 → 88.5 | 64.3 → 88.7 | 6.7 → 10.0 | 7.0 → 11.8 | 0.2 → 14.2 | 27.2 → 31.0 | 11.3 → 11.4 | 0 → 0 |
| h264 † | B | 30.0 → 39.5 | 20.1 → 39.4 | 38.8 → 47.9 | 16.6 → 50.5 | 0.2 → 14.0 | 32.2 → 53.2 | 11.8 → 11.7 | 1 → 0 |
| hevc10 † | A | 1.9 → 6.1 | 1.7 → 5.6 | 2.3 → 5.5 | 5.2 → 5.3 | 0.2 → 5.8 | 5.8 → 9.7 | 12.9 → 13.0 | 0 → 0 |
| hevc10 † | B | 3.7 → 37.0 | 1.7 → 36.9 | 2.9 → 5.3 | 3.8 → 5.4 | 0.2 → 4.8 | 11.8 → 45.5 | 13.1 → 13.2 | 0 → 0 |
| prores422hq † | B | 1.0 (1/240) → 36.8 | 15.2 → 36.8 | 18.2 → 39.3 | 17.5 → 39.8 | 0.2 → – | 18.5 → 56.6 | 13.1 → 13.2 | 0 → 0 |
| av1 † | A | 19.4 → 6.8 | 32.8 → 7.1 | 199.2 → 7.3 | 216.3 → 6.8 | 0.2 → 3.7 | 7.6 → 13.5 | 8.7 → 13.0 | 240 → 0 |
| av1 † | B | 18.0 → 20.1 | 17.6 → 7.3 | 44.8 → 7.0 | 68.2 → 6.7 | 0.2 → 3.8 | 2.4 → 26.1 | 14.0 → 13.3 | 240 → 0 |
| vp9 † | A | – → 35.1 | – → 32.4 | – → 25.9 | – → 19.7 | – → 22.5 | – → 19.9 | – → 11.4 | – → 0 |
| vp9 † | B | – → 40.2 | – → 40.3 | – → 49.6 | – → 51.3 | – → 13.3 | – → 52.0 | – → 11.7 | – → 0 |

### 2160p

| Format | Var | Fill Full cold | Fill Full warm | Fill Half cold | Fill Quarter warm | Playback | GPU busy % | Peak RSS GB | CPU-fallback frames |
|---|---|---|---|---|---|---|---|---|---|
| png8 † | A | – → 17.0 | – → 17.1 | – → 18.0 | – → 16.5 | – → 10.3 | – → 37.0 | – → 20.2 | – → 0 |
| png8 † | B | – → 8.0 | – → 8.3 | – → 12.3 | – → 12.9 | – → 6.0 | – → 56.4 | – → 20.1 | – → 0 |
| png16 † | A | – → 11.7 | – → 11.1 | – → 13.4 | – → 13.7 | – → 8.0 | – → 26.3 | – → 20.4 | – → 0 |
| png16 † | B | – → 5.4 | – → 5.4 | – → 10.6 | – → 10.9 | – → 5.2 | – → 39.6 | – → 20.1 | – → 0 |
| exr_half_piz † | A | 3.9 (231/240) → 12.8 | 3.9 (231/240) → 12.7 | 4.6 → 14.2 | 4.8 → 13.9 | 0.2 → 6.2 | 33.3 → 27.4 | 20.2 → 20.3 | 0 → 0 |
| exr_half_piz † | B | 1.8 (231/240) → 6.5 | 1.7 (231/240) → 6.4 | 3.1 → 11.8 | 4.6 → 11.0 | 0.2 → 5.0 | 26.2 → 39.7 | 20.0 → 20.2 | 0 → 0 |
| exr_float_zip † | A | 4.3 (231/240) → 8.8 | 3.9 (231/240) → 9.0 | 4.2 → 10.9 | 7.5 → 10.7 | 0.2 → 4.8 | 26.7 → 23.1 | 20.4 → 20.6 | 0 → 0 |
| exr_float_zip † | B | 2.7 (231/240) → 4.6 | 3.0 (231/240) → 4.6 | 2.2 → 8.7 | 3.1 → 9.1 | 0.2 → 3.5 | 22.0 → 33.3 | 20.2 → 20.4 | 0 → 0 |
| exr_multilayer † | A | – → 14.0 | – → 13.3 | – → 25.8 | – → 28.5 | – → 0.2 | – → 0.2 | – → 18.5 | – → 4583 |
| dnxhr_hqx | A | – → stalled 207/240 | – → stalled 204/240 | – → 10.5 | – → 11.6 | – → 6.2 | – → 20.6 | – → 21.5 | – → 0 |
| dnxhr_hqx | B | – → stalled 182/240 | – → stalled 182/240 | – → 7.2 | – → 8.0 | – → 2.7 | – → 26.2 | – → 20.7 | – → 0 |
| av1 | B | – → stalled 182/240 | – → stalled 183/240 | – → 1.8 | – → 1.7 | – → 0.8 | – → 20.8 | – → 20.7 | – → 0 |

The 1080p ProRes 422 HQ B recheck is in the 1080p table. Its Full cold fill completed 239/240 at
36.8 fps in three separate runs (36.83, 36.81, 36.83), with 0 CPU-fallback frames. That recheck
measured no playback.

### #402 rows

| #402 item | Status on 6757dacbf | Numbers (before → after) |
|---|---|---|
| Multilayer EXR fills on the CPU reference | **Remaining** | Still 240/240 CPU-fallback frames and 0 native dispatches at 720p, 1080p and 2160p A. Fills: `bloom.runtime.evaluation.invalid-parameter: No color layer — use Channels` (the suite projects select no colour layer). The CPU fills got faster (1080p A Full cold 18.0 → 52.5 fps). Playback is still 0.17 fps at 720p B, 1080p A/B and 2160p A; 720p A playback fell from 5.0 to 1.5 fps |
| AV1 fills on the CPU reference | **Fixed (GPU path), slower** | CPU fallback 240 → 0; fills run natively (1,434 dispatches for A and 4,878 for B, 239 packed transfers). They are now flat and slow: 720p 11.6–17.3 fps (one 56.2; was 34–143 on the CPU), 1080p 6.4–7.6 fps (one 20.1). Playback 0.17 → 3.7–16.8 fps. 2160p: A was killed by the 20G cap; B Full stalls at 182/240, Half/Quarter 1.7–2.1 fps |
| HEVC 10-bit fills crawl | **Remaining (improved)** | 720p 1.1–5.5 → 6.1–10.0 fps (one 58.9). 1080p A 1.7–5.2 → 5.3–6.1 fps, GPU 6–11% busy. 1080p B Full 3.7 → 37.0 fps, but Half/Quarter 5.3–5.6 fps |
| Long-GOP H.264 fills erratic; reduced resolution slower than Full | **720p fixed, 1080p remaining** | 720p A 2.3–64 → 93.5–115.7 fps across all six cells. 1080p A: Full 64.3 → 88.5 fps, but Half/Quarter 6.4–7.0 → 9.2–12.9 fps. Reduced resolution is still about 8× slower than Full. The same pattern is in 1080p HEVC 10-bit B (37.0 Full vs 5.3 Half) |
| 1080p ProRes 422 HQ B Full cold fill failed after 1/240 | **Fixed** | 1.0 fps (1/240, Failed) → 36.8 fps, 239/240, three runs, 0 CPU fallback |
| Uncached playback stalled at 0.17 fps (cfdf1c05) | **Fixed except multilayer EXR** | H.264 14.0–21.3, HEVC 10-bit 4.8–10.8, AV1 3.7–16.8, VP9 13.3–22.7 fps at 720p/1080p. 2160p: png8/png16 5.2–10.3, EXR 3.5–6.2, DNxHR 2.7–6.2, AV1 B 0.8 fps. Multilayer EXR is still 0.17 fps (see row 1) |

### 2160p memory

The harness peaks at 20.1–21.5 GB RSS in every 2160p cell, so every cell hits the 20G cap. Its own
budgets are much smaller: the RAM-preview cache budget is 7.6 GB, and the measured host capacity is
7.3–9.7 GB. The image sequences still complete: the cache fills to its 7.6 GB budget and the fill
ends. Video does not. Full fills of DNxHR HQX A/B and AV1 B stop at 182–207/240 frames against the
cap (the cgroup's `memory.events` recorded over 2,000 `max` hits) and run to the 600 s timeout. AV1 A
was killed by the kernel OOM killer inside the scope (harness anon RSS 20.2 GB) during its first
Full cold fill, so it produced no rows. Half and Quarter complete for all three. This run did not
test whether the stall is purely cap-induced, for example by running at 40G with no other Bloom
resident.

**Cause and fix (#411).** The harness's memory attribution (`memoryAtPeak`, `BENCH_MEMORY`) shows
that almost all of the memory was Bloom's own reusable caches, sized to the whole 20G limit. At the
DNxHR HQX A stall (205/240), RSS was 21.3 GB: the operation cache held 11.4 GB, the preview cache
6.8 GB, the other ledger pools 2.3 GB, and less than 1 GB was outside the pools. The harness had
copied the application's memory setup without the RAM/VRAM headroom, the active-work claims and the
evict-on-demand reclaimer. With no headroom, the process ledger divided the absolute limit between
the reusable pools in proportion to their automatic targets, and with no reclaimer a frame that
waited for host capacity waited for the full timeout. It now runs the application's wiring
(`connectApplicationMemoryBudget`). A second fault, which the application shared, held back every
processing claim from pool growth, including claims already allocated (retained packed preview
frames among them), so pools froze while a RAM preview filled; only unallocated claims are held back
now.

Re-measured inside the same 20G scope, Full cold, A variant. Before: commit 21a6455fa plus the
attribution commit. After: the fix, rebased on 9bffab0eb.

| Cell | Before | After |
|---|---|---|
| DNxHR HQX A | stalled 205/240, 0.3 fps, timed out at 600 s, peak RSS 21.5 GB | 239/240 prepared, finished, 9.2 fps, peak RSS 15.2 GB |
| png16 A | 239/240 prepared, 10.4 fps, peak RSS 20.3 GB | 239/240 prepared, 8.5 fps, peak RSS 15.8 GB |

Peak RSS now stays below usable capacity (the 20 GiB limit minus the 2.1 GB default headroom).
At the after peak the caches held about 14 GB: preview 3.4–3.6 GB, operation cache 6.5–10.4 GB
(prepared uploads 6.5–8.2 GB, decoded media up to 2.1 GB), next to 4 GB of in-flight work. The
reusable pools share the room and give back memory in proportion to what they hold, so by the end
of the fill only 82 (png16) and 101 (DNxHR) of the 240 frames are still cached, and both runs end
`PartialBudget`. Before, the preview cache had a fixed 7.6 GB share of the limit and kept 230 png16
frames, at the cost of running the process at the limit. Keeping all 240 frames of a 2160p fill
inside 20G needs about 8 GB of preview frames, so it depends on which retained bytes give way first:
the operation cache's prepared uploads and decoded sources, or the preview frames.

## Conditions

- **Host crash, 2026-10-31 16:14 local.** The first after-run queue ran one runner for the whole
  2160p tier, under the same 20G cap. The workstation stopped hard during that queue:
  - The last cell to start was 2160p multilayer EXR **B, Full cold fill**, at 15:59:41. (An earlier
    note said its playback; the A playback had just finished.) It produced no result. The external
    load average fell from about 10 to about 1.1 by 16:04 and stayed there. That fits a fill stalled
    at the memory cap, like the 2160p video stalls above, under the then-default 1800 s timeout.
  - The system journal stops at 16:14:09 with no shutdown. The load sampler kept writing until
    16:15:10, and its file ends in unwritten (NUL) blocks.
  - No OOM-killer, NVIDIA Xid/NVRM, machine-check, NVMe, thermal or watchdog messages were logged in
    that boot, and the next boot reports no machine-check record.
  - The cause is unknown. The memory cap held the runner, so a cgroup-charged runaway is unlikely.
- **Resume.** The resume ran one runner scope per format and variant, with a 600 s harness timeout.
  A watchdog sampled the GPU every 10 s (`temperature.gpu`, `memory.used`, `power.draw`) and host
  `MemAvailable` every 2 s. It would kill the scope above 90 °C or below 3 GiB available. The peak
  GPU temperature was 42 °C.
  - A first attempt at `MemoryMax=40G` was stopped. Next to the resident 18–28 GB Bloom session, it
    drove host `MemAvailable` below 3 GiB within seconds in each 2160p cell, so the watchdog killed
    four cells.
  - The resume therefore went back to 20G, the cap used by part 1 and by the first queue.
- **Paused** on 2026-11-01 at 00:48 local, by the owner's decision, during 2160p H.264 A. That cell
  was stopped about 2 minutes in and produced no rows.

## Not run: pending on the second bench host

These continue on a second bench host (RTX 5080 / Core Ultra 9) as a second hardware profile. Their
numbers will not be directly comparable with this profile.

- **2160p, not measured:** H.264 A/B (A was stopped by the pause), HEVC 10-bit A/B, ProRes 422 HQ
  A/B, ProRes 4444 A/B, VP9 A/B.
- **2160p multilayer EXR B: skipped as the crash suspect.** Its Full cold fill was running when the
  host crashed.
- **2160p AV1 A: killed by the 20G memory cap** (kernel OOM inside the scope); no rows.
- **720p and 1080p controls** (Full, cold/warm, playback): PNG 8/16, EXR half PIZ, EXR float ZIP,
  ProRes 422 HQ, ProRes 4444, DNxHR HQX, A and B. Part 1 has before numbers for all of them.
- **1080p exports** of the B projects of PNG16, EXR half, ProRes 422 HQ and H.264 to ProRes 422 HQ,
  H.264 and EXR.
- **Re-runs of every † cell** with no other Bloom session resident: the #402 rows, the ProRes
  recheck, 2160p EXR float ZIP A/B, EXR half PIZ A/B, multilayer EXR A, png8 A/B and png16 A/B.
- **Multilayer EXR with the `Composite.Combined` colour layer selected** (the suite projects select
  none), to separate a project-setup fallback from missing GPU coverage.
- **A 40G check of one stalled 2160p video cell**, to tell a cap-induced stall from an engine stall.
