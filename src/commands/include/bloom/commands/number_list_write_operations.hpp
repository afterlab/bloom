#pragma once

// #627 Write to a Number List: the list a value node gave at one time, stored in a Number List
// node (`bloom.value-number-list`), as ONE operation and so one undo step. The evaluation that
// makes the value happens before the operation is built (off the UI thread, by the caller); this
// stores it.

#include <bloom/commands/operation.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <optional>
#include <string_view>

namespace bloom::commands {

// The output that names the Number List node written to (new or existing).
inline constexpr std::string_view kWriteToNumberListNodeOutput = "node";

class WriteValueToNumberList final : public Operation {
  public:
    // With no `target` a new Number List node is added at `layoutPosition` and wired to nothing,
    // so the source's links are never changed. With a `target` Number List node its constant is
    // replaced; a driven one is refused.
    WriteValueToNumberList(document::CompositionId compositionId,
                           std::optional<document::NodeId> target, document::NumberListValue value,
                           document::Vec2d layoutPosition)
        : compositionId_(compositionId), target_(target), value_(std::move(value)),
          layoutPosition_(layoutPosition) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return compositionId_;
    }

  private:
    document::CompositionId compositionId_;
    std::optional<document::NodeId> target_;
    document::NumberListValue value_;
    document::Vec2d layoutPosition_;
};

} // namespace bloom::commands
