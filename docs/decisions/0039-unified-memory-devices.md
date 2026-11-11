# ADR 0039: Unified-Memory Devices Share One Pool

Status: accepted

Date: 2026-11-11

Issues: none filed; owner decision "One pool, Bloom budget"

## Context

Bloom plans GPU work against two capacities: host memory, measured from the system, and device
memory, taken from the driver's live budget (`VK_EXT_memory_budget`) or the device-local heap size.
That is correct for a GPU with its own memory. It is not correct for a device that has none:

- On Apple silicon the driver exposes one memory heap. It is device-local and host-visible, and it
  is the machine's memory. An integrated GPU on any platform has the same layout.
- What the driver reports as available for that heap does not shrink when the rest of the system
  fills the machine's memory. MoltenVK reports Metal's recommended working-set size as the budget
  and this process's own Metal allocations as the usage: 12.7 GB available on a 16 GB Apple M4.
- Bloom therefore held two budgets over one pool. Host work and device work could each be admitted
  against the same free bytes.
- The memory tests assumed a second, non-device-local heap: two cases reported `NotRun` (and the
  test still passed), and one allocation asked the room reclaimer once per memory type because the
  single heap offers several.

## Decision

A device is a **unified-memory device** when every one of its memory heaps is device-local. Bloom
reads this once at device creation and carries it on the allocation budget
(`GpuAllocationBudget::unified_memory`). A device with any heap that is not device-local keeps
exactly the planning it had.

For a unified-memory device:

1. **One pool.** The device's capacity is the smaller of the driver's available figure and the room
   left in the measured system pool: the host capacity less the host claims. Bloom's own budget
   decides; the driver's figure is only an upper bound.
2. **Charged once.** Memory the device has actually allocated is already missing from the measured
   free memory, and it is still counted in Bloom's device claims. It is credited back when the
   bound is computed, the same way the driver's figure is credited for it, so it is charged once.
3. **The pool is followed.** The device capacity is recomputed on every device report and whenever
   a new host memory sample arrives, so it tracks the pool without waiting for the next frame.
4. **The ceiling is the pool's.** The most the device could ever give is never more than the host
   ceiling.
5. **Both headrooms still apply.** The RAM headroom is taken from the pool and the VRAM headroom
   from the device capacity, so a unified device keeps at least the larger of the two free.
6. **No nonlocal domain.** There is no non-device-local memory to back host-visible buffers with.
   The cases that exercise that domain are not applicable on such a device and say so
   (`NotApplicable:`); they do not report `NotRun`.

Two smaller rules follow from a heap that offers several memory types:

- An allocation that Bloom's budget refuses for one memory type is not retried on another type of
  the same heap. Types that share a heap share its budget, so the answer is the same, and the room
  reclaimer is asked once.
- A test case that could not run on a device that should have run it ends the test with the skip
  exit code, or with a failure when a device is required. Printing `NotRun` and passing is not
  allowed.

## What This Does Not Do

Device bytes that a phase has reserved but not yet allocated are not charged against host
admission. The window is one device stage. The device side is bounded by the host claims at every
report, and an allocation that then finds no memory is a typed refusal, as before.

## Consequences

- A device with its own memory is planned exactly as before; the new rules read a flag that is
  false for it.
- On Apple silicon and on integrated GPUs the device capacity is smaller than the driver reports
  when the system is busy, and work waits or falls back by the existing rules instead of being
  admitted against memory that is not there.
- `bloom.runtime.gpu_processing_phase_policy` covers the bound, the claim from host work, the
  follow-up on a new sample and the driver's figure as the upper bound. The render memory tests
  pass on an Apple M4.

## References

- [GPU backend architecture](../architecture/gpu-backend.md)
- [ADR 0037: memory follows the working set](0037-memory-follows-the-working-set.md)
