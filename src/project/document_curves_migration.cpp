// #493: opening a project migrates every Curves node of schema 1 (five Vector 2 points per channel)
// to schema 2 (one point-list curve per channel and four hue curves). See
// docs/architecture/project-format.md, "Curves Schema 2".

#include <bloom/project/document_decode.hpp>

#include <bloom/document/animation.hpp>
#include <bloom/document/grade_curves.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bloom::project {
namespace {

using document::Vec2d;

// The value a schema 1 point had where its own keys begin: a constant as stored; an animated point
// its first key per component (or the curve's default, then the identity default, for a component
// with no key); a driven point the identity default its node started with.
[[nodiscard]] Vec2d legacyPointValue(const DecodedComposition& composition,
                                     const document::ParameterRecord& record,
                                     const std::size_t point,
                                     std::set<document::AnimationCurveId>& retired) {
    const double fallback = document::defaultCurvePointCoordinate(point);
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&record.source)) {
        if (const auto* value = std::get_if<Vec2d>(&constant->value))
            return *value;
        return {fallback, fallback};
    }
    const auto* animated = std::get_if<document::AnimationCurveSource>(&record.source);
    if (animated == nullptr)
        return {fallback, fallback};
    Vec2d value{fallback, fallback};
    if (animated->defaultValue)
        if (const auto* stored = std::get_if<Vec2d>(&*animated->defaultValue))
            value = *stored;
    retired.insert(animated->curveId);
    const auto curve = std::ranges::find_if(composition.animationCurves, [&](const auto& item) {
        return std::visit([](const auto& held) { return held.id; }, item) == animated->curveId;
    });
    if (curve == composition.animationCurves.end())
        return value;
    if (const auto* vector = std::get_if<document::Vec2AnimationCurve>(&*curve)) {
        if (const auto* x = vector->component(document::AnimationComponent::X);
            x != nullptr && !x->keyframes.empty())
            value.x = x->keyframes.front().value;
        if (const auto* y = vector->component(document::AnimationComponent::Y);
            y != nullptr && !y->keyframes.empty())
            value.y = y->keyframes.front().value;
    }
    return value;
}

// One schema 1 node. Each channel's curve takes its first point's parameter id; the hue curves take
// the master channel's second to fifth point ids (empty, unchanged), so no id is allocated. The
// other points' records and their animation curves are removed.
void migrateNode(DecodedComposition& composition, document::NodeRecord& node) {
    const auto findRecord = [&](const std::string_view role) -> document::ParameterRecord* {
        const auto binding =
            std::ranges::find(node.parameters, role, &document::ParameterBinding::role);
        if (binding == node.parameters.end())
            return nullptr;
        const auto record = std::ranges::find(composition.parameters, binding->parameterId,
                                              &document::ParameterRecord::id);
        return record == composition.parameters.end() ? nullptr : &*record;
    };
    std::set<document::AnimationCurveId> retired;
    std::array<std::optional<document::PathValue>, document::kGradeChannelCount> curves;
    std::array<std::optional<document::ParameterId>, document::kGradeCurveCount> ids;
    std::set<document::ParameterId> removed;
    for (std::size_t channel = 0; channel < document::kGradeChannelCount; ++channel) {
        std::vector<Vec2d> points;
        for (std::size_t point = 0; point < document::kLegacyCurvePointCount; ++point) {
            const auto* record = findRecord(document::curvePointRole(channel, point));
            if (record == nullptr)
                return;
            points.push_back(legacyPointValue(composition, *record, point, retired));
            removed.insert(record->id);
            if (point == 0)
                ids[channel] = record->id;
            else if (channel == 0)
                ids[document::kGradeChannelCount + point - 1] = record->id;
        }
        curves[channel] = document::gradeCurvePath(points);
    }
    std::vector<document::ParameterBinding> bindings;
    for (const auto& binding : node.parameters)
        if (!binding.role.ends_with("Point1") && !binding.role.ends_with("Point2") &&
            !binding.role.ends_with("Point3") && !binding.role.ends_with("Point4") &&
            !binding.role.ends_with("Point5"))
            bindings.push_back(binding);
    for (std::size_t index = 0; index < document::kGradeCurveCount; ++index) {
        const auto curve = document::kGradeCurves[index];
        const auto id = *ids[index];
        removed.erase(id);
        auto record = std::ranges::find(composition.parameters, id, &document::ParameterRecord::id);
        record->schemaKey = std::string(document::gradeCurveSchemaKey(curve));
        record->source = document::ConstantValueSource{
            index < document::kGradeChannelCount ? *curves[index]
                                                 : document::defaultGradeCurve(curve)};
        bindings.push_back({std::string(document::gradeCurveRole(curve)), id});
    }
    std::ranges::sort(bindings, {}, &document::ParameterBinding::role);
    node.parameters = std::move(bindings);
    node.schemaVersion = document::kCurvesNodeSchemaVersion;
    std::erase_if(composition.parameters,
                  [&](const auto& record) { return removed.contains(record.id); });
    std::erase_if(composition.animationCurves, [&](const auto& curve) {
        return retired.contains(std::visit([](const auto& held) { return held.id; }, curve));
    });
}

} // namespace

void migrateLegacyCurvesNodes(DecodedDocumentEnvelope& envelope) {
    for (auto& composition : envelope.compositions)
        for (auto& node : composition.graph.nodes)
            if (node.typeId == document::kCurvesNodeType &&
                node.schemaVersion == document::kLegacyCurvesNodeSchemaVersion)
                migrateNode(composition, node);
}

} // namespace bloom::project
