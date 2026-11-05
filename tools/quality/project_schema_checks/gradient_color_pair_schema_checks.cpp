#include "schema_check_helpers.hpp"

#include <string>

namespace bloom::quality {
// Document 1.39 (#541) moves the Linear and Radial Gradient colours and the Glow octave tints onto
// gradient values at node schema 2. Parameters are generic, so the document shape is unchanged:
// renaming the version must give back the 1.38 artifact exactly.
namespace {
json::Value previousVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.39",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.39").at("properties").at("minor"),
                 R"({"const":39})", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.39"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.39")
            member.first = "fixedVersion-1.38";
    previous.at("$defs").at("fixedVersion-1.38").at("properties").at("minor") =
        json::parse(R"({"const":38})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.38"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.38"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_39(const json::Value& schema) {
    validateDocumentSchemaV1_38(previousVersion(schema, false));
}

void validateManifestSchemaV1_39(const json::Value& schema) {
    validateManifestSchemaV1_38(previousVersion(schema, true));
}
} // namespace bloom::quality
