# ADR 0040: The Disk Cache Mirrors Source Bytes

Status: accepted

Date: 2026-11-13

Issues: #718, #732

## Context

Footage often lives on a network share or another slow volume. Bloom read it from there again and
again:

- The disk cache stored decoded Float32 frames keyed by the asset digest, the interpretation and
  the color setup. Computing that key needs the file's digest, so a cold lookup read the whole
  source from the network before it could even ask the cache. A change of interpretation or color
  setup missed every entry and read the network again.
- A decoded frame is many times the size of its file (a 4K frame is about 127 MiB), so the byte
  budget covered few frames and the write queue held large buffers.
- Only the still-image evaluation path and the thumbnails used the store. Export workers, headless
  and scripted renders and GPU preparation read the source directly.
- Edits that did not touch the assets still restarted the thumbnail job, which probed and
  sometimes read every file.

## Decision

1. **The disk cache is a write-through mirror of encoded source bytes.** An entry is the file
   exactly as it was read, keyed by the source path and guarded by the file's identity (size,
   modification time and file id). A lookup costs one metadata query. The payload digest is
   verified on every read and is the same digest a direct read computes, so no cache key, project
   field or relink rule changes with the route.
2. **Storage classes decide by default.** `platform::storageClass()` classifies the volume that
   holds a file as Fast local, Slow attached, Network or Unknown, on Linux, macOS and Windows.
   Under the Automatic policy everything but Fast local gets a local copy. The Preferences can
   switch the policy to Always or Never and add folder rules; an asset's `localCopies`
   interpretation overrides both.
3. **One choke point, every route.** `media::captureImageSourceRevision()` consults the mirror
   before it reads and feeds it afterwards. The application, `bloom-cli`, `bloom-mcp` and headless
   export attach the same per-user store; the application publishes its settings to a small file
   beside the cache root so the Qt-free hosts follow the same policy.
4. **The first read is never slower.** A miss returns the bytes it read at once and shares them
   with one idle-priority writer. The queue is bounded in bytes and drops its oldest write when
   full; a dropped write costs one more read later, never a wait.
5. **Decoded frames are not stored on disk.** Memory holds decoded frames (the operation cache and
   the preview frame cache); disk holds sources. The retired store's files are removed at startup.
6. **Source revisions are retained during gestures.** The encoded bytes of a file do not depend
   on a parameter override, so an interactive evaluation keeps the source revision it read in the
   operation cache. Decoded frames keep their gesture rule: read-only reuse, no insertion. This
   amends the CACHE-1 rule recorded in `media-io.md`, which kept gesture misses out of the cache
   entirely.
7. **Panels evaluate through the shared cache.** The anchor grid and the driven-value readouts
   use the application's evaluator, so they read a source the Viewer holds from memory. The
   anchor grid resolves bounds when the playhead has stood still, not on every playback frame.
8. **The thumbnail job runs when its inputs change.** It reruns only when the asset set, an
   interpretation, the color setup or the frame rate changes, or after a footage verification
   pass. A run checks its in-memory thumbnail first and probes files by metadata.
9. **Video is mirrored by byte range and never copied first.** A movie the mirror keeps is
   opened as a progressive snapshot: the sandboxed worker gets its one read-only handle to an
   unnamed local file at once and asks the host for the ranges a decode touches. Each range
   comes from the mirror's sparse chunk file when it is there and from the source otherwise, and
   what the source gave is kept. The rest of the file fills at idle priority. The snapshot
   carries the digest the asset records until the fill completes and the whole local file is
   hashed; a request that needs verified sources waits for that, an interactive frame does not.
   The worker still never receives a path or a writable handle.
10. **One store per process.** The host that opens the mirror names it the process's source
    store, and a new operation cache starts with it. An evaluator made for one measurement or
    one export therefore reads through the mirror without each call site being wired; a test or
    a host that wants none, or another, sets it on the cache. This is one typed process-wide
    default beside the memory ledger and the verified-source memo, not a general locator.
11. **The playhead frame's decode stays in memory.** The native samples a frame decodes for
    itself are kept as decoded media under the key the read-ahead uses, for stills and sequence
    members alike, within the operation cache's budget and the playback window.

## Consequences

- A network source is read once. Playback, scrubbing, RAM Preview, exports, thumbnails and a
  restart all read the local copy, whatever the interpretation or color setup.
- A movie on a share plays from its first frame while it arrives, and a later session or a
  restart decodes from the local chunks without reading the share.
- Until a movie's fill completes its frames are shown on the strength of the recorded digest and
  the file's identity. A file whose bytes do not hash to that digest is refused when the fill
  completes, and before any export or final render uses it.
- A movie is written twice on the cache volume while a session holds it: the chunk file, which
  persists, and the worker's unnamed snapshot, which is released with the session.
- A warm frame still pays its decode. Decoding from a local solid-state copy is far cheaper than a
  network read, and the decoded frame stays in memory while it is in use.
- The byte budget covers many more frames, since entries are file-sized.
- A file that is replaced in place with the same size, modification time and file id is not
  noticed by the mirror; footage verification hashes the source itself and remains the check for
  that case.
- A file removed outside Bloom is noticed by the next evaluation that needs it or the next
  verification pass, not by an unrelated edit.
- `localCopies` joins the asset interpretation in the project format as an optional member; a
  build that predates it treats it under the format's rules for unknown members.

## Amendment (2026-11-16, #732): a disk headroom instead of a byte budget

The first version gave the mirror a byte budget fixed at startup (a tenth of the cache volume's
free space, at most 32 GiB, not counting the mirror's own bytes) and evicted the least recently
used entry. Two things followed on real footage. A playback loop larger than the budget got no
disk hits after its first pass, because the least recently used entry was always the next frame
the loop needed. And the write queue was registered with the memory ledger as reusable cache, so
a RAM Preview fill shrank it to nothing and most writes of the fill were dropped.

This amends decision 4 and the consequence about the byte budget:

1. **A headroom, not a budget.** The user sets the space to keep free on the cache volume
   (default: the smaller of a tenth of the volume and 32 GiB; never under 1 GiB free). The mirror
   may hold what it stores plus the free space beyond the headroom, measured when it writes. Its
   own bytes count as usable. This is the model the RAM and VRAM headroom already use. The byte
   budget setting is removed and an existing value is dropped.
2. **Space is given back.** When other programs bring the free space under the headroom, the
   mirror evicts what is missing, least recently used first.
3. **Making room keeps what is in use.** A write that needs room is refused and counted, and
   evicts nothing, when the entry it would evict was used within ten minutes, or was used since
   the same source last found no place. A loop larger than the space therefore keeps a stable set
   of disk hits for any pass length, and new work still replaces entries nothing has used for
   ten minutes. The alternative, a clock window alone, fails when one pass takes longer than the
   window; protecting entries by project would need the mirror to know about projects.
4. **The write queue is live work.** Its 256 MiB ceiling is fixed; the memory budgets neither
   resize it nor drop what is waiting. A full queue drops its oldest write during playback, where
   a frame has a deadline, and makes the read wait during a RAM Preview fill, where it has none.
   Decision 4's "never a wait" now holds for every read with a deadline; a fill trades a short
   wait for leaving everything it read on disk.
5. **A copy on its way to the disk already counts.** A lookup is answered from a write that is
   still queued or being written, and one reader at a time reads a source: a second reader of the
   same source waits for the first and takes its bytes. The writer runs at idle priority and
   falls behind while the machine renders, so without this a source was read again by every
   evaluation that asked for it before its write was done, and could be written twice. The
   alternative, raising the writer's priority, would put the copy in the way of the frames.

Consequences: the disk cache follows the free space of the machine instead of a figure chosen at
startup, so on a mostly empty drive it holds far more than before and on a nearly full one it
holds less and gives space back. A source that does not fit is read from its storage on every
pass and shows up as a refused write in the status bar instead of silently evicting its
neighbors. The status bar and Preferences show used and available space and the counts.
