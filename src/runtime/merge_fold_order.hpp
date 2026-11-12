#ifndef BLOOM_RUNTIME_MERGE_FOLD_ORDER_HPP
#define BLOOM_RUNTIME_MERGE_FOLD_ORDER_HPP

// Private to src/runtime. #343: the CPU reference's evaluation order when it folds Merges entry by
// entry, as the GPU executor does.
//
// In plan order the CPU reference evaluates every entry of a Merge before the Merge, so a stack of
// a few hundred imported shapes holds a few hundred images at once. In fold order each entry's own
// operations run just before the entry is composited, bottom entry first, and the entry is released
// as soon as it is folded into the Merge's accumulator. An operation belongs to an entry when every
// reader of it is in that entry (or is the Merge reading it as that entry); anything read from more
// than one place keeps its plan position. The per-pixel arithmetic and its order are unchanged, so
// both orders produce the same image.

#include "cpu_composition_evaluator_support.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace bloom::runtime::detail {

// One step: evaluate operation `index`, or (with `foldEntry`) composite entry `foldEntry` of the
// Merge at `index` into that Merge's accumulator.
struct EvaluationStep final {
    std::size_t index = 0;
    std::optional<std::size_t> foldEntry{};
};

[[nodiscard]] inline std::vector<EvaluationStep>
mergeFoldSteps(const CompiledCompositionPlan& plan) {
    const auto& operations = plan.operations();
    const auto count = operations.size();
    // Static: MSVC rejects an automatic constant as a default member of a local class (C2587).
    static constexpr auto kNone = std::numeric_limits<std::size_t>::max();
    struct Owner final {
        std::size_t merge = kNone;
        std::size_t entry = 0;
        bool seen = false;
        bool shared = false;
    };
    std::vector<Owner> owners(count);
    const auto note = [&](const std::size_t input, const std::size_t consumer,
                          const std::size_t merge, const std::size_t entry) {
        if (input >= count)
            return;
        auto& owner = owners[input];
        if (input >= consumer || merge == kNone) {
            owner.seen = true;
            owner.shared = true;
            return;
        }
        if (!owner.seen) {
            owner = {merge, entry, true, false};
            return;
        }
        if (owner.merge != merge || owner.entry != entry)
            owner.shared = true;
    };
    const auto ownerOf = [&](const std::size_t index) -> std::pair<std::size_t, std::size_t> {
        const auto& owner = owners[index];
        if (!owner.seen || owner.shared)
            return {kNone, 0};
        return {owner.merge, owner.entry};
    };
    for (std::size_t index = count; index-- > 0;) {
        const auto& operation = operations[index];
        if (const auto* merge = std::get_if<CompiledMerge>(&operation)) {
            for (std::size_t entry = 0; entry < merge->entries.size(); ++entry)
                note(merge->entries[entry].input.value(), index, index, entry);
            continue;
        }
        const auto [merge, entry] = ownerOf(index);
        forEachInput(operation,
                     [&](const OperationIndex input) { note(input.value(), index, merge, entry); });
    }

    std::vector<std::vector<std::vector<std::size_t>>> groups(count);
    for (std::size_t index = 0; index < count; ++index)
        if (const auto* merge = std::get_if<CompiledMerge>(&operations[index]))
            groups[index].resize(merge->entries.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto [merge, entry] = ownerOf(index);
        if (merge != kNone && merge < count && entry < groups[merge].size())
            groups[merge][entry].push_back(index);
        else
            owners[index].shared = true;
    }

    std::vector<EvaluationStep> steps;
    steps.reserve(count * 2);
    const std::function<void(std::size_t)> visit = [&](const std::size_t index) {
        if (const auto* merge = std::get_if<CompiledMerge>(&operations[index])) {
            for (std::size_t entry = merge->entries.size(); entry-- > 0;) {
                for (const auto member : groups[index][entry])
                    visit(member);
                steps.push_back({index, entry});
            }
        }
        steps.push_back({index, std::nullopt});
    };
    for (std::size_t index = 0; index < count; ++index)
        if (ownerOf(index).first == kNone)
            visit(index);
    return steps;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_MERGE_FOLD_ORDER_HPP
