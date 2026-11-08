#pragma once

// #574: layers and parameters addressed by name and role. A client turns `{"name": "Shot 2"}` or
// `{"layer": "Shot 2", "role": "opacity"}` into an ObjectReference value; the registry resolves
// every one to the id it names before the operation is created, scoped to a composition, so no
// operation factory has to know references exist.

#include <bloom/scripting/operation_registry.hpp>

#include <optional>

namespace bloom::scripting::detail {

// Replaces each ObjectReference among `arguments` with the id (or ids) it names. Only the
// arguments that hold a layer (`layer`, `layers`, and `parent` of a layer operation) or a
// parameter (`parameter`) take one. nullopt on success; otherwise the refusal, which names the
// candidates when a name is missing or ambiguous.
[[nodiscard]] std::optional<OperationDiagnostic>
resolveObjectReferences(const OperationSchema& schema, Arguments& arguments,
                        const OperationContext& context);

} // namespace bloom::scripting::detail
