#include "dependency_artifact_checks_internal.hpp"

#include "production_lock_checks.hpp"

#include <algorithm>
#include <array>

#include <functional>

#include <tuple>

#include <system_error>

namespace bloom::quality::dependencies {

using namespace detail;

// One lock schema and one prefix-manifest schema, each at 1.0. No other file may sit beside them,
// so a second schema version cannot come back unnoticed.
void verifySchemaSet(const Path& root) {
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(root / Path{kSchemaDirectory}, error)) {
        const auto name = entry.path().filename().string();
        if (name != "dependency-lock-1.0.schema.json" &&
            name != "prefix-manifest-1.0.schema.json") {
            fail("schema-set", name,
                 "the schema directory holds exactly the lock and prefix-manifest 1.0 schemas");
        }
    }
    if (error) {
        fail("schema-set", (root / Path{kSchemaDirectory}).string(), error.message());
    }
}

auto checkRepository(const Path& inputRoot) -> CheckResult {
    std::error_code error;
    const auto root = std::filesystem::canonical(inputRoot, error);
    if (error) {
        fail("repository-root", inputRoot.string(), error.message());
    }
    const std::array schemaArtifacts{
        std::tuple{ArtifactKind::Lock, "dependency-lock-1.0.schema.json",
                   "225504d96c57b1ec7fb3c2dc2de52ead52186d63ce6845c321c189a07cb77ee4"},
        std::tuple{ArtifactKind::Prefix, "prefix-manifest-1.0.schema.json",
                   "42d48bfadb647e295dd155a82ff3e873ffa63b7140615e6c122672fc7397046c"}};
    verifySchemaSet(root);
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
    const auto* expectedId = kind == ArtifactKind::Lock
                                 ? "urn:afterlab:bloom:schema:dependency-lock:1.0"
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
                                           text.starts_with("dependency-lock-1.0.schema.json#/"));
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
            ? "3b374c8918a0e0578b8d5a871dd995c64e2c0bc3d026709e02883f34e60010f9"
            : "cbe233295e066bc45c77fc71fa2c26aeedd2f2e5a932b8840bb23b7670533dce";
    if (digestHex(encodeCanonical(value)) != expectedValueDigest) {
        fail("schema-contract", "$", "schema value differs from the frozen artifact");
    }
}
} // namespace bloom::quality::dependencies
