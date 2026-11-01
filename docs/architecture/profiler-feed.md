# Profiler Telemetry Feed

Status: working. Owner: `src/runtime` (the contract), `src/ui` (the feed and the Profiler panel).
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
the bucket index). The runtime sampler that fills it belongs to the runtime: one low-priority
sampler thread, refcounted and running only while something subscribes, 2 Hz by default (1–10 Hz),
publishing through a seqlock or triple buffer with no per-sample allocation, and never mutating
budgets, caches or scheduling. Its cheap-read rules are part of the contract: RSS from `statm` or the
platform equivalent at the sampler rate; anonymous, file-backed and shared memory and helper
processes from the ledger's existing five-second process sample, never `smaps` at the sampler
rate.

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

`FakeProfilerFeed` (`profiler_fake_feed.hpp`) is a deterministic source for tests and development:
sample n is a pure function of n, with growth, plateaus, eviction drops, absent fields and stale
ages. The application uses it only when `BLOOM_PROFILER_FAKE_FEED` is set; it is not offered in the
interface. Without it the application has no source yet and every metric reads absent; wiring the
runtime sampler replaces `makeDevelopmentProfilerFeed()` in the composition root.

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
