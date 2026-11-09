# Profiler Telemetry Feed

Status: working. Owner: `src/runtime` (the contract and the sampler), `src/ui` (the feed, the
UI-side hook and the Profiler panel).
Issue #423. The user-facing panel is described in [The Profiler panel](../user-guide/profiler.md).

## Contract

`bloom::runtime::ProfilerSnapshot`
(`src/runtime/include/bloom/runtime/profiler_snapshot.hpp`) is the one runtime telemetry contract.
The Profiler panel, the benchmark harness (BENCH_MEMORY) and the agent surfaces (MCP/CLI
`profiler.snapshot`) all read it, so a number means the same thing everywhere.

- It is Qt-free, fixed-capacity and trivially copyable (a `static_assert` holds this): no
  `std::string`, `std::vector` or pointer members, fixed `char[48]` names, and fixed arrays for up
  to 32 pools, 8 executor pools, 16 tasks, 6 stage histograms and 8 helper processes. A publisher
  copies it whole; a reader never allocates to receive it.
- It carries `schemaVersion`, a `serial` that increases with every sample, the steady-clock
  timestamp and the sampling interval.
- **Absent is not zero.** Every value a platform or an unwired counter cannot supply is a
  `ProfilerMetric<T>` with `present == false`. Values from slower sources carry that source's age
  (`ledgerSampleAgeNs`, `gpuBudgetQueryAgeNs`).
- Counters are cumulative; readers derive rates from two snapshots.
- Unattributed memory is `anon - (pools + pending + staging + claims)`
  (`profilerUnattributedBytes()`), signed because attribution can overcount, and read against the
  idle baseline (`profilerUnattributedOverBaseline()`). An absent staging or claim counter
  contributes nothing; absent anonymous memory makes the result absent.
- Stage histograms use fixed log2 buckets with edges from 16 µs to 16 µs × 2^18 (about 4.19 s): one
  bucket below the first edge, one above the last.

The header is data only, apart from small inline helpers (the unattributed math, name copying,
the bucket index). Schema 2 made pool counters (entries, hits, misses, stores, evictions,
refusals) and executor busy time `ProfilerMetric`s, so a source that does not count them reports
them absent rather than zero. Schema 3 added each pool's cumulative evicted bytes
(`ProfilerPool::evictedBytes`). Schema 4 added this process's memory in swap
(`ProfilerMemory::processSwappedBytes`). Schema 5 (#540) added the Viewer (`ProfilerViewer`: the
displayed frame's freshness, display provider and frame index, the playhead's frame index, and the
GPU preview route's cumulative CPU fallbacks by reason, admission refusals, completions and
capacity waits), the RAM Preview's state, range and longest contiguous cached run
(`ramPreviewCaching`, `ramPreviewTotalFrames`, `ramPreviewSpanFirstFrame`,
`ramPreviewSpanFrames`), and the device admission claims and usable capacity
(`ProfilerGpu::admissionClaimBytes`, `usableBytes`), so an agent reading `profiler.snapshot` can
check a shot for CPU fallbacks, fill order and claims without the benchmark harness.

## Sampler

`bloom::runtime::ProfilerSampler` (`src/runtime/include/bloom/runtime/profiler_sampler.hpp`) fills
the snapshot.

- **One low-priority thread, refcounted.** The first `acquire()` starts it and it samples at once;
  the last `release()` stops and joins it. No thread runs without a subscriber. The thread lowers
  its own priority (Linux nice 10, the lowest priority of its policy on macOS, Windows lowest).
- **Rate.** 2 Hz by default, clamped to 1–10 Hz; the feed passes its interval on activation.
- **No allocation after start.** The sampler fills one preallocated snapshot and publishes it
  through `ProfilerSeqlock` (`profiler_seqlock.hpp`), a single-writer, many-reader seqlock over
  relaxed atomic words, so a reader racing the writer retries instead of racing. OS reads use fixed
  buffers. Tested over 10,000 samples with a counted `operator new`; one sample costs well under a
  millisecond on Linux (tested).
- **Read only.** It never mutates budgets, caches or scheduling and never triggers ledger process
  sampling (`memoryBudgetSample()` or anything that walks `/proc`).

Sources, each read at the sampler rate unless noted:

| Snapshot part | Source |
| --- | --- |
| RSS | Linux `/proc/self/statm` (O(1), never `smaps`), macOS `task_info`, Windows `GetProcessMemoryInfo` |
| Anonymous | Linux statm `resident - shared` (the kernel's anonymous resident pages); absent elsewhere |
| File-backed, shared, swapped | Linux `/proc/self/status` `RssFile`, `RssShmem` and `VmSwap` (the kernel's per-process counters, O(1), fixed buffer, never `smaps`); absent on macOS and Windows, whose standard calls do not split resident memory this way |
| Machine, headroom, usable, ledger totals, pressure flags | the memory budget ledger, through `readProfilerLedger()` (below) |
| Ledger sample age | the time the ledger last applied a machine sample (`MemoryBudgetState::machineAppliedAt`: a process sample, a pressure sample or a poll); it ages available memory, swap, the pressure flags and the ledger totals |
| Pool rows | `MemoryBudgetLedger::copyProfilerPools()`, through the adapter: one row per registered pool (below) |
| Staging live and peak | the process-wide telemetry counters (below); the peak restarts when the sampler starts |
| Admission claims | the UI-side hook: `GpuProcessingPhasePolicy::usableCapacity()` host claims minus the retained preview frames they include |
| Unattributed, idle baseline | computed from what is present; the baseline is the unattributed gap at the first sample that has the pool rows while Bloom is idle (`profilerSampleIdle()`: no queued or running task, and the hook reports no playback, RAM Preview fill or export); absent until then |
| GPU live/retiring bytes, refusals, reclaims | the render module's process-wide `GpuImageReclamationStats`, read-only, through an injected function pointer (`runtime::readProfilerGpuDevice`; absent in a build without a GPU device) |
| GPU upload and readback bytes, submits | the telemetry counters, reported only in a process whose GPU reader found a device |
| GPU heap budget and usage | the telemetry counters: the preview display service's owner thread queries `VK_EXT_memory_budget` (summed over device-local heaps) at most once a second while a sampler runs, and once when a snapshot taken with no sampler running asks for it (`requestGpuHeapBudget()`; the figure arrives with the next snapshot); `gpuBudgetQueryAgeNs` is its age; absent on a device without the extension or before the first query |
| GPU utilization | absent on every platform: Vulkan has no portable query and Bloom links no vendor library (NVML, ADLX, Level Zero); Linux sysfs busy counters exist only for some kernel drivers (amdgpu `gpu_busy_percent`), not for the proprietary NVIDIA driver, and would need a Vulkan-device-to-DRM-node mapping |
| Process CPU time | `getrusage` (Linux, macOS), `GetProcessTimes` (Windows) |
| Tasks and executor pools | `TaskScheduler::copyProfilerTasks()`: counts and the top 16 tasks copied into the snapshot's fixed slots under the scheduler lock, names truncated, no allocation or formatting under the lock; per executor pool (CPU, Blocking I/O, GPU submit, and Row bands when the scheduler has a row-band pool) its workers, queue depth and busy time |
| Stage histograms | the telemetry counters (below) |
| Helper processes | the telemetry counters' helper registry; each listed pid is read through Linux `/proc/<pid>/statm` and `/proc/<pid>/stat` (fixed buffers), macOS `proc_pidinfo(PROC_PIDTASKINFO)`, Windows `GetProcessMemoryInfo`, `GetProcessTimes` and `GetExitCodeProcess`. A process that is gone or a zombie is listed as not alive with absent figures |
| Playback fps, dropped frames, RAM Preview cached frames and fill rate, export fps | the UI-side hook (below) |
| RAM Preview caching state, range frames and longest contiguous run | the UI-side hook: `RamPreviewController::isCaching()`, `totalFrameCount()` and `longestRetainedRun()` |
| Viewer frame (freshness, provider, frame index) and playhead frame | the UI-side hook: `CompositionPreviewController::state()` (the displayed frame's provenance provider and its request time) and the session's current time, as nearest frame indices on the composition grid |
| GPU preview route counters (CPU fallbacks by reason, admission refusals, completions, capacity waits) | the UI-side hook: `GpuPreviewDisplayService::status().counters`; absent without the service |
| Device admission claims and usable capacity | the UI-side hook: `GpuProcessingPhasePolicy::usableCapacity()` (`deviceClaimBytes`, `deviceUsableBytes` once the device is ready) |
| Admission waits and refusals | the telemetry counters: `GpuProcessingPhasePolicy::reserve()` counts each `AwaitingCapacity` answer (a waiting request retries, so each retry that waits again counts) and each `BudgetRefused` answer |
| Evicted bytes by cause | the telemetry counters (below, "Eviction causes") |

Still absent: GPU utilization (above).

**Eviction causes.** `core::telemetry::EvictionCause` matches `ProfilerEvictionCause`: Ceiling,
MemoryPressure, Admission, Purge. `MemoryPoolCounters::evicted()` adds the bytes to the pool's row
and to the thread's current cause; a pool evicts synchronously inside the ledger's budget callbacks,
so every ledger entry point that runs them opens an `EvictionCauseScope` (a thread-local cause,
restored on exit) naming why:

- Ceiling: `registerCache`, `unregisterCache`, `setCacheCeiling`, `setConfiguredTotal` and
  `setRamHeadroom` (a changed allowance or preference). Outside every scope a pool's eviction is
  also Ceiling: a pool making room within its own budget on a store.
- MemoryPressure: `poll`, `applySample`, `applyPressureSample` and `beginProcessSampling` (budgets
  from a measured memory sample).
- Admission: `reclaim` (admission's retention reclaimer for a waiting request), `beginPreviewFill`
  and `yieldToPreview` (room for a RAM Preview fill).
- Purge: the user purge's clears count their host bytes directly
  (`OperationCache::clearOperations` and `clearDecodedMedia` the cleared entries' standalone
  charge, `DecodedVideoCache::clear` its planes, `CompositionPreviewController::purgePreviewCache`
  the preview frames' host bytes). A purge is not a pool eviction: the pool rows' eviction counters
  do not include it.

`MemoryPoolCounters::released()` counts an eviction in the pool's row only, outside every cause:
decoded media and decoded video released because the source working set moved on, and media disk
cache entries removed from disk (bytes on disk, not memory). The causes therefore sum to the host
evictions, not to the pools' evicted bytes.

**Pool identity and counters.** A pool registers its Profiler identity once, after
`registerCache`, with `MemoryBudgetLedger::setPoolProfile(owner, MemoryPoolProfile)`
(`memory_pool_counters.hpp`): a fixed name, a kind (scratch, cache or live) and a
`MemoryPoolCounters`, six relaxed atomics (hits, misses, stores, evictions, evicted bytes,
refusals) the pool increments next to the work it already does, and an `entries` callback that
returns the entries it holds now (under the ledger's callback rules). A pool that is several rows
supplies a `rows` callback instead, which fills the entry counts itself. The ledger reads counters without the pool's lock; the host,
pending and device bytes come from the pool's existing holding callback, the ceiling from its
registration. A registered pool without a profile is one "Unnamed pool" row with absent counters.

| Row (kind) | Entries | Hit / miss | Store | Eviction (bytes) | Refusal |
| --- | --- | --- | --- | --- | --- |
| Preview frames (cache) | frames held (host or GPU-resident) | `PreviewFrameCache::take` | an inserted frame | budget, pressure and GPU-resident evictions (the frame's charge) | a rejected or unallocatable frame |
| Operation cache: operations, Operation cache: decoded media (cache) | retained entries of the kind | `OperationCache::find`: a hit under the found entry's kind, a miss under the kind the caller asked for | an inserted entry | LRU eviction and, for decoded media, release outside the playback window (the entry's standalone charge) | over the budget, or allocation failure |
| Decoded video (cache) | frames held | `DecodedVideoCache::find` | `store` | budget eviction and release outside the playback window (plane bytes) | a frame larger than the budget |
| Proxies (cache) | thumbnails held | the thumbnail lookups of the asset preview worker | a kept thumbnail | trimming (thumbnail bytes) | a thumbnail larger than the allowance |
| Decoded audio (cache) | buffers held | reusing a cached buffer | a kept buffer | trimming (buffer bytes) | a buffer sent for delivery only |
| Media disk cache write queue (scratch) | queued writes | disk-cache lookups | disk writes | disk entries evicted (bytes on disk) | writes the queue refused |

**Telemetry counters.** `bloom::core::telemetry` (`src/core/include/bloom/core/telemetry_counters.hpp`)
holds the process-wide always-on counters, in the core module because render, media, output and
runtime all count into it and only the sampler reads it. Each update is a relaxed atomic add on a
path that already does the work (two for a stage: its bucket and its summed duration; the staging
peak is a compare-and-swap only when a new peak is reached), plus the steady-clock reads that time
a stage. Nothing there allocates, locks or formats.

- **Host upload staging** (scratch, ADR 0037): the converted pixels a scene's upload command holds
  (`gpu_media_preparation.cpp`, including what `GpuSceneBuildUploads` shares inside one build),
  counted at allocation and subtracted by their deleter, and the host-visible Vulkan upload
  staging buffers (`createUploadStagingBuffer` / `UploadStagingBuffer::release`).
- **GPU transfers:** upload bytes at the image upload's submission (its staging size), readback
  bytes at each readback submission (process, display, resident and output readbacks), and every
  `vkQueueSubmit`.
- **Stage histograms**, log2 buckets from 16 µs: decode (the video worker's frame round trip and
  `media::decodeImage`), colour convert (decoded video to the working-space float image), upload
  and GPU render (the GPU scene executor's submission to observed fence, on its existing timing),
  transfer (the preview's packed display transfer and the final render's readback, on their
  existing timing) and encode (handing a frame to the encoder worker). A failed or cancelled
  attempt records nothing.
- **Helper processes:** a fixed registry of eight slots. The media worker pool registers each
  worker process it launches (one-call and retained) for its lifetime, and the colour workers
  (the OCIO CPU file-transform worker and the GPU program extraction worker) theirs.
  `MediaWorkerPool::workerProcessIds()` is the pool's read-only view of its own entries.
- **Admission and eviction causes:** relaxed adds where admission answers a reservation and where
  a pool evicts or a purge clears (above).
- **Profiler active:** set while a sampler thread runs; the GPU heap-budget query runs only then, or once when a snapshot with no sampler asks for it.

**The ledger adapter.** `readProfilerLedger()` (`profiler_ledger_adapter.hpp`) is the one place
that reads the ledger, through existing const accessors only: `state()`, `reusableHolding()`,
`ramHeadroom()`, `usableByteBudget()`, `physicalByteBudget()` and `copyProfilerPools()`.
`state()` copies the whole process sample (strings and vectors), so the hook refreshes it only
after the ledger applied a sample or a pressure sample; `copyProfilerPools()` asks every
registered pool what it holds and follows the ledger's thread rule, so it runs on the UI thread.
It fills fixed rows and never allocates.

## Feed

`bloom::ui::ProfilerFeed` (`src/ui/include/bloom/ui/profiler_feed.hpp`) is the Qt side. It delivers
`const runtime::ProfilerSnapshot&` to subscribers on the UI thread at the published rate.

- **Refcounted, visibility-driven.** A Profiler subscribes when it is shown and unsubscribes when it
  is hidden or destroyed. The first subscriber activates the source and starts the delivery timer;
  the last one stops both. With no Profiler on screen nothing samples and no timer runs.
- **One copy per sample.** On each tick the feed asks its source for the newest snapshot, copies it
  once into its own storage, and hands every subscriber the same reference. An unchanged serial
  delivers nothing. Subscriber storage is reserved up front; delivery never allocates (tested over
  10,000 deliveries).
- **No sampling on the UI thread.** A real source's `readLatest()` only copies the sampler's
  published buffer.

`RuntimeProfilerFeed` (`profiler_runtime_feed.hpp`) is the application's source. Activation
acquires the sampler and deactivation releases it; `readLatest()` copies the published snapshot.
It is also the UI-side hook: on each feed tick, on the UI thread, it reads the ledger through the
adapter and the playback, RAM Preview and export controllers, and publishes those values into the
sampler with `publishHostInputs()` (a second seqlock), which the next sample merges. Playback fps
is the rate of playhead advances while playing, the RAM Preview fill rate the rate of newly cached
frames while caching, and export fps the rate of completed frames of the running job. It also
publishes the Viewer's displayed frame and the GPU preview service's counters (schema 5), and
reports whether any of the three runs (`ProfilerHostInputs::hostBusy`), which holds back the idle
baseline. No Qt object is read off the UI thread.

`FakeProfilerFeed` (`profiler_fake_feed.hpp`) is a deterministic source for tests and captures:
sample n is a pure function of n, with growth, plateaus, eviction drops, absent fields and stale
ages. The application does not use it.

## One source

- **MCP/CLI.** `profiler.snapshot` (read-only, inline, no session lease) returns
  `{"succeeded": true, "snapshot": {...}}`, the snapshot serialized by
  `runtime::profilerSnapshotJson()` (`profiler_snapshot_json.hpp`): member names as in the header,
  enums as lowercase words, absent values as `null`, `schemaVersion` first. The desktop host fills
  it on the UI thread (the hook publishes the UI-owned values, then the sampler takes one sample
  unless its thread is running, in which case its newest sample is returned); the stdio host
  (`bloom-mcp`) samples its own process on request.
- **Benchmarks.** The harness's `BENCH_MEMORY` figures come from the same snapshot (see
  [Benchmarks](../development/benchmarks.md)), and `--bench-profiler` prints the window's last
  snapshot in the same JSON.

## Cost

The always-on cost is the counters above: relaxed atomic adds and the stage clock reads, measured
at a few nanoseconds each (`bloom.core.telemetry_counters` prints them). With no Profiler shown
nothing samples, no sampler thread exists and the GPU heap-budget query does not run (a snapshot requested by an agent asks for one query). One sample
costs a few microseconds on Linux, takes no lock while it copies or formats, and allocates nothing
(tested over 10,000 samples, with a helper process and staging bytes present).

## Panel cost rules

The Profiler (`bloom.profiler`, `src/ui/profiler_editor*.cpp`) must not add a problem of its own:

- **History** is `ProfilerHistory`: every graphed series is a 600-sample `kit::SampleRing`, all over
  one float block allocated when the panel is created and freed when it is destroyed. A push writes
  one float per series and derives rates from the previous snapshot; it never allocates (tested
  over 10,000 samples). Pressure events go to a fixed ring of 16.
- **Repaint** happens only on a new sample and only in open sections: a collapsed section is skipped
  and refreshed when it opens. Charts read their rings when they paint, readouts store numbers and
  format them when they paint, and tables ask for a cell only when the view paints it, so work
  scrolled out of view costs a store.
- **UI-thread work** is the snapshot copy, the history push and painting.
- **Read only.** Nothing in the feed or the panel changes scheduling, budgets or caches.

The chart and readout primitives are kit widgets (`KTimeSeriesChart`, `KHistogramChart`,
`KReadoutGrid`; see [UI grammar](../ux/ui-grammar.md#profiler-panel)).
