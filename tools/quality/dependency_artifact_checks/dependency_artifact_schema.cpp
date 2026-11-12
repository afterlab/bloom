#include "dependency_artifact_checks_internal.hpp"

#include "production_lock_checks.hpp"

#include <algorithm>
#include <array>

#include <functional>

#include <tuple>

#include <system_error>

namespace bloom::quality::dependencies {

using namespace detail;

auto checkRepository(const Path& inputRoot) -> CheckResult {
    std::error_code error;
    const auto root = std::filesystem::canonical(inputRoot, error);
    if (error) {
        fail("repository-root", inputRoot.string(), error.message());
    }
    const std::array schemaArtifacts{
        std::tuple{ArtifactKind::Lock, "dependency-lock-1.0.schema.json",
                   "a04c2bc037845234ac5232e79e6491e47bd889c34e4c8e77507549531d6afd3e"},
        std::tuple{ArtifactKind::LockV1_1, "dependency-lock-1.1.schema.json",
                   "d2048f3738012d8ce95ae1d681e6bf4f14f3b13b92e16affd4a10a890e8d6111"},
        std::tuple{ArtifactKind::LockV1_2, "dependency-lock-1.2.schema.json",
                   "4ba0732225df759b0cd579b28d1a70316c4fc243405311d28434c8d55c0fb174"},
        std::tuple{ArtifactKind::LockV1_3, "dependency-lock-1.3.schema.json",
                   "79f04697e1fb1322a6ef5f1e916fe705892778aa7205be7b3d3c71df35a6a22e"},
        std::tuple{ArtifactKind::LockV1_4, "dependency-lock-1.4.schema.json",
                   "5a64fd07078bc17e4b21cefe76a83f276da4921fd79e523ea2e0b313b0cc0f10"},
        std::tuple{ArtifactKind::Prefix, "prefix-manifest-1.0.schema.json",
                   "42d48bfadb647e295dd155a82ff3e873ffa63b7140615e6c122672fc7397046c"}};
    for (const auto& [kind, name, expectedDigest] : schemaArtifacts) {
        const auto schema = loadSchemaArtifact(root / Path{kSchemaDirectory} / name);
        if (digestHex(schema.encoded) != expectedDigest) {
            fail("schema-bytes", name, "readable schema bytes differ from frozen artifact");
        }
        validateSchemaArtifact(schema.value, kind);
    }
    const auto context = makeFixtureContext(root);
    const auto lockEncoded = readBounded(root / Path{kFixtureDirectory} / "valid-lock.json",
                                         limitsFor(ArtifactKind::Lock).maximumBytes);
    const auto lock = parseCanonicalFixture(lockEncoded, ArtifactKind::Lock);
    validateLockFixture(lock, context);
    const auto prefixEncoded =
        readBounded(root / Path{kFixtureDirectory} / "valid-prefix-manifest.json",
                    limitsFor(ArtifactKind::Prefix).maximumBytes);
    const auto prefix = parseCanonicalFixture(prefixEncoded, ArtifactKind::Prefix);
    validatePrefixFixture(prefix, lock, lockEncoded, context);
    const auto production = validateProductionLock(root);
    return {.lockVector = identityVector("bloom.dependencies.lock.v1", lockEncoded),
            .prefixVector = identityVector("bloom.dependencies.prefix.v1", prefixEncoded),
            .productionLockPresent = production.present,
            .productionLockIdentity = production.identity};
}
void validateSchemaArtifact(const Value& value, const ArtifactKind kind) {
    constexpr std::string_view draft = "https://json-schema.org/draft/2020-12/schema";
    const auto* expectedId =
        kind == ArtifactKind::Lock       ? "urn:afterlab:bloom:schema:dependency-lock:1.0"
        : kind == ArtifactKind::LockV1_1 ? "urn:afterlab:bloom:schema:dependency-lock:1.1"
        : kind == ArtifactKind::LockV1_2 ? "urn:afterlab:bloom:schema:dependency-lock:1.2"
        : kind == ArtifactKind::LockV1_3 ? "urn:afterlab:bloom:schema:dependency-lock:1.3"
        : kind == ArtifactKind::LockV1_4
            ? "urn:afterlab:bloom:schema:dependency-lock:1.4"
            : "urn:afterlab:bloom:schema:dependency-prefix-manifest:1.0";
    const auto* schema = value.find("$schema");
    const auto* id = value.find("$id");
    if (schema == nullptr || id == nullptr || !schema->isString() || !id->isString() ||
        schema->asString() != draft || id->asString() != expectedId) {
        fail("schema-identity", "$", "expected the frozen Draft 2020-12 schema identity");
    }

    std::function<void(const Value&, const std::string&)> visit;
    visit = [&](const Value& node, const std::string& location) {
        if (node.isArray()) {
            for (std::size_t index = 0; index < node.asArray().size(); ++index) {
                visit(node.asArray()[index], location + '[' + std::to_string(index) + ']');
            }
            return;
        }
        if (!node.isObject()) {
            return;
        }
        if (const auto* reference = node.find("$ref"); reference != nullptr) {
            if (!reference->isString()) {
                fail("schema-ref", location + ".$ref", "reference must be a string");
            }
            const auto& text = reference->asString();
            const bool allowed =
                text.starts_with("#/") || (kind == ArtifactKind::Prefix &&
                                           (text.starts_with("dependency-lock-1.0.schema.json#/") ||
                                            text.starts_with("dependency-lock-1.1.schema.json#/")));
            if (!allowed) {
                fail("schema-ref", location + ".$ref", "reference is not repository-local");
            }
        }
        const auto* type = node.find("type");
        if (type != nullptr && type->isString() && type->asString() == "object") {
            const auto* properties = node.find("properties");
            const auto* required = node.find("required");
            const auto* closed = node.find("unevaluatedProperties");
            if (properties == nullptr || required == nullptr || closed == nullptr ||
                !properties->isObject() || !required->isArray() || !closed->isBoolean() ||
                closed->asBoolean()) {
                fail("schema-object", location,
                     "object properties, required order, and closure differ from contract");
            }
            std::size_t requiredIndex = 0;
            for (const auto& [propertyName, child] : properties->asObject()) {
                static_cast<void>(child);
                const auto requiredName = requiredIndex < required->asArray().size()
                                              ? required->asArray()[requiredIndex]
                                              : Value{};
                if (requiredName.isString() && requiredName.asString() == propertyName) {
                    ++requiredIndex;
                    continue;
                }
                const auto isRequired = std::ranges::any_of(
                    required->asArray(), [&propertyName](const Value& candidate) {
                        return candidate.isString() && candidate.asString() == propertyName;
                    });
                if (isRequired) {
                    fail("schema-object", location,
                         "required members must follow property declaration order");
                }
            }
            if (requiredIndex != required->asArray().size()) {
                fail("schema-object", location,
                     "required members must be declared by the property object");
            }
        }
        for (const auto& [name, child] : node.asObject()) {
            auto childLocation = location;
            childLocation.push_back('.');
            childLocation += name;
            visit(child, childLocation);
        }
    };
    visit(value, "$");

    const auto* expectedValueDigest =
        kind == ArtifactKind::Lock
            ? "2a6d32da79c344d889499e5db156a55c990a3a23da321419586714e006df5198"
        : kind == ArtifactKind::LockV1_1
            ? "f062116be3712cbfb7f80016926d255289af68ea6ddfb3060a6e930802d99d29"
        : kind == ArtifactKind::LockV1_2
            ? "1feacbc8a91dd223568a9d4f3a011cc2a6106cfc909a8071a4be5a4ef37ef9dc"
        : kind == ArtifactKind::LockV1_3
            ? "609e687993afa705f4629a41c58d6852ccbb64618854613cc21712f33c106437"
        : kind == ArtifactKind::LockV1_4
            ? "02248b9c5eaf9135c6f627ef4df2436f4b7b42b813370557274b7031be1a48a9"
            : "cbe233295e066bc45c77fc71fa2c26aeedd2f2e5a932b8840bb23b7670533dce";
    if (digestHex(encodeCanonical(value)) != expectedValueDigest) {
        fail("schema-contract", "$", "schema value differs from the frozen artifact");
    }
}
} // namespace bloom::quality::dependencies
