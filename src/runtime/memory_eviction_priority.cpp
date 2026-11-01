#include <bloom/runtime/memory_eviction_priority.hpp>

#include <algorithm>
#include <cmath>

namespace bloom::runtime {
namespace {

// Distributes `bytes` over the holdings `inTier` selects, in proportion to what each holds.
void distribute(std::span<const MemoryEvictionHolding> holdings, std::vector<std::size_t>& shares,
                const long double bytes, const MemoryEvictionRounding rounding,
                const auto& inTier) {
    long double total = 0;
    for (const auto& holding : holdings)
        if (inTier(holding))
            total += static_cast<long double>(holding.heldBytes);
    if (total <= 0 || bytes <= 0)
        return;
    for (std::size_t index = 0; index < holdings.size(); ++index) {
        const auto& holding = holdings[index];
        if (!inTier(holding))
            continue;
        const auto own = static_cast<long double>(holding.heldBytes);
        auto share = bytes * own / total;
        if (rounding == MemoryEvictionRounding::Up)
            share = std::ceil(share);
        // Clamp before conversion so the share never exceeds what the pool holds.
        shares[index] = static_cast<std::size_t>(std::min(own, share));
    }
}

} // namespace

std::vector<std::size_t> memoryEvictionShares(std::span<const MemoryEvictionHolding> holdings,
                                              const std::size_t bytes, const bool previewFill,
                                              const MemoryEvictionRounding rounding) {
    std::vector<std::size_t> shares(holdings.size(), 0);
    const auto asked = static_cast<long double>(bytes);
    if (!previewFill) {
        distribute(holdings, shares, asked, rounding, [](const auto&) { return true; });
        return shares;
    }
    const auto ordinary = [](const MemoryEvictionHolding& holding) {
        return holding.priority == MemoryEvictionPriority::Ordinary;
    };
    long double ordinaryHeld = 0;
    for (const auto& holding : holdings)
        if (ordinary(holding))
            ordinaryHeld += static_cast<long double>(holding.heldBytes);
    const auto fromOrdinary = std::min(asked, ordinaryHeld);
    distribute(holdings, shares, fromOrdinary, rounding, ordinary);
    distribute(holdings, shares, asked - fromOrdinary, rounding,
               [&](const MemoryEvictionHolding& holding) { return !ordinary(holding); });
    return shares;
}

} // namespace bloom::runtime
