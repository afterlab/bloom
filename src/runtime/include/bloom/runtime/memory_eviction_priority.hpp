#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bloom::runtime {

// Which reusable pools give memory back first (docs/architecture/evaluation-primitives.md,
// "Eviction priority during RAM Preview"). Outside a RAM Preview fill every reusable pool yields in
// proportion to what it holds, least recently used entries first within the pool. While a fill is
// active (MemoryBudgetLedger::beginPreviewFill), Ordinary pools -- operation-cache intermediates,
// prepared upload host copies, decoded media, queues -- give back what is asked of them before any
// FinishedPreview pool gives back a finished frame. Finished frames still yield after the Ordinary
// pools are empty, so work in flight is never starved by the frames it produced.
enum class MemoryEvictionPriority : std::uint8_t { Ordinary, FinishedPreview };

struct MemoryEvictionHolding final {
    MemoryEvictionPriority priority = MemoryEvictionPriority::Ordinary;
    std::size_t heldBytes = 0;
};

enum class MemoryEvictionRounding : std::uint8_t {
    // Each share rounds down: the pools together never give back more than asked (budget deficits).
    Down,
    // Each share rounds up: the pools together give back at least what was asked (work reclaim).
    Up,
};

// What each holding gives back so the pools together release `bytes`, never more than a holding
// holds. Without a fill, shares are proportional to what each holds across all holdings. During a
// fill the Ordinary tier covers as much of `bytes` as it holds, proportionally within the tier, and
// only the remainder falls to the FinishedPreview tier, proportionally within that tier.
[[nodiscard]] std::vector<std::size_t>
memoryEvictionShares(std::span<const MemoryEvictionHolding> holdings, std::size_t bytes,
                     bool previewFill, MemoryEvictionRounding rounding);

} // namespace bloom::runtime
