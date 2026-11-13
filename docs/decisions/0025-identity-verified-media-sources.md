# ADR 0025: Identity-Verified Media Sources

Status: accepted

Date: 2026-10-23; amended 2026-11-04 (digests leave the frame path, issue #495) and 2026-11-13
(verified identities persist, issue #718)

## Context

Bloom tied every media source to a SHA-256 content digest and treated only a fresh full hash as
proof that the original still held those bytes. `Revalidate`, the default and final-render source
policy, therefore re-read, re-hashed and (for Linux video) re-copied the whole source on every
asset refresh, every Assets freshness tick during playback, every exported frame and every 65,536
sample audio block. A long movie made ordinary editing, playback and export spend most of their
time reading the same file again (issue #356).

## Decision

- A full hash records the file identity observed before and after the pass: length, modification
  time, status-change time, and volume plus file id (POSIX `stat` device/inode on Linux and macOS;
  the handle's volume serial and 128-bit file id, `LastWriteTime` and `ChangeTime` on Windows). A
  pass whose bracketing observations differ records nothing.
- `platform::VerifiedSourceDigests` is a bounded process-wide memo of (absolute path, identity) to
  digest. While a later identity observation is identical, the recorded digest stands for the
  bytes. Any difference — touch, rewrite, rename-over, another volume, a missing file — forces a
  full rehash, so changed content still reports `SourceChanged` and the relink diagnostic.
- `Revalidate` now means "observe the original's identity, rehash only when it changed". It
  remains the default, and final preparation still forces it and still rejects missing or changed
  media before memoized pixels are accepted. `Retained` keeps its meaning: use the last verified
  revision without touching the original.
- Verified encoded revisions stay the decode authority. Linux video still decodes only an
  anonymous immutable snapshot; a live snapshot is shared by digest between sessions through a
  non-owning index, so it is never copied twice while any session retains it. Still-image
  revisions in the evaluator cache are reused under `Revalidate` after the same identity check.
- Every whole-file source read for hashing or snapshot capture increments process-wide work
  counters (`platform::sourceReadWork()`), which tests use to prove zero full-file reads during
  playback, ordinary edits and per-frame export once a source is verified.

## Consequences

- One full read per source per process at import or first use; afterwards one `stat` (or one
  handle query on Windows) per verification.
- The memo is runtime state and is never saved; reopening a project in a new process hashes each
  source once again.
- Residual risk, accepted: content changed in place while length, modification time and
  status-change time all stay identical is not detected until the identity changes. On POSIX
  status-change time cannot be set by ordinary user calls; on Windows `ChangeTime` can be set
  explicitly, and coarse-timestamp or network filesystems may hide a same-size rewrite within one
  timestamp tick. Explicit refresh/relink and the immutable decode snapshot bound the damage: a
  decode never mixes old and new bytes.
- The behavior is identical on Linux, macOS and Windows; only the identity query differs.

## Amendment (2026-11-04): digests leave the frame path

Owner decision, issue #495: loosen decision records that cost throughput, as long as colours and
pixels stay correct. The full SHA-256 was still on the first-use path of every source and, because
each image-sequence member is a source used once during playback, on the per-frame path: a cold
2160p multilayer EXR shot (95 MB a frame) hashed every frame on the read-ahead workers before it
could decode.

- The file identity authorises preview and export from first use. A capture reads the file into its
  immutable revision and observes the identity (length, modification and status-change time, volume
  and file id) before and after the read. When it held, the bytes are admitted without a hash: under
  the process memo's digest when the memo verified this identity, under the stamp for a stamped
  member that still carries it, else under the recorded digest. The admitted identity is remembered
  in the memo without vouching for a digest (`VerifiedSourceDigests::admit`).
- A changed identity still forces a full hash: one that changed during the read, a stamp that
  differs, or a memo that knows the path under another identity. Changed content then reports
  `SourceChanged`/`DigestMismatch` and the relink diagnostic as before; a memo digest that refutes
  the recorded digest does the same.
- Digests are computed in the background: a project task ("Verifying footage", idle OS priority,
  cancellable, with progress in Jobs) hashes members still pending a digest and verifies recorded
  digests. Its hashes enter the memo, so a refuted file is reported at its next use and the caches
  holding frames made from it are purged. A save records what the memo has already verified
  (a member may carry its stamp and its digest) and never waits; a member without a
  digest saves pending. See
  [`project-format.md`](../architecture/project-format.md#verified-member-digests).
- Decode still reads only the immutable revision (image) or snapshot (video), so old and new bytes
  never mix. Pixels are unchanged: a deferred and a digest-first capture decode identical pixels
  (`bloom.media.image` tests).
- Video sources keep their single capture pass on Linux, which copies the immutable decode snapshot
  and hashes the bytes in the same read (the copy, not the hash, dominates it); macOS and Windows
  verify a video's digest once per process at first use, as before.

Measured on 2026-11-04 (16 threads, RTX 4090; same base with and without the change, runs
interleaved): exporting 2160p and 1080p 16-bit PNG sequences to ProRes 422 HQ took 0.178 and
0.054 s a frame against 0.184 to 0.213 and 0.056 to 0.069. Cold playback of the 2160p multilayer
EXR shot stayed decode-bound at the same frame rate within noise (2.83 against 2.98 frames/s over
eight runs each, about 75 CPU-seconds in each six-second window): its hash cost about 50 ms of one
core per 95 MB frame, under 1% of the decode work. Its first motion came later, 2.35 s against
1.96 s, consistently across the runs. Measured again on 2026-11-04 after playback waits for frames
(#497), six interleaved runs each: with the image-sequence read-ahead's captures turned off, first
motion came at 1.01 s instead of 1.97 s, confirming that the captures, issued without a hash
between them, competed with the frames' own cold reads. Since frames' own reads go first (#477,
see media-io) it comes at 1.22 s, at the same cold frame rate (6.1 frames/s).

Residual risk, accepted with the decision: until the background pass reaches it, a file whose
content changed between sessions without a recorded identity (a project saved before 1.36, or a
still image) is decoded from its current bytes at first use; the pass then reports it, purges what
was rendered from it, and later uses show the relink diagnostic.

## Amendment (2026-11-13): verified identities persist

The memo lived for one run of the application, so every restart hashed each source again the
first time something needed it verified: a whole read of every file, which on a network share is
the cost issue #718 removes. The memo's verified entries now persist in a small per-user file
beside the disk cache (`verified-sources.v1`, path and identity to digest).

- A persisted entry stands for a file of the same size, modification time, status-change time
  and file id. The volume id is left out of that comparison: a network share is given a new one
  at every mount, and comparing it would empty the record at each restart. Within one run the
  full identity, volume included, is compared as before.
- Admitted (never hashed) identities do not persist: they vouch for no digest.
- A path observed under another identity, or forgotten, is removed from the record, so a changed
  file is hashed again exactly as in the original decision.
- The record is advisory in the same way the memo is: it can only skip a hash for a file whose
  identity did not change. A file rewritten in place with every one of those identity fields
  restored is the residual risk this decision already accepted, now across runs as well as
  within one.

## Related

- [`../architecture/media-io.md`](../architecture/media-io.md)
- [`../architecture/frame-output.md`](../architecture/frame-output.md)
