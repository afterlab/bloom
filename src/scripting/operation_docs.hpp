#pragma once

// Private to bloom_scripting: the reference notes of the built-in operations (operation_docs.cpp).

#include <bloom/scripting/operation_reference.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace bloom::scripting::detail {

// What the reference says about one operation beyond its schema. `outputs` names the ids the
// transaction result's `outputs` carry; `related` is a space-separated list of reference names;
// `errors` is newline-separated "when => remedy" lines specific to this operation; a line may start
// with "[invalid-argument] " or "[operation-warning] " to name its bloom.scripting.* code (the
// default is command-rejected).
struct OperationNotes final {
    std::string_view id;
    std::string_view description;
    std::string_view outputs;
    std::string_view related;
    std::string_view errors;
};

// The ids an operation reports, from its notes' `outputs` or, for an operation described by its
// own text, from the "Outputs:" sentence of `description`.
[[nodiscard]] std::vector<ReferenceOutput> operationOutputs(std::string_view typeId,
                                                            std::string_view description);
// What an operation changes beyond its named target; empty when it has no such effect.
[[nodiscard]] std::vector<std::string> operationSideEffects(std::string_view typeId);

[[nodiscard]] const OperationNotes* operationNotes(std::string_view typeId) noexcept;
[[nodiscard]] std::span<const OperationNotes> allOperationNotes() noexcept;

} // namespace bloom::scripting::detail
