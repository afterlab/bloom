#include "schema_check_helpers.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::quality {
namespace {

using schema_detail::ExpectedBranch;

constexpr std::string_view draft202012 = "https://json-schema.org/draft/2020-12/schema";
constexpr std::string_view documentId = "urn:afterlab:bloom:schema:project-document:1.0";
constexpr std::string_view identifierPattern = "^[a-z0-9][a-z0-9._-]{0,127}$";
constexpr std::string_view objectIdPattern =
    "^(?:[1-9][0-9]{0,18}|1[0-7][0-9]{18}|18[0-3][0-9]{17}|184[0-3][0-9]{16}|"
    "1844[0-5][0-9]{15}|18446[0-6][0-9]{14}|184467[0-3][0-9]{13}|"
    "1844674[0-3][0-9]{12}|184467440[0-6][0-9]{10}|1844674407[0-2][0-9]{9}|"
    "18446744073[0-6][0-9]{8}|1844674407370[0-8][0-9]{6}|"
    "18446744073709[0-4][0-9]{5}|184467440737095[0-4][0-9]{4}|"
    "1844674407370955[0][0-9]{3}|18446744073709551[0-5][0-9]{2}|"
    "184467440737095516[0][0-9]|1844674407370955161[0-5])$";
constexpr std::string_view allocatorPattern =
    "^(?:0|[1-9][0-9]{0,18}|1[0-7][0-9]{18}|18[0-3][0-9]{17}|184[0-3][0-9]{16}|"
    "1844[0-5][0-9]{15}|18446[0-6][0-9]{14}|184467[0-3][0-9]{13}|"
    "1844674[0-3][0-9]{12}|184467440[0-6][0-9]{10}|1844674407[0-2][0-9]{9}|"
    "18446744073[0-6][0-9]{8}|1844674407370[0-8][0-9]{6}|"
    "18446744073709[0-4][0-9]{5}|184467440737095[0-4][0-9]{4}|"
    "1844674407370955[0][0-9]{3}|18446744073709551[0-5][0-9]{2}|"
    "184467440737095516[0][0-9]|1844674407370955161[0-5])$";
constexpr std::string_view positiveSigned64Body =
    "[1-9][0-9]{0,17}|[1-8][0-9]{18}|9[0-1][0-9]{17}|92[0-1][0-9]{16}|"
    "922[0-2][0-9]{15}|9223[0-2][0-9]{14}|92233[0-6][0-9]{13}|"
    "922337[0-1][0-9]{12}|92233720[0-2][0-9]{10}|922337203[0-5][0-9]{9}|"
    "9223372036[0-7][0-9]{8}|92233720368[0-4][0-9]{7}|"
    "922337203685[0-3][0-9]{6}|9223372036854[0-6][0-9]{5}|"
    "92233720368547[0-6][0-9]{4}|922337203685477[0-4][0-9]{3}|"
    "9223372036854775[0-7][0-9]{2}|922337203685477580[0-7]";
constexpr std::string_view positiveUnsigned32Pattern =
    "^(?:[1-9][0-9]{0,8}|[1-3][0-9]{9}|4[0-1][0-9]{8}|42[0-8][0-9]{7}|"
    "429[0-3][0-9]{6}|4294[0-8][0-9]{5}|42949[0-5][0-9]{4}|"
    "429496[0-6][0-9]{3}|4294967[0-1][0-9]{2}|42949672[0-8][0-9]|"
    "429496729[0-5])$";

void requirePatternDefinition(const json::Value& definitions, const std::string_view name,
                              const std::string_view pattern) {
    using namespace schema_detail;
    const auto location = "$.$defs." + std::string{name};
    const auto& definition = requireMember(definitions, name, "$.$defs");
    requireExactKeys(definition, {"type", "pattern"}, location + " keys");
    requireExactString(requireMember(definition, "type", location), "string", location + ".type");
    requireExactString(requireMember(definition, "pattern", location), pattern,
                       location + ".pattern");
}

void validateTextDefinitions(const json::Value& definitions) {
    using namespace schema_detail;
    for (const auto [name, maximumLabel] :
         {std::pair{"humanFacingName", "4096-byte"}, std::pair{"structuralText", "256-byte"}}) {
        const auto location = "$.$defs." + std::string{name};
        const auto& value = requireMember(definitions, name, "$.$defs");
        requireExactKeys(value, {"$comment", "type", "minLength"}, location + " keys");
        requireExactString(requireMember(value, "type", location), "string", location + ".type");
        requireExactInteger(requireMember(value, "minLength", location), 1,
                            location + ".minLength");
        const auto& comment =
            requireString(requireMember(value, "$comment", location), location + ".$comment");
        if (comment.find(maximumLabel) == std::string::npos || value.find("maxLength") != nullptr) {
            fail(location + " must leave its UTF-8 byte ceiling to Project I/O");
        }
    }
    requireExact(requireMember(definitions, "namespacedIdentifier", "$.$defs"),
                 "{\"type\":\"string\",\"minLength\":1,\"maxLength\":128,\"pattern\":\"" +
                     std::string{identifierPattern} + "\"}",
                 "$.$defs.namespacedIdentifier");
}

void validateRationals(const json::Value& definitions) {
    using namespace schema_detail;
    struct RationalSpec final {
        std::string_view name;
        std::string_view numerator;
        std::string_view denominator;
    };
    constexpr std::array specs{
        RationalSpec{"rationalTime", "#/$defs/signed64Decimal", "#/$defs/positiveSigned64Decimal"},
        RationalSpec{"positiveRationalTime", "#/$defs/positiveSigned64Decimal",
                     "#/$defs/positiveSigned64Decimal"},
        RationalSpec{"unsignedRatio", "#/$defs/positiveUnsigned32Decimal",
                     "#/$defs/positiveUnsigned32Decimal"},
    };
    for (const auto& spec : specs) {
        const auto& rational = validateObjectShape(
            definitions, spec.name, {"numerator", "denominator"}, {"numerator", "denominator"});
        requireExact(requireMember(rational, "numerator", spec.name),
                     "{\"$ref\":\"" + std::string{spec.numerator} + "\"}",
                     "$.$defs." + std::string{spec.name} + ".properties.numerator");
        requireExact(requireMember(rational, "denominator", spec.name),
                     "{\"$ref\":\"" + std::string{spec.denominator} + "\"}",
                     "$.$defs." + std::string{spec.name} + ".properties.denominator");
    }
}

void validateColorDefinitions(const json::Value& definitions) {
    using namespace schema_detail;
    const auto& color = validateObjectShape(definitions, "colorSettings-1.0",
                                            {"schemaVersion", "processColorSpaceId", "ocioConfig"},
                                            {"schemaVersion", "processColorSpaceId", "ocioConfig"});
    requireExact(requireMember(color, "schemaVersion", "colorSettings"),
                 R"({"$ref":"#/$defs/fixedVersion-1.0"})",
                 "$.$defs.colorSettings-1.0.properties.schemaVersion");
    requireExact(requireMember(color, "processColorSpaceId", "colorSettings"),
                 R"({"const":"lin_rec709_scene"})",
                 "$.$defs.colorSettings-1.0.properties.processColorSpaceId");
    requireExact(requireMember(color, "ocioConfig", "colorSettings"),
                 R"({"$ref":"#/$defs/ocioConfigReference-1.0"})",
                 "$.$defs.colorSettings-1.0.properties.ocioConfig");

    const auto& reference = validateObjectShape(
        definitions, "ocioConfigReference-1.0",
        {"schemaVersion", "locator", "expectedRevision", "portability", "contextVariables"},
        {"schemaVersion", "locator", "expectedRevision", "portability", "contextVariables"});
    requireExact(requireMember(reference, "portability", "ocioConfigReference"),
                 R"({"enum":["builtin","project-relative","external"]})",
                 "$.$defs.ocioConfigReference-1.0.properties.portability");
    validateArray(requireMember(reference, "contextVariables", "ocioConfigReference"),
                  "$.$defs.ocioConfigReference-1.0.properties.contextVariables",
                  "#/$defs/ocioContextVariable-1.0", 256);

    const std::array locatorSpecs{
        ExpectedBranch{"builtin", {"kind", "uri"}, {"kind", "uri"}},
        ExpectedBranch{"project-relative-ocioz", {"kind", "path"}, {"kind", "path"}},
        ExpectedBranch{"external-ocioz", {"kind", "uri"}, {"kind", "uri"}},
        ExpectedBranch{"external-config", {"kind", "uri"}, {"kind", "uri"}},
    };
    const auto locators =
        validateDiscriminatedUnion(definitions, "ocioConfigLocator-1.0", locatorSpecs);
    requireExact(requireMember(*locators[0], "uri", "builtin locator"),
                 R"({"const":"bloom://ocio/neutral-v1/config.ocio"})",
                 "$.$defs.ocioConfigLocator-1.0 builtin URI");
    requireExact(
        requireMember(*locators[1], "path", "project-relative locator"),
        R"schema({"$comment":"The 4096-byte UTF-8 ceiling and complete normalized-component profile are Project I/O checks.","type":"string","minLength":1,"allOf":[{"pattern":"^(?!/)(?![A-Za-z]:)(?!.*\\\\)(?!.*//)[^\\u0000]+$"},{"pattern":"\\.ocioz$"}]})schema",
        "$.$defs.ocioConfigLocator-1.0 project-relative-ocioz path");
    for (const auto index : {2U, 3U}) {
        requireExact(requireMember(*locators[index], "uri", "external locator"),
                     R"({"$ref":"#/$defs/externalFileUri"})",
                     "$.$defs.ocioConfigLocator-1.0 external URI");
    }
    requireExact(
        requireMember(definitions, "externalFileUri", "$.$defs"),
        R"schema({"$comment":"Project I/O enforces the exact RFC 8089 subset, percent-escape rules, and decoded .ocioz/config.ocio leaf name.","type":"string","minLength":1,"maxLength":16384,"allOf":[{"pattern":"^[\\u0001-\\u007f]+$"},{"pattern":"^[Ff][Ii][Ll][Ee]:"},{"pattern":"^[^?#]*$"}]})schema",
        "$.$defs.externalFileUri");

    const auto& revision = validateObjectShape(definitions, "ocioConfigRevision-1.0",
                                               {"algorithm", "digest"}, {"algorithm", "digest"});
    requireExact(requireMember(revision, "algorithm", "revision"), R"({"const":"sha256"})",
                 "$.$defs.ocioConfigRevision-1.0.properties.algorithm");
    requireExact(requireMember(revision, "digest", "revision"),
                 R"({"type":"string","minLength":64,"maxLength":64,"pattern":"^[0-9a-f]{64}$"})",
                 "$.$defs.ocioConfigRevision-1.0.properties.digest");

    const auto& context = validateObjectShape(definitions, "ocioContextVariable-1.0",
                                              {"name", "value"}, {"name", "value"});
    requireExact(
        requireMember(context, "name", "context"),
        R"({"type":"string","minLength":1,"maxLength":128,"pattern":"^[A-Za-z_][A-Za-z0-9_]{0,127}$"})",
        "$.$defs.ocioContextVariable-1.0.properties.name");
    requireExact(
        requireMember(context, "value", "context"),
        R"schema({"$comment":"The normative 4096-byte UTF-8 ceiling is enforced by Project I/O.","type":"string","pattern":"^[^\\u0000]*$"})schema",
        "$.$defs.ocioContextVariable-1.0.properties.value");
}

} // namespace

// The one project document schema. The semantic checks below pin what Project I/O relies on and
// read as the reason for each constraint; the generated lock (project_schema_lock.cpp) then pins
// every remaining schema node, so no edit to the artifact passes without a reviewed lock change.
void validateDocumentSchema(const json::Value& schema) {
    using namespace schema_detail;
    requireExactKeys(schema,
                     {"$schema", "$id", "title", "$comment", "type", "required", "properties",
                      "$defs", "unevaluatedProperties"},
                     "document schema root keys");
    requireExactString(requireMember(schema, "$schema", "$"), draft202012, "$.$schema");
    requireExactString(requireMember(schema, "$id", "$"), documentId, "$.$id");
    requireExactString(requireMember(schema, "title", "$"), "Bloom Project Document 1.0",
                       "$.title");
    static_cast<void>(requireString(requireMember(schema, "$comment", "$"), "$.$comment"));
    requireExactString(requireMember(schema, "type", "$"), "object", "$.type");
    requireExactStringArray(requireMember(schema, "required", "$"),
                            {"schemaVersion", "project", "idAllocation", "extensions"},
                            "$.required");
    // A same-major newer minor keeps unknown members for preservation, so the root stays open.
    requireExactBoolean(requireMember(schema, "unevaluatedProperties", "$"), true,
                        "$.unevaluatedProperties");

    const auto& properties = requireMember(schema, "properties", "$");
    requireExactKeys(properties, {"schemaVersion", "project", "idAllocation", "extensions"},
                     "$.properties keys");
    requireExact(requireMember(properties, "schemaVersion", "$.properties"),
                 R"({"$ref":"#/$defs/documentSchemaVersion"})", "$.properties.schemaVersion");
    requireExact(requireMember(properties, "project", "$.properties"),
                 R"({"$ref":"#/$defs/project-1.0"})", "$.properties.project");
    requireExact(requireMember(properties, "idAllocation", "$.properties"),
                 R"({"$ref":"#/$defs/idAllocation-1.0"})", "$.properties.idAllocation");
    validateArray(requireMember(properties, "extensions", "$.properties"),
                  "$.properties.extensions", "#/$defs/extensionRecord-1.0");

    const auto& definitions = requireMember(schema, "$defs", "$");
    // The schema this file describes is exactly 1.0; the writer and decoder agree on it.
    requireExact(
        requireMember(definitions, "documentSchemaVersion", "$.$defs"),
        R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":0}},"unevaluatedProperties":false})",
        "$.$defs.documentSchemaVersion");
    requireExact(requireMember(definitions, "unsigned32", "$.$defs"),
                 R"({"type":"integer","minimum":0,"maximum":4294967295})", "$.$defs.unsigned32");
    requireExact(requireMember(definitions, "positiveUnsigned32", "$.$defs"),
                 R"({"type":"integer","minimum":1,"maximum":4294967295})",
                 "$.$defs.positiveUnsigned32");
    validateVersionDefinition(requireMember(definitions, "fixedVersion-1.0", "$.$defs"),
                              "$.$defs.fixedVersion-1.0", true);
    validateVersionDefinition(requireMember(definitions, "schemaVersion-1.0", "$.$defs"),
                              "$.$defs.schemaVersion-1.0", false, "#/$defs/positiveUnsigned32");

    requirePatternDefinition(definitions, "objectId", objectIdPattern);
    requirePatternDefinition(definitions, "allocatorHighWater", allocatorPattern);
    requirePatternDefinition(definitions, "positiveSigned64Decimal",
                             "^(?:" + std::string{positiveSigned64Body} + ")$");
    requirePatternDefinition(
        definitions, "signed64Decimal",
        "^(?:0|" + std::string{positiveSigned64Body} +
            "|-(?:[1-9][0-9]{0,17}|[1-8][0-9]{18}|9[0-1][0-9]{17}|92[0-1][0-9]{16}|"
            "922[0-2][0-9]{15}|9223[0-2][0-9]{14}|92233[0-6][0-9]{13}|"
            "922337[0-1][0-9]{12}|92233720[0-2][0-9]{10}|922337203[0-5][0-9]{9}|"
            "9223372036[0-7][0-9]{8}|92233720368[0-4][0-9]{7}|"
            "922337203685[0-3][0-9]{6}|9223372036854[0-6][0-9]{5}|"
            "92233720368547[0-6][0-9]{4}|922337203685477[0-4][0-9]{3}|"
            "9223372036854775[0-7][0-9]{2}|922337203685477580[0-8]))$");
    requirePatternDefinition(definitions, "positiveUnsigned32Decimal", positiveUnsigned32Pattern);
    validateTextDefinitions(definitions);
    validateRationals(definitions);
    validateColorDefinitions(definitions);

    validateSchemaInvariants(schema, "document schema");
    validateSchemaLock(schema, documentSchemaLock(), "document schema");
}

void checkProjectSchemas(const std::filesystem::path& repositoryRoot) {
    using namespace schema_detail;
    // The project format has exactly one schema pair; a second version of either never lands
    // beside it.
    const auto directory = repositoryRoot / "schemas/project";
    std::vector<std::string> entries;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        entries.push_back(entry.path().filename().generic_string());
    }
    std::ranges::sort(entries);
    if (entries !=
        std::vector<std::string>{"document-1.0.schema.json", "manifest-1.0.schema.json"}) {
        fail("schemas/project must hold exactly document-1.0.schema.json and "
             "manifest-1.0.schema.json");
    }
    const auto document = json::parseFile(directory / "document-1.0.schema.json");
    const auto manifest = json::parseFile(directory / "manifest-1.0.schema.json");
    validateDocumentSchema(document);
    validateManifestSchema(manifest);

    // The manifest's document version is the document's own.
    const auto& documentVersion = document.at("$defs").at("documentSchemaVersion").at("properties");
    const auto& manifestVersion = manifest.at("$defs").at("documentSchemaVersion").at("properties");
    if (!json::exactEqual(documentVersion, manifestVersion)) {
        fail("the manifest and document schemas must name the same document schema version");
    }
}

} // namespace bloom::quality
