#pragma once

#include "schema_checks.hpp"

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::quality::schema_detail {

struct ExpectedBranch final {
    std::string_view kind;
    std::vector<std::string_view> required;
    std::vector<std::string_view> properties;
};

[[noreturn]] void fail(std::string_view message);

auto requireObject(const json::Value& value, std::string_view location)
    -> const json::Value::Object&;
[[nodiscard]] auto requireArray(const json::Value& value, std::string_view location)
    -> const json::Value::Array&;
[[nodiscard]] auto requireMember(const json::Value& object, std::string_view key,
                                 std::string_view location) -> const json::Value&;
[[nodiscard]] auto requireString(const json::Value& value, std::string_view location)
    -> const std::string&;
void requireExactString(const json::Value& value, std::string_view expected,
                        std::string_view location);
void requireExactBoolean(const json::Value& value, bool expected, std::string_view location);
void requireExactInteger(const json::Value& value, std::uint64_t expected,
                         std::string_view location);
void requireExact(const json::Value& value, std::string_view expectedJson,
                  std::string_view location);
void requireExactKeys(const json::Value& value, std::initializer_list<std::string_view> expected,
                      std::string_view location);
void requireExactKeys(const json::Value& value, std::span<const std::string_view> expected,
                      std::string_view location);
void requireExactStringArray(const json::Value& value,
                             std::initializer_list<std::string_view> expected,
                             std::string_view location);
void requireExactStringArray(const json::Value& value, std::span<const std::string_view> expected,
                             std::string_view location);

void validateReferences(const json::Value& root, const json::Value& value,
                        const std::string& location = "$", std::size_t depth = 1);
void validateVersionDefinition(const json::Value& definition, std::string_view location, bool fixed,
                               std::string_view majorReference = "#/$defs/positiveMajorVersion");
[[nodiscard]] auto validateObjectShape(const json::Value& definitions, std::string_view name,
                                       std::initializer_list<std::string_view> required,
                                       std::initializer_list<std::string_view> propertyNames)
    -> const json::Value&;
[[nodiscard]] auto validateDiscriminatedUnion(const json::Value& definitions, std::string_view name,
                                              std::span<const ExpectedBranch> expected)
    -> std::vector<const json::Value*>;
void validateArray(const json::Value& value, std::string_view location,
                   std::string_view itemReference, std::uint64_t maximum = 1'000'000,
                   std::uint64_t minimum = 0, bool hasMinimum = false);

// One pinned schema node: its JSON pointer and its own keywords, with each subschema replaced by
// a marker (member names for `properties` and `$defs`, a count for `oneOf`/`anyOf`/`allOf`, and
// "<schema>" for a single subschema). generate_schema_lock writes these from the artifacts.
struct SchemaNodePin final {
    std::string_view pointer;
    std::string_view node;
};

[[nodiscard]] auto documentSchemaLock() -> std::span<const SchemaNodePin>;
[[nodiscard]] auto manifestSchemaLock() -> std::span<const SchemaNodePin>;

// Walks every schema node in a fixed order and requires it to equal its pin, so an added, removed
// or changed node fails at its own pointer.
void validateSchemaLock(const json::Value& schema, std::span<const SchemaNodePin> lock,
                        std::string_view name);

// Rules every project schema keeps: references are repository-local and resolve, every definition
// is reachable from the root, every object schema says whether unknown members are kept and lists
// each required member among its properties, and every array is bounded.
void validateSchemaInvariants(const json::Value& schema, std::string_view name);

} // namespace bloom::quality::schema_detail
