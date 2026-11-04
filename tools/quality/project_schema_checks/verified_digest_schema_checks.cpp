#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
// Document 1.36 (#495) lets a stamped sequence member also carry the content digest a full hash
// verified for the bytes that carried its stamp. Removing exactly the 1.36 member and manifest,
// restoring the 1.35 member reference and renaming the version must give back the 1.35 artifact
// exactly.
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.36",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.36").at("properties").at("minor"),
                 R"({"const":36})", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.36"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.36")
            member.first = "fixedVersion-1.35";
    previous.at("$defs").at("fixedVersion-1.35").at("properties").at("minor") =
        json::parse(R"({"const":35})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.35"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.35"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_36(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersion(schema, false);
    auto& defs = previous.at("$defs");
    auto& asset = defs.at("asset-1.11").at("properties");
    requireExact(asset.at("manifest"), R"({"$ref":"#/$defs/assetManifest-1.36"})",
                 "1.36 asset manifest");
    requireExact(defs.at("assetMember-1.36"),
                 R"({"type":"object","required":["frame","locator"],"properties":{)"
                 R"("frame":{"$ref":"#/$defs/signed64Decimal"},)"
                 R"("locator":{"$ref":"#/$defs/assetLocator-1.11"},)"
                 R"("contentDigest":{"type":"string","pattern":"^[0-9a-f]{64}$"},)"
                 R"("stamp":{"type":"object","required":["size","modified"],"properties":{)"
                 R"("size":{"$ref":"#/$defs/allocatorHighWater"},)"
                 R"("modified":{"$ref":"#/$defs/signed64Decimal"}},)"
                 R"("unevaluatedProperties":false}},)"
                 R"("anyOf":[{"required":["contentDigest"]},{"required":["stamp"]}],)"
                 R"("unevaluatedProperties":false})",
                 "1.36 sequence member: a content digest, a file stamp, or both");
    erase(defs, "assetMember-1.36");
    auto manifestDef = defs.at("assetManifest-1.36");
    auto& items = manifestDef.at("properties").at("members").at("items");
    requireExact(items, R"({"$ref":"#/$defs/assetMember-1.36"})", "1.36 manifest members");
    items = json::parse(R"({"$ref":"#/$defs/assetMember-1.35"})");
    if (!json::exactEqual(manifestDef, defs.at("assetManifest-1.35")))
        fail("assetManifest-1.36 is not assetManifest-1.35 with verified members");
    asset.at("manifest") = json::parse(R"({"$ref":"#/$defs/assetManifest-1.35"})");
    erase(defs, "assetManifest-1.36");
    validateDocumentSchemaV1_35(previous);
}

void validateManifestSchemaV1_36(const json::Value& schema) {
    validateManifestSchemaV1_35(previousVersion(schema, true));
}
} // namespace bloom::quality
