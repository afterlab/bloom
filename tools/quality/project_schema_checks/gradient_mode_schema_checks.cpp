#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace bloom::quality {
// Document 1.41 (#546) lets a gradient value carry its colour mode, hue direction and
// interpolation, each written only when it is not the default. Removing exactly those three
// properties from gradientValue-1.38 and their three definitions, and renaming the version, must
// give back the 1.40 artifact exactly.
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.41",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.41").at("properties").at("minor"),
                 R"({"const":41})", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.41"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.41")
            member.first = "fixedVersion-1.40";
    previous.at("$defs").at("fixedVersion-1.40").at("properties").at("minor") =
        json::parse(R"({"const":40})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.40"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.40"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_41(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersion(schema, false);
    auto& defs = previous.at("$defs");
    requireExact(defs.at("gradientColorMode-1.41"), R"({"enum":["hsv","hsl"]})",
                 "1.41 gradient colour mode");
    requireExact(defs.at("gradientHueDirection-1.41"),
                 R"({"enum":["far","clockwise","counter-clockwise"]})",
                 "1.41 gradient hue direction");
    requireExact(defs.at("gradientInterpolation-1.41"),
                 R"({"enum":["constant","ease","cardinal","b-spline","exponential"]})",
                 "1.41 gradient interpolation");
    auto& properties = defs.at("gradientValue-1.38").at("properties");
    requireExact(properties.at("colorMode"), R"({"$ref":"#/$defs/gradientColorMode-1.41"})",
                 "1.41 gradient colour mode property");
    requireExact(properties.at("hueDirection"), R"({"$ref":"#/$defs/gradientHueDirection-1.41"})",
                 "1.41 gradient hue direction property");
    requireExact(properties.at("interpolation"), R"({"$ref":"#/$defs/gradientInterpolation-1.41"})",
                 "1.41 gradient interpolation property");
    for (const auto* key : {"colorMode", "hueDirection", "interpolation"})
        erase(properties, key);
    for (const auto* key :
         {"gradientColorMode-1.41", "gradientHueDirection-1.41", "gradientInterpolation-1.41"})
        erase(defs, key);
    validateDocumentSchemaV1_40(previous);
}

void validateManifestSchemaV1_41(const json::Value& schema) {
    validateManifestSchemaV1_40(previousVersion(schema, true));
}
} // namespace bloom::quality
