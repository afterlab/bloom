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
them absent rather than zero.

## Sampler

`bloom::runtime::ProfilerSampler` (`src/runtime/include/bloom/runtime/profiler_sampler.hpp`) fills
the snapshot.

- **One low-priority thread, refcounted.** The first `acquire()` starts it and it samples at once;
  the last `release()` stops and joins it. No thread runs without a subscriber. The thread lowers
  its own priority (Linux nice 10, macOS utility QoS, Windows lowest).
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
| Machine, headroom, usable, ledger totals, pressure flags, the aggregate pool row | the memory budget ledger, through `readProfilerLedger()` (below) |
| Unattributed, idle baseline | computed from what is present; the baseline is the unattributed gap at the first sample that has the pool holding |
| GPU live/retiring bytes, refusals, reclaims | the render module's process-wide `GpuImageReclamationStats`, read-only, through an injected function pointer (absent in a build without a GPU device) |
| Process CPU time | `getrusage` (Linux, macOS), `GetProcessTimes` (Windows) |
| Tasks and executor pools | `TaskScheduler::copyProfilerTasks()`: counts and the top 16 tasks copied into the snapshot's fixed slots under the scheduler lock, names truncated, no allocation or formatting under the lock; worker counts and queue depths per executor |
| Playback fps, dropped frames, RAM Preview cached frames and fill rate, export fps | the UI-side hook (below) |

Absent until the engine's counter work lands: file-backed and shared memory and the ledger sample's
age (the ledger's process sample carries cgroup figures, not a memory breakdown), per-pool rows and
their counters and ceilings (the ledger exposes only the aggregate holding), staging and admission
claims, the driver heap figures, upload and readback bytes, submits, utilization, executor busy
time, stage histograms, pressure counters and helper processes (their process IDs are not
reachable outside the media provider).

**The ledger adapter.** `readProfilerLedger()` (`profiler_ledger_adapter.hpp`) is the one place
that reads the ledger, through existing const accessors only: `state()`, `reusableHolding()`,
`ramHeadroom()`, `usableByteBudget()` and `physicalByteBudget()`. `state()` copies the whole
process sample (strings and vectors), so the hook refreshes it only after the ledger applied a
sample or a pressure sample; `reusableHolding()` asks every registered pool and follows the
ledger's thread rule, so it runs on the UI thread.

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
frames while caching, and export fps the rate of completed frames of the running job. No Qt object
is read off the UI thread.

`FakeProfilerFeed` (`profiler_fake_feed.hpp`) is a deterministic source for tests and captures:
sample n is a pure function of n, with growth, plateaus, eviction drops, absent fields and stale
ages. The application does not use it.

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
