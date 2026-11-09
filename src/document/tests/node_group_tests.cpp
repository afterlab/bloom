#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/node_groups.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

using namespace bloom::document;

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// The guard #554 asks for: a node that lands without a group is a failure here, not a node nobody
// can find. Its name goes in its group's list in node_groups.cpp.
void everyRegisteredNodeHasAGroup() {
    std::set<std::string_view> registered;
    for (const auto& definition : builtInNodeDefinitions().definitions()) {
        registered.insert(definition.key.typeId);
        expect(nodeGroupOf(definition.key.typeId).has_value(),
               definition.key.typeId +
                   " has no group: add it to its group in src/document/node_groups.cpp");
    }
    std::set<std::string_view> listed;
    for (const auto group : kNodeGroups)
        for (const auto typeId : nodeGroupMembers(group)) {
            expect(listed.insert(typeId).second,
                   std::string(typeId) + " is listed in more than one group");
            expect(registered.contains(typeId),
                   std::string(typeId) + " is listed in a group but is not a registered node");
            expect(nodeGroupOf(typeId) == group, std::string(typeId) + " reads back another group");
        }
}

void everyGroupIsNamed() {
    std::set<std::string_view> names;
    std::set<std::string_view> wireNames;
    for (const auto group : kNodeGroups) {
        expect(!nodeGroupName(group).empty() && names.insert(nodeGroupName(group)).second,
               "group " + std::to_string(static_cast<int>(group)) + " has a name of its own");
        expect(!nodeGroupWireName(group).empty() &&
                   wireNames.insert(nodeGroupWireName(group)).second,
               "group " + std::to_string(static_cast<int>(group)) + " has a wire name of its own");
    }
    int valueGroups = 0;
    for (const auto group : kNodeGroups)
        valueGroups += isValueSubgroup(group) ? 1 : 0;
    expect(valueGroups == 8, "eight value groups sit under the Values submenu");
    expect(!nodeGroupOf("bloom.no-such-node").has_value(), "an unknown type has no group");
}

// The group is presentation. Grouping must not reach the effect mask, which is part of the node's
// schema and so of every saved project: these nodes moved out of Filters and Compositing without
// gaining or losing the mask input they have always had.
void movingAGroupLeavesTheEffectMaskAlone() {
    const auto& registry = builtInNodeDefinitions();
    const auto masked = [&registry](const std::string_view typeId) {
        for (const auto& definition : registry.definitions())
            if (definition.key.typeId == typeId)
                return hasEffectMask(definition);
        expect(false, std::string(typeId) + " is registered");
        return false;
    };
    for (const auto typeId : {"bloom.vignette", "bloom.film-grain", "bloom.depth-fog", "bloom.echo", "bloom.frame-rate-hold",
                              "bloom.motion-blur", "bloom.z-defocus", "bloom.colorize", "bloom.sharpen", "bloom.codec-degrade", "bloom.auto-exposure",
                              "bloom.tone-map", "bloom.color-matrix"})
        expect(masked(typeId), std::string(typeId) + " keeps its effect mask");
    for (const auto typeId : {"bloom.lens-flare", "bloom.drop-shadow", "bloom.light-wrap",
                              "bloom.luma-key", "bloom.channel-shuffle", "bloom.alpha-association",
                              "bloom.lens-distortion", "bloom.chromatic-aberration",
                              "bloom.rolling-shutter", "bloom.test-card", "bloom.corner-pin",
                              "bloom.st-map", "bloom.displace"})
        expect(!masked(typeId), std::string(typeId) + " still takes no effect mask");
}

} // namespace

int main() {
    everyRegisteredNodeHasAGroup();
    everyGroupIsNamed();
    movingAGroupLeavesTheEffectMaskAlone();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
