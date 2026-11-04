// bloom.parameter.set-source: repoints one parameter at a constant, a curve or a driver. A value a
// script client sends is read in the parameter's own declared kind (a whole number for an Integer
// or enumeration, a number for a Float64, a member name for an enumeration), and every refusal
// names the parameter, the node that owns it and what it accepts, so an artist or an agent never
// has to guess why an edit was refused. A text font takes one value form only: the id of a Font
// asset in this project, as a decimal string. Any other value is refused here, with the project's
// Font assets listed, instead of being stored and silently rendered in the fallback face. Replacing
// an animated source retires the parameter's animation curve in the same edit.
#include <bloom/commands/operations.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::commands {
namespace {

struct ParameterOwner final {
    const document::NodeRecord* node = nullptr;
    const document::NodeDefinition* definition = nullptr;
    const document::ParameterDefinition* parameter = nullptr;
    std::string role;
    // #502: the Merge entry's own layer that owns the parameter, when no node does.
    const document::LayerOutputBoundary* entryLayer = nullptr;
};

ParameterOwner ownerOf(const document::Composition& composition, const document::ParameterId id) {
    // An entry layer binds its parameters by the Layer node's roles and schema keys, so the Layer
    // node definition declares them.
    for (const auto& layer : composition.graph().layerOutputs()) {
        const auto binding =
            std::ranges::find(layer.parameters, id, &document::ParameterBinding::parameterId);
        if (binding == layer.parameters.end())
            continue;
        ParameterOwner owner{.role = binding->role, .entryLayer = &layer};
        owner.definition = document::builtInNodeDefinitions().find(
            document::kLayerOutputNodeType, document::kLayerOutputNodeSchemaVersion);
        if (owner.definition != nullptr) {
            const auto match = std::ranges::find(owner.definition->parameters, binding->role,
                                                 &document::ParameterDefinition::role);
            if (match != owner.definition->parameters.end())
                owner.parameter = &*match;
        }
        return owner;
    }
    for (const auto& node : composition.graph().nodes()) {
        const auto binding =
            std::ranges::find(node.parameters, id, &document::ParameterBinding::parameterId);
        if (binding == node.parameters.end())
            continue;
        ParameterOwner owner{.node = &node, .role = binding->role};
        owner.definition = document::builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
        if (owner.definition != nullptr) {
            const auto match = std::ranges::find(owner.definition->parameters, binding->role,
                                                 &document::ParameterDefinition::role);
            if (match != owner.definition->parameters.end())
                owner.parameter = &*match;
        }
        return owner;
    }
    return {};
}

std::string describe(const document::ParameterRecord& record, const ParameterOwner& owner) {
    auto text = "Parameter " + std::to_string(record.id.value());
    if (owner.node != nullptr)
        text += " (" + owner.role + " of node " + std::to_string(owner.node->id.value()) + ", " +
                owner.node->typeId + ")";
    else if (owner.entryLayer != nullptr)
        text += " (" + owner.role + " of the Merge entry layer " +
                std::to_string(owner.entryLayer->layerId.value()) + ")";
    return text;
}

std::string formatNumber(const double value) {
    if (std::floor(value) == value && std::abs(value) < 1e15)
        return std::to_string(static_cast<long long>(value));
    auto text = std::to_string(value);
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    return text;
}

// The Font asset a text font value names: a decimal asset id string, nothing else. The legacy
// integer face values are read only when an old document is opened, never accepted from an edit.
const document::AssetRecord* namedFontAsset(const document::Project& project,
                                            const document::ParameterValue& value) {
    const auto* reference = std::get_if<std::string>(&value);
    if (reference == nullptr || reference->empty())
        return nullptr;
    std::uint64_t raw = 0;
    const auto* end = reference->data() + reference->size();
    const auto parsed = std::from_chars(reference->data(), end, raw);
    if (parsed.ec != std::errc{} || parsed.ptr != end || raw == 0)
        return nullptr;
    const auto* asset = project.findAsset(document::AssetId::fromRaw(raw));
    return asset != nullptr && asset->kind == document::AssetKind::Font ? asset : nullptr;
}

// Exactly the values a text font accepts in this project: each Font asset's id, as a string,
// with the face it names.
std::string acceptedFonts(const document::Project& project) {
    constexpr std::size_t kListed = 32;
    std::string listed;
    std::size_t count = 0;
    for (const auto& asset : project.assets()) {
        if (asset.kind != document::AssetKind::Font)
            continue;
        if (++count > kListed)
            continue;
        // Family and style name the face; the asset name is the artist's and may be anything.
        listed += (listed.empty() ? "" : ", ") + std::string("\"") +
                  std::to_string(asset.id.value()) + "\" (" + asset.fontFamily + " " +
                  asset.fontStyle + ")";
    }
    const std::string form = "the id of a Font asset in this project, as a string";
    const std::string add = "bloom.asset.ensure-font adds a face and returns its id";
    if (count == 0)
        return form + "; the project has no Font assets yet, and " + add;
    auto text = form + ": " + listed;
    if (count > kListed)
        text +=
            ", and " + std::to_string(count - kListed) + " more listed by query {kind:\"assets\"}";
    return text + "; " + add;
}

// What the parameter accepts, in words: its kind, then its members or its range.
std::string accepts(const document::ParameterRecord& record, const ParameterOwner& owner,
                    const document::Project& project) {
    if (record.schemaKey == document::kTextFontParameterSchemaKey)
        return acceptedFonts(project);
    if (owner.parameter == nullptr)
        return "a value of its schema " + record.schemaKey;
    const auto kind = owner.parameter->valueKind;
    if (const auto options = document::parameterEnumOptions(record.schemaKey); !options.empty()) {
        std::string text = "one of ";
        for (std::size_t index = 0; index < options.size(); ++index)
            text += (index == 0 ? "" : ", ") + std::to_string(options[index].value) + " (" +
                    options[index].label + ")";
        return text + ", by number or by name";
    }
    std::string text(document::parameterValueKindName(kind));
    text = (kind == document::ParameterValueKind::Integer ? "a whole number" : "a " + text);
    const auto presentation = document::parameterPresentation(*owner.parameter);
    const bool numeric = kind == document::ParameterValueKind::Integer ||
                         kind == document::ParameterValueKind::Float64;
    if (numeric && presentation.named &&
        presentation.minimum > std::numeric_limits<double>::lowest() &&
        presentation.maximum < std::numeric_limits<double>::max())
        text +=
            " in " + formatNumber(presentation.minimum) + ".." + formatNumber(presentation.maximum);
    else if (numeric && presentation.named &&
             presentation.minimum > std::numeric_limits<double>::lowest())
        text += " of at least " + formatNumber(presentation.minimum);
    return text;
}

// The value in the parameter's declared kind, or nothing when it cannot be read as one.
std::optional<document::ParameterValue> coerce(const document::ParameterValue& value,
                                               const document::ParameterRecord& record,
                                               const ParameterOwner& owner) {
    if (owner.parameter == nullptr)
        return value;
    switch (owner.parameter->valueKind) {
    case document::ParameterValueKind::Integer:
        if (std::holds_alternative<std::int64_t>(value))
            return value;
        if (const auto* number = std::get_if<double>(&value)) {
            constexpr double limit = 9007199254740992.0;
            if (std::isfinite(*number) && std::floor(*number) == *number &&
                std::abs(*number) <= limit)
                return document::ParameterValue(static_cast<std::int64_t>(*number));
            return std::nullopt;
        }
        if (const auto* name = std::get_if<std::string>(&value)) {
            if (const auto stored = document::parameterEnumValue(record.schemaKey, *name))
                return document::ParameterValue(*stored);
        }
        return std::nullopt;
    case document::ParameterValueKind::Float64:
        if (const auto* whole = std::get_if<std::int64_t>(&value))
            return document::ParameterValue(static_cast<double>(*whole));
        return value;
    case document::ParameterValueKind::Boolean:
    case document::ParameterValueKind::String:
    case document::ParameterValueKind::Color4d:
    case document::ParameterValueKind::Vec2d:
    case document::ParameterValueKind::Vec3d:
    case document::ParameterValueKind::Path:
        return value;
    }
    return value;
}

std::string valueText(const document::ParameterValue& value) {
    if (const auto* flag = std::get_if<bool>(&value))
        return *flag ? "true" : "false";
    if (const auto* whole = std::get_if<std::int64_t>(&value))
        return std::to_string(*whole);
    if (const auto* number = std::get_if<double>(&value))
        return formatNumber(*number);
    if (const auto* text = std::get_if<std::string>(&value))
        return "\"" + (text->size() > 64 ? text->substr(0, 64) + "..." : *text) + "\"";
    if (std::holds_alternative<document::Vec2d>(value))
        return "a vector2";
    if (std::holds_alternative<document::Vec3d>(value))
        return "a vector3";
    if (std::holds_alternative<core::Color4d>(value))
        return "a color";
    if (std::holds_alternative<document::PathValue>(value))
        return "a path";
    return "this value";
}

OperationResult refusedValue(const document::ParameterRecord& record, const ParameterOwner& owner,
                             const document::ParameterValue& value,
                             const document::Project& project) {
    return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                     describe(record, owner) + " cannot take " + valueText(value) +
                                         "; it takes " + accepts(record, owner, project) + ".");
}

// A driver binding is checked here, where the parameter and the driving node are both known, so a
// wrong port or kind is refused with its reason instead of surfacing later as a composition that
// cannot compile. Document validation applies the same rules to every stored binding.
std::optional<OperationResult> refusedDriver(const document::Composition& composition,
                                             const document::ParameterRecord& record,
                                             const ParameterOwner& owner,
                                             const document::DriverBindingSource& driver) {
    if (record.schemaKey == document::kTextFontParameterSchemaKey)
        return OperationResult::rejected(
            OperationIssueCode::Unsupported,
            describe(record, owner) +
                " cannot be driven: a text font is the id of a Font asset and is read as a "
                "constant. Set a constant Font asset id instead.");
    if (owner.entryLayer != nullptr)
        return OperationResult::rejected(
            OperationIssueCode::Unsupported,
            describe(record, owner) +
                " cannot be driven: a Merge entry's own layer takes a constant or an animation "
                "curve. Insert a Layer node into the entry (bloom.merge.insert-layer-node) and "
                "drive its parameter instead.");
    const auto* source = composition.graph().findNode(driver.sourceNodeId);
    if (source == nullptr)
        return OperationResult::rejected(
            OperationIssueCode::MissingReference,
            "Driver node " + std::to_string(driver.sourceNodeId.value()) +
                " does not exist in composition " + std::to_string(composition.id().value()) +
                "; add the value node first and use the id it returns.");
    const auto* sourceDefinition =
        document::builtInNodeDefinitions().find(source->typeId, source->schemaVersion);
    if (sourceDefinition == nullptr || owner.definition == nullptr)
        return std::nullopt;
    const auto sourceName =
        "node " + std::to_string(source->id.value()) + " (" + source->typeId + ")";
    const auto port = std::ranges::find(sourceDefinition->outputs, driver.outputPort,
                                        &document::OutputPortDefinition::name);
    if (port == sourceDefinition->outputs.end()) {
        std::string outputs;
        for (const auto& output : sourceDefinition->outputs)
            outputs += (outputs.empty() ? "" : ", ") + output.name + " (" +
                       std::string(document::socketValueKindName(output.valueKind)) + ")";
        return OperationResult::rejected(
            OperationIssueCode::MissingReference,
            "Driver " + sourceName + " has no output \"" + driver.outputPort + "\"; " +
                (outputs.empty() ? std::string("it has no outputs.")
                                 : "use one of its outputs: " + outputs + "."));
    }
    const auto socket = std::ranges::find(owner.definition->inputs, owner.role,
                                          &document::InputPortDefinition::name);
    if (socket == owner.definition->inputs.end())
        return OperationResult::rejected(
            OperationIssueCode::Unsupported,
            describe(record, owner) +
                " cannot be driven: it is a fixed setting chosen when the composition compiles. "
                "Set a constant value instead.");
    if (!document::isAcceptedSocketConnection(port->valueKind, socket->valueKind))
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Driver " + sourceName + " output \"" + port->name + "\" is " +
                std::string(document::socketValueKindName(port->valueKind)) + ", but " +
                describe(record, owner) + " takes " +
                std::string(document::socketValueKindName(socket->valueKind)) +
                "; drive it from a " +
                std::string(document::socketValueKindName(socket->valueKind)) +
                " output or add a conversion node between them.");
    return std::nullopt;
}

} // namespace

std::string_view SetParameterSource::typeId() const noexcept {
    return "bloom.parameter.set-source";
}

OperationResult SetParameterSource::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Composition " + std::to_string(compositionId_.value()) +
                                             " does not exist");
    }

    const auto* parameter = composition->parameters().find(parameterId_);
    if (parameter == nullptr) {
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Parameter " + std::to_string(parameterId_.value()) +
                " does not exist in composition " + std::to_string(compositionId_.value()) +
                "; query {kind:\"nodes\"} lists each node's parameter ids.");
    }
    if (composition->parameterLocked(parameterId_))
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Layer is locked; unlock it before editing " +
                describe(*parameter, ownerOf(*composition, parameterId_)) + ".");
    const auto owner = ownerOf(std::as_const(*composition), parameterId_);
    auto source = source_;
    if (auto* constant = std::get_if<document::ConstantValueSource>(&source)) {
        auto value = coerce(constant->value, *parameter, owner);
        if (!value || (parameter->schemaKey == document::kTextFontParameterSchemaKey &&
                       namedFontAsset(draft.project(), *value) == nullptr))
            return refusedValue(*parameter, owner, constant->value, draft.project());
        constant->value = *std::move(value);
    } else if (auto* curve = std::get_if<document::AnimationCurveSource>(&source);
               curve != nullptr && curve->defaultValue.has_value()) {
        auto value = coerce(*curve->defaultValue, *parameter, owner);
        if (!value)
            return refusedValue(*parameter, owner, *curve->defaultValue, draft.project());
        curve->defaultValue = *std::move(value);
    } else if (const auto* driver = std::get_if<document::DriverBindingSource>(&source)) {
        if (auto refused = refusedDriver(*composition, *parameter, owner, *driver))
            return *std::move(refused);
    }
    if (parameter->source == source) {
        return OperationResult::noChange();
    }
    const auto record = *parameter;
    // A parameter owns its animation curve outright, so replacing an animated source with a
    // constant, a driver or another curve retires the curve with it, exactly as
    // bloom.animation.convert-to-constant does. Leaving it would orphan the curve and refuse the
    // whole edit at validation.
    std::optional<document::AnimationCurveId> retiredCurve;
    if (const auto* animated = std::get_if<document::AnimationCurveSource>(&record.source)) {
        const auto* next = std::get_if<document::AnimationCurveSource>(&source);
        if (next == nullptr || next->curveId != animated->curveId)
            retiredCurve = animated->curveId;
    }
    if (!composition->parameters().setSource(parameterId_, source)) {
        if (const auto* constant = std::get_if<document::ConstantValueSource>(&source))
            return refusedValue(record, owner, constant->value, draft.project());
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         describe(record, owner) + " cannot take this source; " +
                                             "it takes " + accepts(record, owner, draft.project()) +
                                             ".");
    }
    if (retiredCurve && composition->animationCurves().find(*retiredCurve) != nullptr &&
        !composition->animationCurves().erase(*retiredCurve))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         describe(record, owner) +
                                             " could not retire its animation curve.");
    // Pointing a source node at another asset or composition renames the layers that follow it.
    (void)document::refreshLayerNamesFromSources(draft.project(), compositionId_);
    return OperationResult::applied();
}

} // namespace bloom::commands
