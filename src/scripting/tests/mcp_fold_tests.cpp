// MCP usability fixes: ids as strings, name and role references, set-keys, key-range warnings,
// duplicate composition id maps, asset import and export destinations, and error wording.
#include <bloom/commands/operations.hpp>
#include <bloom/document/project.hpp>
#include <bloom/scripting/operation_registry.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/scripting/value.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

using bloom::scripting::Arguments;
using bloom::scripting::ObjectReference;
using bloom::scripting::Value;
using bloom::scripting::ValueArray;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

int main() {
    auto created = bloom::scripting::Session::createNew();
    if (!created) {
        std::cerr << "session creation failed\n";
        return 1;
    }
    auto session = std::move(created).takeSession();
    const auto composition = session->snapshot().project().compositions().front().id().value();
    const Value compositionId{bloom::scripting::StableId{composition}};

    const auto solid = [&](const std::string& name) {
        const auto added = session->executeJsonOperation(
            "bloom.layer.add-solid",
            {{"composition", compositionId},
             {"name", Value{name}},
             {"color", Value{bloom::core::Color4d{1.0, 0.0, 0.0, 1.0}}}});
        expect(added.succeeded(), "add-solid " + name);
        return added.succeeded()
                   ? added.command->outputId<bloom::document::LayerId>("layer")->value()
                   : 0U;
    };
    const auto red = solid("Red");
    const auto green = solid("Green");
    const auto green2 = solid("Green");
    expect(red != 0U && green != 0U && green2 != 0U, "three layers");

    const auto layerName = [&](const std::uint64_t layer) {
        const auto* found = session->snapshot()
                                .project()
                                .findComposition(bloom::document::CompositionId::fromRaw(composition))
                                ->graph()
                                .findLayer(bloom::document::LayerId::fromRaw(layer));
        return found == nullptr ? std::string{} : found->name;
    };

    // #577: an id may be a decimal string.
    const auto byString = session->executeJsonOperation(
        "bloom.layer.rename", {{"composition", Value{std::to_string(composition)}},
                               {"layer", Value{std::to_string(red)}},
                               {"name", Value{"Crimson"}}});
    expect(byString.succeeded() && layerName(red) == "Crimson", "decimal string ids are accepted");

    // #574: a layer by name, and the refusals list candidates.
    ObjectReference byName;
    byName.name = "Crimson";
    const auto renamed = session->executeJsonOperation(
        "bloom.layer.rename",
        {{"composition", compositionId}, {"layer", Value{byName}}, {"name", Value{"Scarlet"}}});
    expect(renamed.succeeded() && layerName(red) == "Scarlet", "layer by name");
    ObjectReference ambiguous;
    ambiguous.name = "Green";
    const auto twice = session->executeJsonOperation(
        "bloom.layer.rename",
        {{"composition", compositionId}, {"layer", Value{ambiguous}}, {"name", Value{"X"}}});
    expect(!twice.succeeded() && twice.diagnostic &&
               contains(twice.diagnostic->message, "2 layers are named \"Green\"") &&
               contains(twice.diagnostic->message, std::to_string(green)),
           "ambiguous name lists the matches");
    ObjectReference missing;
    missing.name = "Nope";
    const auto none = session->executeJsonOperation(
        "bloom.layer.rename",
        {{"composition", compositionId}, {"layer", Value{missing}}, {"name", Value{"X"}}});
    expect(!none.succeeded() && none.diagnostic &&
               contains(none.diagnostic->message, "its layers are"),
           "unknown name lists the layers");

    // #576 and #571: set-keys is one undo step; a key past the layer is warned about.
    ObjectReference opacity;
    opacity.layerName = "Scarlet";
    opacity.role = "opacity";
    const auto keyed = session->executeJsonOperation(
        "bloom.animation.set-keys",
        {{"composition", compositionId},
         {"parameter", Value{opacity}},
         {"keys", Value{ValueArray{Value{ValueArray{Value{std::int64_t{0}}, Value{0.0}}},
                                    Value{ValueArray{Value{std::int64_t{12}}, Value{1.0},
                                                     Value{"hold"}}}}}}});
    expect(keyed.succeeded(), "set-keys by role");
    const auto revisionAfterKeys = session->snapshot().revision();
    const auto undone = session->undo();
    expect(undone.succeeded() && session->snapshot().revision() != revisionAfterKeys,
           "set-keys undoes in one step");
    const auto redone = session->redo();
    expect(redone.succeeded(), "set-keys redoes");
    const auto replaced = session->executeJsonOperation(
        "bloom.animation.set-keys",
        {{"composition", compositionId},
         {"parameter", Value{opacity}},
         {"keys", Value{ValueArray{}}},
         {"replace", Value{true}}});
    expect(replaced.succeeded(), "replace with no keys returns the parameter to a constant");
    const auto noReplace = session->executeJsonOperation(
        "bloom.animation.set-keys",
        {{"composition", compositionId},
         {"parameter", Value{opacity}},
         {"keys", Value{ValueArray{}}}});
    expect(!noReplace.succeeded(), "an empty key list needs replace");

    const auto late = session->executeJsonOperation(
        "bloom.animation.set-keys",
        {{"composition", compositionId},
         {"parameter", Value{opacity}},
         {"keys", Value{ValueArray{Value{ValueArray{Value{std::int64_t{100000}}, Value{0.5}}}}}},
         {"replace", Value{true}}});
    expect(late.succeeded() && late.command && !late.command->warnings.empty() &&
               contains(late.command->warnings.front().issue.message, "outside"),
           "a key outside the layer is warned about");

    // #573: duplicating a composition returns old-to-new id maps.
    const auto copy = session->executeJsonOperation("bloom.composition.duplicate",
                                                    {{"composition", compositionId}});
    expect(copy.succeeded(), "duplicate composition");
    if (copy.succeeded()) {
        bool layerMapped = false;
        for (const auto& item : copy.command->outputs)
            layerMapped = layerMapped ||
                          item.output.name == "layer." + std::to_string(red);
        expect(layerMapped, "the duplicate maps the original layer to its copy");
    }

    // #577: a relative import path without a saved project names the cause.
    const auto relative = session->executeJsonOperation(
        "bloom.asset.import", {{"paths", Value{ValueArray{Value{"clip.exr"}}}}});
    expect(!relative.succeeded(), "relative import without a project folder is refused");

    // #578: errors name the object.
    const auto badLayer = session->executeJsonOperation(
        "bloom.layer.rename", {{"composition", compositionId},
                               {"layer", Value{std::int64_t{999999}}},
                               {"name", Value{"X"}}});
    const auto failure = badLayer.command && !badLayer.command->operationFailures.empty()
                             ? badLayer.command->operationFailures.front().issue.message
                             : std::string{};
    expect(contains(failure, "999999"), "a missing layer is named in the error: " + failure);

    // A locked layer's refusal names the layer.
    const auto locked = session->executeJsonOperation(
        "bloom.layer.set-locked",
        {{"composition", compositionId}, {"layer", Value{std::to_string(green)}},
         {"locked", Value{true}}});
    expect(locked.succeeded(), "lock a layer");
    ObjectReference lockedOpacity;
    lockedOpacity.layerId = green;
    lockedOpacity.role = "opacity";
    const auto edit = session->executeJsonOperation(
        "bloom.animation.set-keys",
        {{"composition", compositionId},
         {"parameter", Value{lockedOpacity}},
         {"keys", Value{ValueArray{Value{ValueArray{Value{std::int64_t{0}}, Value{0.5}}}}}}});
    const auto why = edit.command && !edit.command->operationFailures.empty()
                         ? edit.command->operationFailures.front().issue.message
                         : edit.command ? std::string("(no failure)") : std::string("(no command)");
    expect(!edit.succeeded() && contains(why, "is locked") && contains(why, "composition 1"),
           "a locked refusal says what and where: " + why);

    return failures == 0 ? 0 : 1;
}
