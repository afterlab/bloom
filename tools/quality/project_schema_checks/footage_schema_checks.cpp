#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
// Document 1.34 (#381) adds the optional Interpret Footage members to an asset's interpretation and
// an optional coded pixel aspect ratio to a video stream. Removing exactly those, restoring the
// inline 1.11 interpretation and the 1.25 stream reference and renaming the version must give back
// the 1.33 artifact exactly.
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
constexpr std::string_view kRatio =
    R"({"type":"object","required":["numerator","denominator"],"properties":{)"
    R"("numerator":{"$ref":"#/$defs/positiveSigned64Decimal"},)"
    R"("denominator":{"$ref":"#/$defs/positiveSigned64Decimal"}},"unevaluatedProperties":false})";
json::Value previousVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.34",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.34").at("properties").at("minor"),
                 R"({"const":34})", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.34"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.34")
            member.first = "fixedVersion-1.33";
    previous.at("$defs").at("fixedVersion-1.33").at("properties").at("minor") =
        json::parse(R"({"const":33})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.33"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.33"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_34(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersion(schema, false);
    auto& defs = previous.at("$defs");
    auto& asset = defs.at("asset-1.11").at("properties");
    requireExact(asset.at("interpretation"), R"({"$ref":"#/$defs/assetInterpretation-1.34"})",
                 "1.34 asset interpretation");
    auto interpretation = defs.at("assetInterpretation-1.34");
    auto& members = interpretation.at("properties");
    const std::string ratio(kRatio);
    for (const auto& [key, expected] :
         std::initializer_list<std::pair<std::string_view, std::string>>{
             {"frameRate", ratio},
             {"startTimecode",
              R"({"type":"object","required":["start","dropFrame"],"properties":{)"
              R"("start":{"$ref":"#/$defs/rationalTime"},"dropFrame":{"type":"boolean"}},)"
              R"("unevaluatedProperties":false})"},
             {"alphaMode", R"({"enum":["ignore","straight","premultiplied"]})"},
             {"invertAlpha", R"({"const":true})"},
             {"matteColor", R"({"type":"array","minItems":3,"maxItems":3,)"
                            R"("items":{"type":"number","minimum":0,"maximum":1}})"},
             {"fieldOrder", R"({"enum":["progressive","upperFirst","lowerFirst"]})"},
             {"deinterlace", R"({"const":true})"},
             {"pixelAspect", ratio},
             {"loopCount", R"({"type":"integer","minimum":2,"maximum":1000})"},
             {"missingFrames", R"({"enum":["nothing"]})"}}) {
        requireExact(members.at(key), expected, "1.34 interpretation member");
        erase(members, key);
    }
    asset.at("interpretation") = interpretation;
    erase(defs, "assetInterpretation-1.34");
    auto& streams = defs.at("video-1.17").at("properties").at("streams");
    requireExact(streams.at("items"), R"({"$ref":"#/$defs/videoStream-1.34"})",
                 "1.34 video stream");
    auto stream = defs.at("videoStream-1.34");
    requireExact(stream.at("properties").at("pixelAspect"), ratio, "1.34 stream pixel aspect");
    erase(stream.at("properties"), "pixelAspect");
    if (!json::exactEqual(stream, defs.at("videoStream-1.25")))
        fail("videoStream-1.34 is not videoStream-1.25 with a pixel aspect ratio");
    streams.at("items") = json::parse(R"({"$ref":"#/$defs/videoStream-1.25"})");
    erase(defs, "videoStream-1.34");
    validateDocumentSchemaV1_33(previous);
}

void validateManifestSchemaV1_34(const json::Value& schema) {
    validateManifestSchemaV1_33(previousVersion(schema, true));
}
} // namespace bloom::quality
