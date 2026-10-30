#pragma once

// Private to bloom_scripting: the reference notes of the built-in operations (operation_docs.cpp).

#include <span>
#include <string_view>

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

[[nodiscard]] const OperationNotes* operationNotes(std::string_view typeId) noexcept;
[[nodiscard]] std::span<const OperationNotes> allOperationNotes() noexcept;

} // namespace bloom::scripting::detail
