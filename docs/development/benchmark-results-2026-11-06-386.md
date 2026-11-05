# Hardware use results: RTX 4090 profile, #386

Status: record, 2026-11-06 (paired before/after runs of the #386 changes)

This page records the measurements behind the #386 changes: blend and OCIO kernels in bounded
native batches with fence waits ([GPU backend](../architecture/gpu-backend.md), "Bounded Native
Command Batches"), image-sequence samples decoded ahead of the frame
([Media I/O](../architecture/media-io.md), "Image-sequence read-ahead"), hardware video decode, and
the playback investigation. Raw results stay in the runner's work directory, outside git.

- **Builds:** before, main `641721018`; after, the #386 branch on main `74075ab2e`. Both
  RelWithDebInfo, clang, built side by side and run alternately (before, after, after, before), so
  both see the same machine state.
- **Machine:** AMD Ryzen 7 9700X (8 cores, 16 threads), 64 GB RAM, NVIDIA GeForce RTX 4090 24 GB
  (driver 615.71.09), Arch Linux, Linux 7.2. A workstation in use: other GPU applications were
  resident and other build lanes ran between measurements. Runs of one cell vary by about 10%.
- **Harness:** `bloom_ui_gpu_packed_viewer_test --bench-project`, offscreen, warm page cache:
  RAM-preview fill at Full, and playback (`--bench-playback`, 8 s) from a cold Bloom process. The
  benchmark projects are the synthetic B variants ([benchmarks](benchmarks.md#benchmark-projects)).

## Harness runs

| Cell | Measurement | Before | After |
|---|---|---|---|
| `two-layer-glow-2k` (scenario project) | Fill fps | 19.1, 20.2 | 28.6, 29.0 |
| | Native submissions per fill (48 frames) | 1645 | 705 |
| 2160p PNG16 B | Fill fps | 21.4, 21.5 | 24.5, 24.7 |
| | Frames in flight (peak), frame device peak | 11, 0.88 GB | 13, 0.56 GB |
| | Playback fps | 20.9, 21.0 | 22.4, 22.5 |
| | First motion after Play (s) | 0.55, 0.55 | 0.44, 0.45 |
| | Preroll | 0.5 s cap | ready at 0.40 s |
| 2160p EXR half PIZ B | Playback fps | 23.0, 23.1 | 23.4, 23.4 |
| | First motion after Play (s) | 0.38, 0.39 | 0.25, 0.26 |
| 2160p H.264 B, NVDEC | Fill fps | 61.6, 64.5 | 51.3, 57.0, 62.7, 65.1 |
| | Playback fps | 21.2 | 22.4 |
| 2160p H.264 B, software (`BLOOM_VIDEO_HARDWARE_DECODE=0`) | Fill fps | 75.6 | 68.6 |
| | Playback fps | 23.2 | 23.1 |

Every run counted zero CPU-fallback frames.

- **Batching blend and OCIO kernels.** The Glow chain's 35 submissions a frame fall to about 15
  with the 128 MiB output bound (5 without it). Most of the Glow scene's gain is the OCIO program
  cache: with eight programs retained, the scene recompiled its effects every frame (0.9 ms of CPU
  per OCIO step on the owner thread); with the bound so tight that nothing batches, the same scene
  still filled at 28.6 fps.
- **The output bound.** Unbounded batches held every intermediate until the fence, which doubled
  a 2160p frame's device peak (0.88 → 1.96 GB) and cut the frames in flight from 11 to 8: the
  2160p PNG16 fill fell to 15.5 fps. With 128 MiB a 2160p frame's peak is 0.56 GB, below the
  earlier unbounded translation and source-over runs, and 13 frames fit. The bound costs a
  GPU-bound source: keeping translation and source-over runs unbounded (bounding only blend and
  OCIO) filled the H.264 cell at 68–77 fps but the PNG16 cell at 20–21. The bound favours frames in
  flight, which the CPU-bound sequence formats need; the H.264 fill stays within run-to-run noise of
  before.
- **Fence waits.** Disabling the preview owner's fence wait left every cell within noise: the
  owner was not the limit once batches and the program cache removed its per-step work.

## Playback investigation

- **Playback no longer trails the fill much.** On the 2160p PNG16 cell the warm fill runs at
  24.5 fps and playback at 22.4 (realtime ratio 0.93); the earlier gap (#413: cold playback at
  2–7 fps against fills of 11–13) came from cold reads and the per-frame CPU path, and the
  remaining gap is playback's eight-frame window: frames are prepared in parallel, so the frame the
  transport needs next lands one preparation latency after it is asked for, and a wider window made
  the nearest frames land later (#477).
- **First-batch latency at 2160p was the decode of the frames after the playhead.** Before Play,
  the Viewer's frame has queued captures of the next members; those frames then decoded one PNG16
  member each, on one thread, after Play (the first eight frames were cached 0.4–0.5 s after Play
  and the preroll ran to its 0.5 s cap). With the read-ahead decoding those members while the
  pipeline waits, the first four frames are cached by about 0.1 s, the preroll ends on its frames,
  and first motion comes 0.1 s earlier (0.3 s earlier in an idle session: 0.26 against 0.55).
- **Decoding every member ahead was slower.** With no core limit, the read-ahead's four decoding
  workers competed with the eight frames in flight on an eight-core CPU and steady 2160p PNG16
  playback dropped about 10% (decode wall time 56 → 82 s per run); the decodes ahead now run only
  on cores the frames leave, and decodes for frames the transport has passed are not run.
- **Hardware decode.** The NVDEC route works on this host (`bloom.media.video-session`: "hardware
  route on this host: nvdec"; provider `ffmpeg-nvdec`, first-frame software parity). The 2160p
  H.264 B cell is GPU-bound: playback is at real time with either route and the fill is not faster
  with NVDEC (the frames' scene work, not decode, is the limit; NVDEC frees CPU: 1.1 cores busy).

## Scenario suite

`run_scenarios.py run --scenario two-layer-glow-2k --scenario noise-glow-2k --scenario
cryptomatte-mattes-2k` on the after build, defaults, cold page cache:

| | `two-layer-glow-2k` | `noise-glow-2k` | `cryptomatte-mattes-2k` |
|---|---|---|---|
| Fill | 48 in 2.22 s (21.7 fps; 15.5 after #544) | 24 in 0.40 s (59 fps; 45 on 2026-11-05) | 24 in 8.09 s (2.97 fps; 2.8 on 2026-11-06) |
| GPU busy during the fill, mean / peak (%) | 64.3 / 100 (44.4 after #544) | 31.0 / 50 | 18.6 / 92 |
| Cold playback (fps shown) | 22.9 | 23.6 | 2.6 |
| Export s/frame (GPU frames of 4) | 0.54 (4; 0.80 after #544) | 0.47 (4) | 1.01 (4) |

Every other check passed on all three. `two-layer-glow-2k` passes `gpu-busy-during-fill`, so its
known-failure marker is removed. `noise-glow-2k` keeps its marker, now under #386: its fill is too
short to judge (two or three 200 ms GPU samples). `cryptomatte-mattes-2k` keeps its marker under
#386; its low figure is the per-frame Cryptomatte rank-image decode and upload (#549).
