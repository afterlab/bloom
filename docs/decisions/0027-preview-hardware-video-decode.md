# ADR 0027: Preview hardware video decode and decode capacity

Status: accepted

Date: 2026-10-23; amended 2026-11-04 (hardware decode on the final route, NVDEC, issue #495)

## Context

Issue #366 audited the Linux FFmpeg read path. FFmpeg was built without x86 SIMD and decoded on
one thread; the VA-API decode code was never selected and hard-coded one render node; every frame
step other than +1 re-seeked; a session refused concurrent callers with `Busy`; only four sessions
were retained; the decoded cache was a fixed 256 MiB; and each decoded frame was hashed seven
times between the worker and the color conversion.

## Decision

- FFmpeg's x86 SIMD is enabled through a source-built, build-only NASM (see
  [`dependency-intake.md`](../architecture/dependency-intake.md)).
- Software decode is threaded within a memory bound derived from the media-v1 worker address
  space; intra-only codecs use slice threads. It remains the reference path and keeps
  `DecodedSemanticExact`.
- A retained decoder continues forward inside the GOP and seeks by the decode time of the key
  packet presented at or before the target.
- VA-API decode is a separate preview-only worker generation with its own execution key. It
  discovers render nodes at run time, proves its first frame byte-identical to the software
  reference in every worker generation, and falls back explicitly and permanently per source on
  any refusal. Final rendering never uses it. Decoded-cache keys and GPU raw-upload identities
  include the route.
- Sessions queue callers; retained sessions are admitted by estimated worker memory and the
  source-snapshot file quota; the decoded cache target is configurable and otherwise usable memory
  (measured capacity minus the headroom preference, ADR 0037), a reusable pool on the process memory
  ledger evicted on demand and holding the playback window (amended 2026-11-04: the share of
  physical memory was replaced by ADR 0037 on 2026-11-01).
- Decoded planes cross the worker pipe length-checked, without digests (amended 2026-11-04: the
  digests cost a SHA-256 of every frame on both sides and guarded nothing a worker could not
  forge; see `frame-output.md`).

## Amendment (2026-11-04): hardware decode on the final route, and NVDEC

Owner decision, issue #495: loosen decision records that cost throughput, as long as colours and
pixels stay correct. H.264, HEVC and AV1 decoding is bit-exact by specification, and the per-source
proof already checks it, so keeping hardware out of final rendering bought no pixel guarantee.

- Final rendering and export (revalidated revisions: still, sequence, video and headless render)
  may use hardware decode for a source whose worker generation proved its first hardware frame
  byte-identical to the software reference. The proof, its zero tolerance and the explicit,
  permanent per-source software fallback are unchanged. A hardware generation is always a retained
  worker on the source's immutable snapshot, so it proves its first frame once and then continues
  forward, on the final route as in preview.
- Route identity stays in every cache key: decoded-cache keys, the selection cache key and the GPU
  raw-upload identity carry the route ("vaapi" or "nvdec").
- NVIDIA hosts decode through FFmpeg's CUDA hwaccel (NVDEC, `--nvdec-decode`), the standard FFmpeg
  route, with its own execution key (`nvdec-cuda`, device `cuda-device-discovered`) and provider id
  `ffmpeg-nvdec`; other Linux hosts keep VA-API. H.264, HEVC and AV1 are declared on both (AV1
  through FFmpeg's native AV1 decoder, which decodes through a hwaccel only; software AV1 stays
  dav1d). The FFmpeg intake enables `cuda`, `nvdec` and the h264/hevc/av1 NVDEC and AV1 VA-API
  hwaccels (ADR 0019/0035, `dependencies/lock-components/ffmpeg-8.1.2.json`); the driver is loaded at
  run time and the build needs no nonfree component. On the measurement host VA-API also worked on
  the NVIDIA GPU through nvidia-vaapi-driver, but NVDEC decoded 2160p HEVC Main 10 at 383 frames/s
  against 215 (and the VA-API walk reached the integrated GPU first, 67 frames/s).
- The automatic preference is NVDEC wherever it is the route (it measured faster than threaded
  software decode); VA-API keeps the four-thread rule. `BLOOM_VIDEO_HARDWARE_DECODE` accepts
  `vaapi` or `nvdec` to name a route, `1` to prefer the host's route, `0` to disable.
- macOS keeps VideoToolbox as its platform provider for every route; Windows has no hardware decode
  route yet (D3D11VA and NVDEC there cannot be verified on the measurement host) and decodes in
  software, reported as such.

## Consequences

- Hardware decode changes performance only, in preview and final rendering alike. Its parity
  tolerance is zero; a mismatch is a typed fallback, not a degraded frame.
- macOS (VideoToolbox) and Windows keep their own providers; the session API is platform-neutral
  and reports software provenance where no hardware route exists.
- The details and the measured before/after latencies live in
  [`media-io.md`](../architecture/media-io.md#video-read-implementation-media-3).
