#pragma once

// #637 Write to a Path: the result a Path-producing value node gave at one time, stored as points in
// a Path node (`bloom.value-path`), as ONE operation and so one undo step. The evaluation that makes
// the value happens before the operation is built (off the UI thread, by the caller); this stores it.

#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <optional>
#include <string_view>

namespace bloom::commands {

// The output that names the Path node written to (new or existing).
inline constexpr std::string_view kWriteToPathNodeOutput = "node";

class WriteValueToPath final : public Operation {
  public:
    // With no `target` a new Path node is added at `layoutPosition` and wired to nothing, so the
    // source's links are never changed. With a `target` Path node: a constant value is replaced; an
    // animated one gets a whole-path key at `time`; a driven one is refused.
    WriteValueToPath(document::CompositionId compositionId, std::optional<document::NodeId> target,
                     document::PathValue value, document::Vec2d layoutPosition,
                     core::RationalTime time)
        : compositionId_(compositionId), target_(target), value_(std::move(value)),
          layoutPosition_(layoutPosition), time_(time) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return compositionId_;
    }

  private:
    document::CompositionId compositionId_;
    std::optional<document::NodeId> target_;
    document::PathValue value_;
    document::Vec2d layoutPosition_;
    core::RationalTime time_;
};

} // namespace bloom::commands
