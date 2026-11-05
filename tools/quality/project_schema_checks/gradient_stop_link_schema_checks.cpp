#include "schema_check_helpers.hpp"

#include <string>

namespace bloom::quality {
// Document 1.40 (#543) lets a value node drive a gradient stop's colour or position through a
// driver-only parameter bound under the stop's socket name. Parameters and bindings are generic, so
// the document shape is unchanged: renaming the version must give back the 1.39 artifact exactly.
namespace {
json::Value previousVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.40",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.40").at("properties").at("minor"),
                 R"({"const":40})", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.40"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.40")
            member.first = "fixedVersion-1.39";
    previous.at("$defs").at("fixedVersion-1.39").at("properties").at("minor") =
        json::parse(R"({"const":39})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.39"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.39"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_40(const json::Value& schema) {
    validateDocumentSchemaV1_39(previousVersion(schema, false));
}

void validateManifestSchemaV1_40(const json::Value& schema) {
    validateManifestSchemaV1_39(previousVersion(schema, true));
}
} // namespace bloom::quality
