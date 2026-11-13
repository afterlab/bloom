#pragma once

// #636 Bake to keyframes: the numbers a value node's Scalar output gave over the composition,
// stored as keyframes on a new Number node (`bloom.value-scalar`) beside it, as ONE operation and
// so one undo step. The evaluation that makes the numbers happens before the operation is built
// (off the UI thread, by the caller); this stores them. The keys are ordinary keyframes: they can
// be moved, deleted and eased, and they do not follow the source afterwards.

#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::commands {

// The output that names the Number node the keys were written to.
inline constexpr std::string_view kBakeToKeyframesNodeOutput = "node";
// The most keys one bake writes.
inline constexpr std::size_t kMaximumBakedKeys = 100'000;

struct BakedKey final {
    core::RationalTime time;
    double value = 0.0;
};

class BakeValueToKeyframes final : public Operation {
  public:
    // A new Number node is added at `layoutPosition`, wired to nothing, and its value keyed with
    // `keys` (ascending in time, linear between them). The source's links are never changed.
    BakeValueToKeyframes(document::CompositionId compositionId, std::vector<BakedKey> keys,
                         document::Vec2d layoutPosition)
        : compositionId_(compositionId), keys_(std::move(keys)), layoutPosition_(layoutPosition) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return compositionId_;
    }

  private:
    document::CompositionId compositionId_;
    std::vector<BakedKey> keys_;
    document::Vec2d layoutPosition_;
};

} // namespace bloom::commands
