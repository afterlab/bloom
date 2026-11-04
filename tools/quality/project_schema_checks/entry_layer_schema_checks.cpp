#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
// Document 1.37 (#502) lets a Merge entry own its layer: a layer output may name its slot, bind its
// own parameters and carry the frame pivot. Removing exactly the 1.37 layer output, pointing the
// graph back at layerOutput-1.0 and renaming the version must give back the 1.36 artifact exactly.
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.37",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.37").at("properties").at("minor"),
                 R"({"const":37})", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.37"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.37")
            member.first = "fixedVersion-1.36";
    previous.at("$defs").at("fixedVersion-1.36").at("properties").at("minor") =
        json::parse(R"({"const":36})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.36"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.36"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_37(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersion(schema, false);
    auto& defs = previous.at("$defs");
    auto& items = defs.at("graph-1.0").at("properties").at("layerOutputs").at("items");
    requireExact(items, R"({"$ref":"#/$defs/layerOutput-1.37"})", "1.37 graph layer outputs");
    auto layerOutput = defs.at("layerOutput-1.37");
    auto& properties = layerOutput.at("properties");
    requireExact(properties.at("slotId"), R"({"$ref":"#/$defs/objectId"})", "1.37 entry slot");
    requireExact(properties.at("pivot"), R"({"const":"frame"})", "1.37 layer pivot");
    requireExact(properties.at("parameters"),
                 R"({"type":"array","minItems":1,"maxItems":7,)"
                 R"("items":{"$ref":"#/$defs/parameterBinding-1.0"}})",
                 "1.37 entry layer parameters");
    for (const auto* key : {"slotId", "pivot", "parameters"})
        erase(properties, key);
    if (!json::exactEqual(layerOutput, defs.at("layerOutput-1.0")))
        fail("layerOutput-1.37 is not layerOutput-1.0 with the entry layer members");
    items = json::parse(R"({"$ref":"#/$defs/layerOutput-1.0"})");
    erase(defs, "layerOutput-1.37");
    validateDocumentSchemaV1_36(previous);
}

void validateManifestSchemaV1_37(const json::Value& schema) {
    validateManifestSchemaV1_36(previousVersion(schema, true));
}
} // namespace bloom::quality
