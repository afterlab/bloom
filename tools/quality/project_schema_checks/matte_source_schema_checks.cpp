#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace bloom::quality {
// Document 1.42 (#383) lets a layer mask take its shape from a node's Mask output (`matte`) and a
// track matte name a Mask output (`node`) in place of its source layer. Removing exactly those two
// properties and the track matte's oneOf, restoring `source` as required, and renaming the version
// must give back the 1.41 artifact exactly.
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.42",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.42").at("properties").at("minor"),
                 R"({"const":42})", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.42"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.42")
            member.first = "fixedVersion-1.41";
    previous.at("$defs").at("fixedVersion-1.41").at("properties").at("minor") =
        json::parse(R"({"const":41})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.41"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.41"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_42(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersion(schema, false);
    auto& defs = previous.at("$defs");
    auto& mask = defs.at("layerMask-1.24").at("properties");
    requireExact(mask.at("matte"), R"({"$ref":"#/$defs/outputPortReference-1.0"})",
                 "1.42 matte mask property");
    erase(mask, "matte");
    auto& matte = defs.at("trackMatte-1.24");
    requireExact(matte.at("properties").at("node"),
                 R"({"$ref":"#/$defs/outputPortReference-1.0"})", "1.42 node track matte property");
    requireExact(matte.at("oneOf"), R"([{"required":["source"]},{"required":["node"]}])",
                 "1.42 track matte source or node");
    requireExact(matte.at("required"), R"(["channel","inverted"])", "1.42 track matte required");
    erase(matte.at("properties"), "node");
    erase(matte, "oneOf");
    matte.at("required") = json::parse(R"(["source","channel","inverted"])");
    validateDocumentSchemaV1_41(previous);
}

void validateManifestSchemaV1_42(const json::Value& schema) {
    validateManifestSchemaV1_41(previousVersion(schema, true));
}
} // namespace bloom::quality
