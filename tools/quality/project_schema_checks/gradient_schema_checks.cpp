#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
// Document 1.38 (#541) adds the gradient value: a constant of kind gradient and whole-value gradient
// animation curves. Removing exactly the gradient constant from parameterValue-1.4, the gradient
// curve from animationCurve-1.9 and the three gradient definitions, and renaming the version, must
// give back the 1.37 artifact exactly.
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.38",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.38").at("properties").at("minor"),
                 R"({"const":38})", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.38"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.38")
            member.first = "fixedVersion-1.37";
    previous.at("$defs").at("fixedVersion-1.37").at("properties").at("minor") =
        json::parse(R"({"const":37})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.37"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.37"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_38(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersion(schema, false);
    auto& defs = previous.at("$defs");
    requireExact(defs.at("gradientStop-1.38"),
                 R"({"type":"object","required":["position","red","green","blue","alpha"],)"
                 R"("properties":{"position":{"type":"number","minimum":0,"maximum":1},)"
                 R"("red":{"type":"number"},"green":{"type":"number"},"blue":{"type":"number"},)"
                 R"("alpha":{"type":"number","minimum":0,"maximum":1}},)"
                 R"("unevaluatedProperties":false})",
                 "1.38 gradient stop");
    requireExact(defs.at("gradientValue-1.38"),
                 R"({"type":"object","required":["kind","stops"],)"
                 R"("properties":{"kind":{"const":"gradient"},"stops":{"type":"array",)"
                 R"("minItems":1,"maxItems":64,"items":{"$ref":"#/$defs/gradientStop-1.38"}}},)"
                 R"("unevaluatedProperties":true})",
                 "1.38 gradient value");
    auto keyframe = defs.at("gradientKeyframe-1.38");
    requireExact(keyframe.at("properties").at("value"),
                 R"({"$ref":"#/$defs/gradientValue-1.38"})", "1.38 gradient key value");
    keyframe.at("properties").at("value") = json::parse(R"({"$ref":"#/$defs/parameterValue-1.4"})");
    if (!json::exactEqual(keyframe, defs.at("pathKeyframe-1.24")))
        fail("gradientKeyframe-1.38 is not pathKeyframe-1.24 with a gradient value");
    auto& values = defs.at("parameterValue-1.4").at("oneOf").asArray();
    if (values.empty())
        fail("parameterValue-1.4 has no alternatives");
    requireExact(values.back(), R"({"$ref":"#/$defs/gradientValue-1.38"})",
                 "1.38 gradient constant");
    values.pop_back();
    auto& curves = defs.at("animationCurve-1.9").at("oneOf").asArray();
    if (curves.empty())
        fail("animationCurve-1.9 has no alternatives");
    requireExact(curves.back(),
                 R"({"type":"object","required":["id","kind","keyframes"],)"
                 R"("properties":{"id":{"$ref":"#/$defs/objectId"},"kind":{"const":"gradient"},)"
                 R"("keyframes":{"type":"array","minItems":1,"maxItems":1000000,)"
                 R"("items":{"$ref":"#/$defs/gradientKeyframe-1.38"}}},)"
                 R"("unevaluatedProperties":true})",
                 "1.38 gradient curve");
    curves.pop_back();
    for (const auto* key : {"gradientStop-1.38", "gradientValue-1.38", "gradientKeyframe-1.38"})
        erase(defs, key);
    validateDocumentSchemaV1_37(previous);
}

void validateManifestSchemaV1_38(const json::Value& schema) {
    validateManifestSchemaV1_37(previousVersion(schema, true));
}
} // namespace bloom::quality
