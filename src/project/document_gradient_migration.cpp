// #541: opening a document older than 1.39 turns each schema 1 colour pair that makes up a ramp
// into a gradient value of two stops: a Linear or Radial Gradient's start and end colours, and a
// Glow's tint and outer tint. See docs/architecture/project-format.md, "Gradient Colour Pairs".

#include <bloom/project/document_decode.hpp>

#include <bloom/document/animation.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bloom::project {
namespace {

using document::AnimationComponent;
using document::KeyframeInterpolation;

constexpr std::array kColorComponents{AnimationComponent::Red, AnimationComponent::Green,
                                      AnimationComponent::Blue, AnimationComponent::Alpha};

[[nodiscard]] double& channel(core::Color4d& color, const std::size_t index) {
    return index == 0 ? color.red : index == 1 ? color.green : index == 2 ? color.blue : color.alpha;
}

// One colour of a pair as it was saved: a constant (or the fallback a driven colour starts at), or
// the component curves of a keyed colour.
struct PairColor final {
    core::Color4d constant;
    // A copy: the composition's curves change while the pair converts.
    std::optional<document::Color4AnimationCurve> curve;
    std::optional<document::AnimationCurveId> curveId;
};

[[nodiscard]] PairColor pairColor(const DecodedComposition& composition,
                                  const document::ParameterRecord* record,
                                  const core::Color4d fallback) {
    PairColor color{fallback, std::nullopt, std::nullopt};
    if (record == nullptr)
        return color;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source)) {
        if (const auto* value = std::get_if<core::Color4d>(&constant->value))
            color.constant = *value;
        return color;
    }
    const auto* animated = std::get_if<document::AnimationCurveSource>(&record->source);
    if (animated == nullptr)
        return color; // A driven colour has no value of its own here: it starts at its default.
    if (animated->defaultValue)
        if (const auto* value = std::get_if<core::Color4d>(&*animated->defaultValue))
            color.constant = *value;
    color.curveId = animated->curveId;
    for (const auto& held : composition.animationCurves)
        if (const auto* curve = std::get_if<document::Color4AnimationCurve>(&held);
            curve != nullptr && curve->id == animated->curveId)
            color.curve = *curve;
    return color;
}

[[nodiscard]] const document::ComponentAnimationCurve* component(const PairColor& color,
                                                                 const std::size_t index) {
    if (!color.curve)
        return nullptr;
    const auto* curve = color.curve->component(kColorComponents[index]);
    return curve != nullptr && !curve->keyframes.empty() ? curve : nullptr;
}

// The key a component's segment at `time` starts from (its last key at or before `time`), or
// nullptr before its first key.
[[nodiscard]] const document::ScalarKeyframe*
governingKey(const document::ComponentAnimationCurve& curve, const core::RationalTime time) {
    const document::ScalarKeyframe* governing = nullptr;
    for (const auto& key : curve.keyframes)
        if (!(time < key.time))
            governing = &key;
    return governing;
}

// A component's value at `time`: exact at its keys, held before the first and after the last and
// through a Hold segment, and linear inside any other segment (exact for Linear, the straight
// line through its keys for an eased one).
[[nodiscard]] double componentValue(const document::ComponentAnimationCurve& curve,
                                    const core::RationalTime time) {
    const auto& keys = curve.keyframes;
    if (!(keys.front().time < time))
        return keys.front().value;
    if (!(time < keys.back().time))
        return keys.back().value;
    std::size_t index = 0;
    while (index + 1 < keys.size() && !(time < keys[index + 1].time))
        ++index;
    const auto& from = keys[index];
    const auto& to = keys[index + 1];
    if (from.time == time || from.outgoingInterpolation == KeyframeInterpolation::Hold)
        return from.value;
    const double span = to.time.toSeconds() - from.time.toSeconds();
    const double u = span > 0.0 ? (time.toSeconds() - from.time.toSeconds()) / span : 0.0;
    return from.value + (to.value - from.value) * u;
}

[[nodiscard]] core::Color4d colorAt(const PairColor& color, const core::RationalTime time) {
    auto value = color.constant;
    for (std::size_t index = 0; index < kColorComponents.size(); ++index)
        if (const auto* curve = component(color, index))
            channel(value, index) = componentValue(*curve, time);
    value.alpha = std::clamp(value.alpha, 0.0, 1.0);
    return value;
}

[[nodiscard]] document::GradientValue pairGradient(const core::Color4d& start,
                                                   const core::Color4d& end) {
    return {{{0.0, start}, {1.0, end}}};
}

// The gradient key at `time` and the segment that follows it to `next`. A component whose own
// segment is exactly [time, next] lends the key its interpolation, easing and default handles when
// every such component agrees; Hold everywhere stays Hold; any other segment is Linear between the
// two keys' values.
void shapeSegment(document::GradientKeyframe& key, const std::array<PairColor, 2>& colors,
                  const core::RationalTime time, const std::optional<core::RationalTime> next) {
    key.outgoingInterpolation = KeyframeInterpolation::Linear;
    if (!next)
        return;
    std::optional<const document::ScalarKeyframe*> shared;
    bool agree = true;
    bool allHold = true;
    bool any = false;
    for (const auto& color : colors)
        for (std::size_t index = 0; index < kColorComponents.size(); ++index) {
            const auto* curve = component(color, index);
            if (curve == nullptr)
                continue;
            const auto* governing = governingKey(*curve, time);
            if (governing == nullptr || governing == &curve->keyframes.back())
                continue; // Constant across the segment.
            const auto* following = governing + 1;
            any = true;
            allHold = allHold && governing->outgoingInterpolation == KeyframeInterpolation::Hold;
            const bool whole = governing->time == time && following->time == *next &&
                               document::isDefaultKeyframeHandle(governing->outgoingHandle) &&
                               document::isDefaultKeyframeHandle(following->incomingHandle);
            if (!whole) {
                agree = false;
                continue;
            }
            if (!shared)
                shared = governing;
            else if ((*shared)->outgoingInterpolation != governing->outgoingInterpolation ||
                     !((*shared)->easing == governing->easing))
                agree = false;
        }
    if (!any)
        return;
    if (allHold) {
        key.outgoingInterpolation = KeyframeInterpolation::Hold;
        return;
    }
    if (agree && shared) {
        key.outgoingInterpolation = (*shared)->outgoingInterpolation;
        key.easing = (*shared)->easing;
    }
}

// The pair as one gradient: a constant when neither colour has keys, otherwise a whole-value curve
// keyed at every time either colour has a key, on `curveId`.
[[nodiscard]] std::optional<document::ParameterSource>
convertPair(DecodedComposition& composition, const std::array<PairColor, 2>& colors,
            const std::optional<document::AnimationCurveId> curveId,
            document::IdAllocator& ids) {
    std::set<core::RationalTime> times;
    for (const auto& color : colors)
        for (std::size_t index = 0; index < kColorComponents.size(); ++index)
            if (const auto* curve = component(color, index))
                for (const auto& key : curve->keyframes)
                    times.insert(key.time);
    const auto constant = pairGradient(colors[0].constant, colors[1].constant);
    if (times.empty() || !curveId)
        return document::ConstantValueSource{constant};
    document::GradientAnimationCurve curve;
    curve.id = *curveId;
    for (auto at = times.begin(); at != times.end(); ++at) {
        const auto keyId = ids.allocateKeyframe();
        if (!keyId)
            return std::nullopt;
        document::GradientKeyframe key;
        key.id = *keyId;
        key.time = *at;
        key.value = pairGradient(colorAt(colors[0], *at), colorAt(colors[1], *at));
        const auto next = std::next(at);
        shapeSegment(key, colors, *at,
                     next == times.end() ? std::nullopt : std::optional(*next));
        curve.keyframes.push_back(std::move(key));
    }
    composition.animationCurves.emplace_back(std::move(curve));
    return document::AnimationCurveSource{*curveId, document::ParameterValue{constant}};
}

[[nodiscard]] document::ParameterRecord* findRecord(DecodedComposition& composition,
                                                    const document::NodeRecord& node,
                                                    const std::string_view role) {
    const auto binding = std::ranges::find(node.parameters, role, &document::ParameterBinding::role);
    if (binding == node.parameters.end())
        return nullptr;
    const auto record = std::ranges::find(composition.parameters, binding->parameterId,
                                          &document::ParameterRecord::id);
    return record == composition.parameters.end() ? nullptr : &*record;
}

// One node: `first` and `second` (by role) become one gradient record bound as `target`. The
// gradient reuses the first record that `reuse` names, or a new id; the pair's other record, its
// colour curves and the edges into a removed role are dropped. Returns false (the node left as
// saved, so it fails as an unsupported version) when a required role is missing or ids run out.
bool migrateNode(DecodedComposition& composition, document::NodeRecord& node,
                 const std::string_view first, const core::Color4d firstFallback,
                 const std::string_view second, const core::Color4d secondFallback,
                 const bool keepFirst, const std::string_view target,
                 const std::string_view targetSchemaKey, const std::uint32_t schemaVersion,
                 document::IdAllocator& ids) {
    auto* firstRecord = findRecord(composition, node, first);
    auto* secondRecord = findRecord(composition, node, second);
    if (firstRecord == nullptr)
        return false;
    const std::array colors{pairColor(composition, firstRecord, firstFallback),
                            pairColor(composition, secondRecord, secondFallback)};
    // The gradient record: the first colour's own unless it stays (Glow keeps its tint), then the
    // second's, then a new one.
    std::optional<document::ParameterId> id;
    if (!keepFirst)
        id = firstRecord->id;
    else if (secondRecord != nullptr)
        id = secondRecord->id;
    else
        id = ids.allocateParameter();
    if (!id)
        return false;
    // The gradient curve takes the first keyed colour's curve id that is being retired.
    std::set<document::AnimationCurveId> retired;
    if (!keepFirst && colors[0].curveId)
        retired.insert(*colors[0].curveId);
    if (colors[1].curveId)
        retired.insert(*colors[1].curveId);
    std::optional<document::AnimationCurveId> curveId;
    if (!retired.empty())
        curveId = *retired.begin();
    else if (colors[0].curve)
        curveId = ids.allocateAnimationCurve();
    std::erase_if(composition.animationCurves, [&](const auto& curve) {
        return retired.contains(std::visit([](const auto& held) { return held.id; }, curve));
    });
    auto source = convertPair(composition, colors, curveId, ids);
    if (!source)
        return false;
    std::set<document::ParameterId> removed;
    if (!keepFirst && secondRecord != nullptr)
        removed.insert(secondRecord->id);
    std::erase_if(composition.parameters,
                  [&](const auto& record) { return removed.contains(record.id); });
    if (const auto record =
            std::ranges::find(composition.parameters, *id, &document::ParameterRecord::id);
        record != composition.parameters.end()) {
        record->schemaKey = std::string(targetSchemaKey);
        record->source = std::move(*source);
    } else {
        composition.parameters.push_back({*id, std::string(targetSchemaKey), std::move(*source)});
    }
    std::erase_if(node.parameters, [&](const document::ParameterBinding& binding) {
        return (!keepFirst && binding.role == first) || binding.role == second;
    });
    node.parameters.push_back({std::string(target), *id});
    std::ranges::sort(node.parameters, {}, &document::ParameterBinding::role);
    std::erase_if(composition.graph.edges, [&](const document::EdgeRecord& edge) {
        const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
        return input != nullptr && input->nodeId == node.id &&
               ((!keepFirst && input->port == first) || input->port == second);
    });
    node.schemaVersion = schemaVersion;
    return true;
}

} // namespace

bool isLegacyGradientColorPairNode(const std::string_view typeId,
                                   const std::uint32_t schemaVersion) noexcept {
    return ((typeId == document::kLinearGradientNodeType ||
             typeId == document::kRadialGradientNodeType) &&
            schemaVersion == document::kLegacyGradientNodeSchemaVersion) ||
           (typeId == document::kGlowNodeType &&
            schemaVersion == document::kLegacyGlowNodeSchemaVersion);
}

void migrateLegacyGradientColorPairs(DecodedDocumentEnvelope& envelope) {
    auto ids = document::IdAllocator::fromHighWater(envelope.highWater);
    for (auto& composition : envelope.compositions)
        for (auto& node : composition.graph.nodes) {
            if (!isLegacyGradientColorPairNode(node.typeId, node.schemaVersion))
                continue;
            if (node.typeId == document::kGlowNodeType)
                static_cast<void>(migrateNode(
                    composition, node, "tint", core::Color4d{1.0, 1.0, 1.0, 1.0}, "outerTint",
                    core::Color4d{1.0, 0.5, 0.25, 1.0}, true, "tintGradient",
                    document::kGlowTintGradientSchemaKey, document::kGlowNodeSchemaVersion, ids));
            else
                static_cast<void>(migrateNode(
                    composition, node, "startColor", core::Color4d{0.0, 0.0, 0.0, 1.0},
                    "endColor", core::Color4d{1.0, 1.0, 1.0, 1.0}, false, "gradient",
                    document::kGradientStopsParameterSchemaKey,
                    document::kGradientNodeSchemaVersion, ids));
        }
    envelope.highWater.parameter = ids.highWater().parameter;
    envelope.highWater.animationCurve = ids.highWater().animationCurve;
    envelope.highWater.keyframe = ids.highWater().keyframe;
}

} // namespace bloom::project
