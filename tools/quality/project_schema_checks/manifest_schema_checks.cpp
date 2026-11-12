#include "schema_check_helpers.hpp"

#include <array>
#include <string>
#include <string_view>

namespace bloom::quality {
namespace {

constexpr std::string_view draft202012 = "https://json-schema.org/draft/2020-12/schema";
constexpr std::string_view manifestId = "urn:afterlab:bloom:schema:project-manifest:1.0";
constexpr std::string_view identifierPattern = "^[a-z0-9][a-z0-9._-]{0,127}$";

} // namespace

void validateManifestSchema(const json::Value& schema) {
    using namespace schema_detail;
    requireObject(schema, "manifest schema root");
    requireExactString(requireMember(schema, "$schema", "$"), draft202012, "$.$schema");
    requireExactString(requireMember(schema, "$id", "$"), manifestId, "$.$id");
    requireExactString(requireMember(schema, "title", "$"), "Bloom Project Manifest 1.0",
                       "$.title");
    static_cast<void>(requireString(requireMember(schema, "$comment", "$"), "$.$comment"));
    requireExactString(requireMember(schema, "type", "$"), "object", "$.type");
    requireExactStringArray(requireMember(schema, "required", "$"),
                            {"format", "containerVersion", "document", "requirements"},
                            "$.required");
    requireExactBoolean(requireMember(schema, "unevaluatedProperties", "$"), true,
                        "$.unevaluatedProperties");

    const auto& properties = requireMember(schema, "properties", "$");
    requireExactKeys(properties,
                     {"format", "containerVersion", "document", "requirements", "generator"},
                     "$.properties keys");
    requireExact(requireMember(properties, "format", "$.properties"),
                 R"({"const":"org.kinetik.bloom.project"})", "$.properties.format");
    requireExact(requireMember(properties, "containerVersion", "$.properties"),
                 R"({"$ref":"#/$defs/fixedVersion-1.0"})", "$.properties.containerVersion");
    requireExact(requireMember(properties, "document", "$.properties"),
                 R"({"$ref":"#/$defs/document-1.0"})", "$.properties.document");

    const auto& requirements = requireMember(properties, "requirements", "$.properties");
    requireExactString(requireMember(requirements, "type", "$.properties.requirements"), "array",
                       "$.properties.requirements.type");
    requireExactInteger(requireMember(requirements, "maxItems", "$.properties.requirements"),
                        1'000'000, "$.properties.requirements.maxItems");
    requireExact(requireMember(requirements, "items", "$.properties.requirements"),
                 R"({"$ref":"#/$defs/requirement-1.0"})", "$.properties.requirements.items");
    if (requirements.find("uniqueItems") != nullptr) {
        fail("requirement identity uniqueness remains a Project I/O semantic check");
    }

    const auto& definitions = requireMember(schema, "$defs", "$");
    requireExactKeys(definitions,
                     {"unsigned32", "positiveMajorVersion", "namespacedIdentifier",
                      "fixedVersion-1.0", "requirementVersion-1.0", "document-1.0",
                      "requirement-1.0", "documentSchemaVersion", "generatorText", "generator-1.0"},
                     "$.$defs keys");
    requireExact(requireMember(definitions, "unsigned32", "$.$defs"),
                 R"({"type":"integer","minimum":0,"maximum":4294967295})", "$.$defs.unsigned32");
    requireExact(requireMember(definitions, "positiveMajorVersion", "$.$defs"),
                 R"({"type":"integer","minimum":1,"maximum":4294967295})",
                 "$.$defs.positiveMajorVersion");
    requireExact(requireMember(definitions, "namespacedIdentifier", "$.$defs"),
                 "{\"type\":\"string\",\"minLength\":1,\"maxLength\":128,\"pattern\":\"" +
                     std::string{identifierPattern} + "\"}",
                 "$.$defs.namespacedIdentifier");
    validateVersionDefinition(requireMember(definitions, "fixedVersion-1.0", "$.$defs"),
                              "$.$defs.fixedVersion-1.0", true);
    validateVersionDefinition(requireMember(definitions, "requirementVersion-1.0", "$.$defs"),
                              "$.$defs.requirementVersion-1.0", false);

    const auto& document = requireMember(definitions, "document-1.0", "$.$defs");
    requireExactString(requireMember(document, "type", "$.$defs.document-1.0"), "object",
                       "$.$defs.document-1.0.type");
    requireExactStringArray(requireMember(document, "required", "$.$defs.document-1.0"),
                            {"path", "schemaVersion"}, "$.$defs.document-1.0.required");
    requireExactBoolean(requireMember(document, "unevaluatedProperties", "$.$defs.document-1.0"),
                        true, "$.$defs.document-1.0.unevaluatedProperties");
    requireExact(
        requireMember(document, "properties", "$.$defs.document-1.0"),
        R"({"path":{"const":"document.json"},"schemaVersion":{"$ref":"#/$defs/documentSchemaVersion"}})",
        "$.$defs.document-1.0.properties");
    // The manifest names the one document schema, 1.0. It stays open like the document's own
    // version object so a newer minor's manifest still reads.
    requireExact(
        requireMember(definitions, "documentSchemaVersion", "$.$defs"),
        R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":0}},"unevaluatedProperties":true})",
        "$.$defs.documentSchemaVersion");

    const auto& requirement = requireMember(definitions, "requirement-1.0", "$.$defs");
    requireExactString(requireMember(requirement, "type", "$.$defs.requirement-1.0"), "object",
                       "$.$defs.requirement-1.0.type");
    requireExactStringArray(requireMember(requirement, "required", "$.$defs.requirement-1.0"),
                            {"providerId", "capabilityId", "schemaVersion", "providedNodeTypeIds"},
                            "$.$defs.requirement-1.0.required");
    requireExactBoolean(
        requireMember(requirement, "unevaluatedProperties", "$.$defs.requirement-1.0"), true,
        "$.$defs.requirement-1.0.unevaluatedProperties");
    const auto& requirementProperties =
        requireMember(requirement, "properties", "$.$defs.requirement-1.0");
    requireExactKeys(requirementProperties,
                     {"providerId", "capabilityId", "schemaVersion", "providedNodeTypeIds"},
                     "$.$defs.requirement-1.0.properties keys");
    for (const std::string_view identifier : {"providerId", "capabilityId"}) {
        requireExact(
            requireMember(requirementProperties, identifier, "$.$defs.requirement-1.0.properties"),
            R"({"$ref":"#/$defs/namespacedIdentifier"})",
            "$.$defs.requirement-1.0.properties." + std::string{identifier});
    }
    requireExact(
        requireMember(requirementProperties, "schemaVersion", "$.$defs.requirement-1.0.properties"),
        R"({"$ref":"#/$defs/requirementVersion-1.0"})",
        "$.$defs.requirement-1.0.properties.schemaVersion");

    const auto& provided = requireMember(requirementProperties, "providedNodeTypeIds",
                                         "$.$defs.requirement-1.0.properties");
    requireExactString(requireMember(provided, "type", "providedNodeTypeIds"), "array",
                       "providedNodeTypeIds.type");
    requireExactInteger(requireMember(provided, "maxItems", "providedNodeTypeIds"), 1'000'000,
                        "providedNodeTypeIds.maxItems");
    if (provided.find("uniqueItems") != nullptr) {
        fail("provided-node ordering and uniqueness remain Project I/O semantic checks");
    }
    const auto& allOf =
        requireArray(requireMember(requireMember(provided, "items", "providedNodeTypeIds"), "allOf",
                                   "providedNodeTypeIds.items"),
                     "providedNodeTypeIds.items.allOf");
    if (allOf.size() != 2U) {
        fail("providedNodeTypeIds.items.allOf must contain two constraints");
    }
    requireExact(allOf[0], R"({"$ref":"#/$defs/namespacedIdentifier"})",
                 "providedNodeTypeIds.items.allOf[0]");
    requireExact(
        allOf[1],
        R"({"not":{"enum":["bloom.composition-output","bloom.layer-output","bloom.layer-stack","bloom.solid-source","bloom.text-source","bloom.image-source","bloom.audio-source","bloom.shape-source"]}})",
        "providedNodeTypeIds.items.allOf[1]");

    // The optional generator records which application wrote the file. Reading never decides
    // anything from it, so it is never required.
    requireExact(requireMember(properties, "generator", "$.properties"),
                 R"({"$ref":"#/$defs/generator-1.0"})", "$.properties.generator");
    const auto& generator = requireMember(definitions, "generator-1.0", "$.$defs");
    requireExactString(requireMember(generator, "type", "$.$defs.generator-1.0"), "object",
                       "$.$defs.generator-1.0.type");
    requireExactStringArray(requireMember(generator, "required", "$.$defs.generator-1.0"),
                            {"name", "version"}, "$.$defs.generator-1.0.required");
    requireExact(
        requireMember(generator, "properties", "$.$defs.generator-1.0"),
        R"({"name":{"$ref":"#/$defs/generatorText"},"version":{"$ref":"#/$defs/generatorText"}})",
        "$.$defs.generator-1.0.properties");
    const auto& generatorText = requireMember(definitions, "generatorText", "$.$defs");
    requireExactString(requireMember(generatorText, "type", "$.$defs.generatorText"), "string",
                       "$.$defs.generatorText.type");
    requireExactInteger(requireMember(generatorText, "minLength", "$.$defs.generatorText"), 1,
                        "$.$defs.generatorText.minLength");
    const auto& generatorComment =
        requireString(requireMember(generatorText, "$comment", "$.$defs.generatorText"),
                      "$.$defs.generatorText.$comment");
    if (generatorComment.find("256-byte") == std::string::npos ||
        generatorText.find("maxLength") != nullptr) {
        fail("$.$defs.generatorText must leave its 256-byte UTF-8 ceiling to Project I/O");
    }

    validateSchemaInvariants(schema, "manifest schema");
    validateSchemaLock(schema, manifestSchemaLock(), "manifest schema");
}

} // namespace bloom::quality
