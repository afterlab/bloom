# The Profiler panel

The Profiler is a process and task monitor for Bloom itself. It shows, as it happens, how much
memory, GPU memory and CPU Bloom uses, what work is running, how long each pipeline stage takes,
when Bloom is under memory pressure, and how its helper processes are doing. Use it to explain a
problem: memory that grows and never comes back, a cache that keeps evicting, a playback that drops
frames, or an export that slows down.

Open it by choosing **Profiler** in any panel's editor picker (the gauge icon). It is an ordinary
panel: put it anywhere, open several, or replace it with another editor.

The Profiler only reads. It never changes caches, budgets or scheduling, and nothing it does enters
the undo history. It costs nothing while it is not on screen: Bloom samples only while at least one
Profiler is visible.

## Reading the panel

Each section has a graph of the last five minutes (600 samples at two a second, newest at the
right) and the current numbers beside it. A colored square next to a number is that line or band's
color in the graph above it.

- **Unavailable** means Bloom cannot measure that value on this computer or has not measured it yet.
  It is never the same as zero: a value that really is zero reads 0.
- **"3.0 s old"** after a value means it comes from a slower source than the graph, and says how
  old it is. It turns orange once the value is more than ten seconds old.
- A gap in a line means the value was unavailable at that time.

Click a section's title to collapse it; a collapsed section does no work at all.

## Memory

The graph stacks what Bloom's memory is made of, bottom to top: the **unattributed** gap (orange),
**staging** (yellow), **admission claims** (light grey), then one band per pool. The top of the
stack is Bloom's **anonymous** memory (the dashed line); **RSS** (the solid white line) adds file-
backed and shared memory on top. The dashed **Idle baseline** line marks how much unattributed
memory an idle Bloom with an empty project has.

- **Resident (RSS)**: the memory the operating system has given Bloom, read twice a second.
- **Anonymous**, **File-backed**, **Shared memory**: RSS broken down by kind, from Bloom's slower
  memory sample (every five seconds); their age is shown.
- **RSS growth**: how much RSS changed across the graph. Memory that keeps climbing and never comes
  back down while you work points to a leak.
- **Unattributed**: anonymous memory that no pool, staging or claim accounts for, and how far it
  sits **over the idle baseline**. A gap that keeps growing over the baseline is memory Bloom holds
  without knowing why.
- **Staging**: memory staged on its way to or from the GPU right now, and its peak.
- **Admission claims**: memory granted to running work that has not reached a pool yet.
- **Pools**: everything the pools hold, with the number of pools.
- **Ledger configured / effective / retained**: the reusable memory you allowed in Preferences,
  what the computer's limits leave of it, and what the caches hold now (see [Memory](memory.md)).
- **Headroom** and **Usable**: the RAM headroom preference and the capacity left for Bloom's work.
- **Physical memory**, **Available**, **Swap used**: the computer as a whole.
- **Memory pressure** and **Swap pressure**: whether the operating system reports pressure now.

The **Pools** table lists every pool: its kind (*Cache* keeps reusable results, *Live* holds what a
playback or export needs right now, *Scratch* holds memory only while work runs), the memory it
holds on the host, memory evicted but not yet released (**Pending**), memory on the GPU
(**Device**), its entries, its ceiling, and how many hits, misses, stores, evictions and refusals it
has counted.

## GPU

The first graph stacks Bloom's own **live** and **retiring** GPU memory (retiring memory waits for
the GPU work that still reads it) under the driver's figure for **heap use** (white line), with the
driver's **heap budget** dashed. The second graph shows **upload** and **readback** traffic.

**Bloom share of heap use** compares what Bloom allocated with what the driver reports in use, so
memory used by something else on the GPU is visible. **Utilization** is shown where the platform
reports it. **Queue submits**, **Admission refusals** and **Reclaims** count GPU work, requests the
GPU budget turned away, and memory taken back to make room.

## CPU

The first graph is Bloom's whole-process CPU use in cores (2.00 cores is two cores fully busy). The
second shows how busy each worker pool is, from 0 to 100%: the CPU pool, the blocking I/O pool
(disk and decode) and the GPU service. Each pool's row says how many workers it has and how many
tasks wait in its queue.

## Workload

- **Queued**, **Running**, **Succeeded**, **Cancelled**, **Failed**: how many tasks are in each
  state, and **By owner** and **By executor** the same tasks by who asked for them and where they
  run.
- The **Tasks** table lists up to 16 tasks, running first, with their owner, executor, state,
  progress (*Indeterminate* when a task cannot tell) and age.
- **Stage timings** show, for decode, color convert, upload, render, transfer and encode, how long
  each step took since this Profiler opened: one bar per time range from 16 µs to over 4 s, the
  bar holding the median in white, and the median (p50) and 95th percentile (p95) above it. A
  stage that nothing has timed reads Unavailable.
- **Throughput** graphs playback frames per second, RAM Preview fill rate and export frames per
  second. Each is Unavailable while nothing plays, fills or exports. **Dropped frames** counts the
  frames playback skipped, and **RAM Preview cached** the frames RAM Preview holds.

## Pressure

The graph shows admission **waits** (work held until memory frees up) and **refusals** (work that
could not fit at all) per second. Below it: whether the operating system reports memory or swap
pressure, the totals of waits and refusals, how much memory caches gave back for each reason (a
pool over its **ceiling**, memory **pressure**, to **admit** new work, or a **purge**), and the
current eviction rate.

**Events** lists the latest 16 changes, newest first: pressure starting or ending, refusals,
evictions and helper processes exiting.

## Processes

Bloom's helper processes (such as the media worker) with their process ID, resident memory, CPU
time and whether they are still running.

## Before the runtime sampler arrives

The Profiler is built against Bloom's runtime telemetry contract. Until Bloom's sampler is
connected, a Profiler shows "No profiler source is connected" and every value reads Unavailable.
