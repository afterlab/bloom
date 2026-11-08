#include "schema_check_helpers.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace bloom::quality {
// Document 1.43 (#589) adds the compound path value as one more constant value shape; the 1.14
// shape stays the spelling of a one-contour path. Removing that alternative and its definition and
// renaming the version must give back the 1.42 artifact exactly.
namespace {
void eraseMember(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.43",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.43").at("properties").at("minor"),
                 R"({"const":43})", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.43"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.43")
            member.first = "fixedVersion-1.42";
    previous.at("$defs").at("fixedVersion-1.42").at("properties").at("minor") =
        json::parse(R"({"const":42})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.42"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.42"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_43(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersion(schema, false);
    auto& defs = previous.at("$defs");
    requireExact(defs.at("compoundPathValue-1.43").at("required"), R"(["kind","contours"])",
                 "1.43 compound path required members");
    eraseMember(defs, "compoundPathValue-1.43");
    auto& alternatives = defs.at("parameterValue-1.4").at("oneOf").asArray();
    const std::array<std::string_view, 1> kind{"$ref"};
    const auto removed = std::erase_if(alternatives, [&kind](const json::Value& alternative) {
        if (!alternative.isObject())
            return false;
        const auto& members = alternative.asObject();
        return members.size() == kind.size() && members.front().first == kind.front() &&
               members.front().second.isString() &&
               members.front().second.asString() == "#/$defs/compoundPathValue-1.43";
    });
    if (removed != 1)
        fail("1.43 parameter value must offer the compound path exactly once");
    validateDocumentSchemaV1_42(previous);
}

void validateManifestSchemaV1_43(const json::Value& schema) {
    validateManifestSchemaV1_42(previousVersion(schema, true));
}
} // namespace bloom::quality
