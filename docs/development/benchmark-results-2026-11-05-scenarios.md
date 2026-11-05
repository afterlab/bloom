# Scenario suite results: RTX 4090 profile, main 8626d3918 (#540)

Status: record, 2026-11-05 (first stored run of the suite: 3 scenarios, every step ran)

This page records the first stored run of the real-world scenario suite
([benchmarks](benchmarks.md#scenario-suite)) on the RTX 4090 workstation. The raw results JSON
stays in the runner's work directory, outside git. It is the first baseline: later runs compare
against it with `--baseline`.

- **Build:** main `8626d3918` plus the schema 5 profiler telemetry the suite reads (`2e9d376b8`,
  telemetry only), RelWithDebInfo, clang.
- **Machine:** AMD Ryzen 7 9700X (16 logical cores), 64 GB RAM, NVIDIA GeForce RTX 4090 24 GB
  (driver 615.71.09), Arch Linux, Linux 7.2.
- **Machine state:** a workstation in use, not a dedicated host: another Bloom session and other
  GPU applications were resident (about 3 GB of device memory), and a second build lane compiled
  and ran tests during the run. `gpuBusyPercent` is machine-wide.
- **Media:** the synthetic benchmark footage (`verified-exact`), 1080p and 2160p tiers.
- **Limits:** the defaults (claim factor 3, GPU busy 50%, export 4 frames, export fill factor 4
  plus 0.5 s, claims at most 60 s above 90% of usable capacity, stall 120 s, regression threshold
  10%), except `--reference-frames 2` (frames 0 and 3 compared). The checks were evaluated again
  from the stored measurements with the committed check set.

## Measurements

| | `two-layer-glow-2k` | `cg-comp-2k` | `cg-comp-4k` |
|---|---|---|---|
| Size, frames, nodes | 1920×1080, 48, 7 | 1920×1080, 24, 32 | 3840×2160, 12, 32 |
| First frame (s, from `project.open`): fill / playback / scrub | 2.53 / 2.58 / 2.45 | 4.97 / 5.08 / 4.94 | 47.6 / 62.4 / 98.9 |
| Fill | 48 of 48 in 4.40 s (10.90 fps) | 24 of 24 in 55.4 s (0.433 fps) | stalled: 1 of 12 in 120 s |
| Capacity waits during the fill | 42 | 538 | 349 |
| GPU busy during the fill, mean / peak (%) | 38.8 / 69 | 38.7 / 100 | 28.2 / 100 |
| Cached playback (fps shown) | 23.94 | 23.96 | none cached |
| Cold playback (fps shown) | 12.32 | 0.0 | 0.0 |
| Scrub median / max (s) | 0.194 / 0.225 | 2.06 / 11.43 | 22.8 / 38.5 (2 of 6 targets not shown within 60 s) |
| Export: frames, s/frame | 4, 0.833 | 4, 1.455 | 4, 141.2 (564.8 s) |
| Export: GPU / CPU reference frames | 4 / 0 | 4 / 0 | 3 / 1 (frame 2) |
| Export: GPU busy mean (%), CPU cores busy | 12.7, 1.55 | 35.0, 1.62 | 25.9, 1.19 |
| Peak device claims / Bloom's own device memory / usable (GB), fill | 17.4 / 17.3 / 19.0 | 18.2 / 18.1 / 18.5 | 18.1 / 18.1 / 18.8 |
| Peak host claims / anonymous memory (GB), fill | 2.74 / 1.40 | 4.33 / 2.58 | 10.4 / 6.87 |
| CPU reference render (s/frame, `bloom-cli`, no driver) | 26.7 | 79.9 | 557 |

The Viewer route (first frames, fill, cached and cold playback, scrub) showed only
`gpuPreparedPacked` frames and counted zero CPU fallbacks in every scenario. No CPU-side profile
of the export was taken.

## Checks

`known-failure` is a check the scenario lists with the open issue that makes it fail; it is
reported and does not fail the run. No check failed outside those lists.

| Check | `two-layer-glow-2k` | `cg-comp-2k` | `cg-comp-4k` |
|---|---|---|---|
| `no-cpu-fallback` | pass | pass | known-failure, #544 (1 export CPU reference frame) |
| `claims-within-factor` | pass (device 1.0, host at most 1.95) | pass (device 1.01, host at most 1.68) | pass (device 1.02, host at most 1.70) |
| `viewer-and-fill-progress` | pass | pass | known-failure, #544 ("2 scrub target(s) not shown within 60 s; the fill cached no frame for 120 s (1 of 12 cached)") |
| `claims-below-capacity` | pass (0.9 s above 90%) | pass (1.6 s) | known-failure, #544 ("device claims stayed above 90% of usable capacity for 264.02 s", in the export) |
| `fill-completes` | pass | pass | known-failure, #544 (stalled) |
| `gpu-busy-during-fill` | known-failure, #544 (38.8%) | known-failure, #544 (38.7%) | known-failure, #544 (28.2%) |
| `fill-contiguous-from-playhead` | pass (at most 15 frames cached ahead of the run) | known-failure, #544 (the longest run started at frames 0, 2, 3, 6, 7 and 14 during the fill) | pass (one frame cached) |
| `ram-preview-usable-memory` | pass | pass | pass |
| `export-gpu-complete` | pass | pass | known-failure, #544 (3 GPU and 1 CPU reference frames of 4) |
| `export-rate` | pass (0.833 s/frame, limit 0.867 s) | pass (1.455 s/frame, limit 9.73 s) | known-failure, #544 ("export 141.2 s/frame, and the fill did not complete, so there is no preview rate it keeps up with") |
| `export-cpu-parity` | known-failure, #545 (frame 0: 6 RGB components above 2e-6, max 4.53e-6, 2189 alpha samples differ; frame 3: 1963 alpha samples differ) | known-failure, #545 (18518 and 18857 alpha samples differ; RGB within 2e-6) | known-failure, #545 (49467 and 50496 alpha samples differ; RGB max 2.38e-6 absolute, within the relative bound) |

Listed checks that passed in this run (for example `export-gpu-complete` on the two 2K shots) are
reported as passing with a note; the lists stay while the issues are open because the same checks
failed on longer work areas.

## Findings

- **The 4K comp's export takes minutes per frame (#544).** Four frames took 564.8 s (141.2 s per
  frame) with the GPU 25.9% busy on average and 1.19 CPU cores busy; the third frame was a CPU
  reference frame (the run log's `export.device.cpu-reference` warning), and the device claims
  stayed above 90% of the usable capacity for 264 s of the export.
- **The 4K comp's fill and Viewer wait for device capacity (#544).** The first Viewer frame took
  48 to 99 s, the fill cached one of 12 frames before it stalled for 120 s with 349 capacity waits
  and the device claims at about 18.1 GB of 18.8 GB usable, and two scrub targets were not shown
  within 60 s. The 2K comp filled out of order with 538 capacity waits and showed 0 fps in cold
  playback.
- **GPU export frames differ from the CPU reference in alpha (#545).** Every compared frame had
  thousands of alpha samples off (the gate requires exact alpha); RGB stayed within the gate
  except six components of the two-layer shot's frame 0.
- **The GPU stays under half busy during fills** in every scenario (28 to 39%), the capacity waits'
  symptom (#544).
- **Claims follow use.** Device claims stayed at Bloom's own device memory (ratio at most 1.02) and
  host claims within 2 times anonymous memory, inside the stated factor of 3. The RAM preview's
  ceiling is the usable memory (58.3 GB), with no fixed cap.

## After #544

The same suite on the same machine and media after the #544 change (retention gives way inside a
refused device allocation, a mid-frame device refusal waits instead of rendering on the CPU, the
final render's idle trim, and purge returning device memory;
[GPU backend](../architecture/gpu-backend.md), "Retention gives way inside the allocation").

- **Build:** main `8e8071b2c` plus the #544 change, RelWithDebInfo, clang.
- **Machine state:** another Bloom session and other GPU applications were resident (about 1.8 GB
  of device memory); `gpuBusyPercent` is machine-wide.
- **Limits:** the defaults, `--reference-frames 1` (frame 0 compared).

| | `two-layer-glow-2k` | `cg-comp-2k` | `cg-comp-4k` |
|---|---|---|---|
| First frame (s): fill / playback / scrub | 2.93 / 2.37 / 2.44 | 5.83 / 4.87 / 4.87 | 5.88 / 6.06 / 5.87 |
| Fill | 48 of 48 in 3.09 s (15.5 fps) | 24 of 24 in 5.36 s (4.48 fps) | 12 of 12 in 9.95 s (1.21 fps) |
| Capacity waits during the fill | 0 | 0 | 5 |
| GPU busy during the fill, mean / peak (%) | 44.4 / 100 | 69.8 / 100 | 75.2 / 100 |
| Cached playback (fps shown) | 23.95 | 23.96 | 23.97 |
| Cold playback (fps shown) | 21.47 | 1.99 | 0.0 |
| Scrub median / max (s) | 0.270 / 0.299 | 0.761 / 1.81 | 3.55 / 3.65 (all 6 targets shown) |
| Export: frames, s/frame | 4, 0.799 | 4, 1.685 | 4, 3.154 (12.6 s) |
| Export: GPU / CPU reference frames | 4 / 0 | 4 / 0 | 4 / 0 |
| Export: GPU busy mean (%), CPU cores busy | 14.4, 1.59 | 19.0, 1.50 | 25.4, 2.16 |
| Peak device claims / Bloom's own device memory / usable (GB), fill | 14.6 / 14.5 / 17.9 | 15.2 / 15.2 / 17.9 | 17.1 / 16.8 / 18.2 |
| Peak host claims / anonymous memory (GB), fill | 2.90 / 1.36 | 4.12 / 2.61 | 9.60 / 6.37 |
| CPU reference render (s/frame, `bloom-cli`, no driver) | 27.1 | 76.7 | 530 |

Every route counted zero CPU fallbacks. Checks that pass now and were listed for #544 have been
removed from the lists. Still listed:

| Check | `two-layer-glow-2k` | `cg-comp-2k` | `cg-comp-4k` |
|---|---|---|---|
| `gpu-busy-during-fill` | known-failure, #544 (44.4% over a 3.1 s fill) | pass (69.8%) | pass (75.2%) |
| `fill-contiguous-from-playhead` | pass | pass | known-failure, #544 (the run started at frames 0, 2 and 3; at most 5 frames ahead) |
| `export-rate` | known-failure, #544 (0.799 s/frame, limit 0.757 s) | known-failure, #544 (1.685 s/frame, limit 1.39 s) | pass (3.154 s/frame, limit 3.81 s) |
| `export-cpu-parity` | known-failure, #545 (2189 alpha samples differ; 6 RGB components above 2e-6) | known-failure, #545 (18518 alpha samples differ) | known-failure, #545 (49467 alpha samples differ) |

The 2K exports now fail `export-rate` only because their fills became fast: four frames carry the
pipeline's first-frame latency (a 4K frame waits about 1.9 s in scene preparation, 0.74 s on the GPU
and 1.3 s in the EXR write and reopen verification, which run one frame at a time with the
one-thread codec), and a 12-frame 4K export ran at 2.37 s per frame against 3.21 s for four.

## Earlier evidence on longer work areas

Before the work areas were shortened and the export capped, a run over 96-frame work areas held
the GPU for hours and was stopped. On the same build and machine it measured:

- `two-layer-glow-2k`: the export rendered 84 of 96 frames on the CPU reference path, from frame
  11, at 6.5 s per frame, with about 18 GB of device memory held.
- `cg-comp-2k`: a fill of 0.154 fps over 623 s with 7255 capacity waits, and an export of 18.5 s
  per frame with 81 of 96 CPU reference frames.
- `cg-comp-4k`: no frame cached after more than 10 minutes with the device claims at 14 to 16.5 GB
  of 17.3 GB usable, and an export of about 2.4 minutes per frame (27 of 48 frames in about 58
  minutes) with the GPU about 3% busy.

These are the reasons the export checks stay listed as known failures on the 2K shots.
