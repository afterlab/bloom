#pragma once

// A ledger pool's identity and cumulative counters for the Profiler (#423;
// docs/architecture/profiler-feed.md). A pool owns one MemoryPoolCounters per profiler row and
// increments it with relaxed atomic adds next to the work it already does (a lookup, a store, an
// eviction, a refusal). The ledger reads the counters without taking the pool's lock.

#include <bloom/core/telemetry_counters.hpp>
#include <bloom/runtime/profiler_snapshot.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace bloom::runtime {

struct MemoryPoolCounters final {
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<std::uint64_t> stores{0};
    std::atomic<std::uint64_t> evictions{0};
    std::atomic<std::uint64_t> evictedBytes{0};
    std::atomic<std::uint64_t> refusals{0};

    void hit() noexcept { hits.fetch_add(1, std::memory_order_relaxed); }
    void miss() noexcept { misses.fetch_add(1, std::memory_order_relaxed); }
    void store() noexcept { stores.fetch_add(1, std::memory_order_relaxed); }
    // An eviction of host memory: counted in the pool's row and, under the thread's current
    // eviction cause (core::telemetry::EvictionCauseScope), in the pressure section.
    void evicted(const std::uint64_t bytes) noexcept {
        released(bytes);
        core::telemetry::addEvictedBytes(bytes);
    }
    // An eviction counted in the pool's row only, outside the pressure section's causes: entries
    // released because the source working set moved on, or bytes that were on disk.
    void released(const std::uint64_t bytes) noexcept {
        evictions.fetch_add(1, std::memory_order_relaxed);
        evictedBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    void refused() noexcept { refusals.fetch_add(1, std::memory_order_relaxed); }

    // Copies the counters into a profiler row (present: this pool counts them).
    void copyTo(ProfilerPool& pool) const noexcept {
        pool.hits = ProfilerCount::of(hits.load(std::memory_order_relaxed));
        pool.misses = ProfilerCount::of(misses.load(std::memory_order_relaxed));
        pool.stores = ProfilerCount::of(stores.load(std::memory_order_relaxed));
        pool.evictions = ProfilerCount::of(evictions.load(std::memory_order_relaxed));
        pool.evictedBytes = ProfilerBytes::of(evictedBytes.load(std::memory_order_relaxed));
        pool.refusals = ProfilerCount::of(refusals.load(std::memory_order_relaxed));
    }
};

// How a registered pool appears in the Profiler: a fixed name (static storage), its kind, its
// counters and, when it counts them, its entries now. A pool that is several rows (the operation
// cache's Operation and DecodedMedia kinds) supplies `rows` instead: it fills up to `out.size()`
// rows with names, kinds, bytes, ceilings, entries and counters and returns how many it filled.
// `rows` and `entries` run under the ledger's callback rules (the thread that polls the ledger, no
// I/O, no waiting on workers, no ledger calls).
struct MemoryPoolProfile final {
    std::string_view name;
    ProfilerPoolKind kind = ProfilerPoolKind::Cache;
    const MemoryPoolCounters* counters = nullptr;
    std::function<std::size_t(std::span<ProfilerPool> out)> rows;
    // Entries the pool holds now; the row's entry count stays absent without it.
    std::function<std::size_t()> entries;
};

} // namespace bloom::runtime
