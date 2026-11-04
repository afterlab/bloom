// #502 Every Merge entry is a layer (docs/architecture/layer-graph-model.md, "Merge"). The scripting
// face of the entry commands: create an entry's own layer, insert a Layer node into an entry, add
// an Adjustment layer, set an entry's opacity, transform or blend mode by role, and address every
// layer edit at a Merge entry. An entry that has no layer yet behaves as one at its defaults; the
// first edit of any of its controls creates that layer in the same undo step, exactly as the
// Timeline does, so an agent never needs a layer id it cannot know yet.

#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/project.hpp>

#include "operation_registry_parity.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::scripting::detail::parity {
namespace {

using document::CompositionId;
using document::LayerId;
using document::LayerSlotId;
using document::NodeId;
using document::ParameterId;

constexpr std::string_view kSlotUnit = "layer-stack slot id (query:layers .entries[].slot)";
constexpr std::string_view kMergeUnit = "Merge node id (query:layers .entries[].merge)";

const std::string kEnsure =
    R"(Makes sure a Merge entry has a layer and returns it (#502: every Merge entry is a layer). )"
    R"(An entry whose layer already exists -- a Layer node on its image chain, or its own entry )"
    R"(layer -- is left as it is and its layer id is returned. An entry without one gets its own )"
    R"(entry layer at the defaults it already renders with (the identity: position the )"
    R"(composition centre, anchor [0, 0], scale [1, 1], rotation 0, opacity 1, Normal, pivot )"
    R"(frame), named after its source. Every layer operation also takes `merge` and `slot` )"
    R"(instead of `layer` and creates the layer in the same undo step, so this is only needed to )"
    R"(learn the parameter ids before animating them. )"
    R"(Outputs: "layer" (the entry's layer id), and when the layer was created "parameter.<role>" )"
    R"(for position, anchor, scale, rotation, opacity and blendMode. )"
    R"(Errors: "The Merge entry does not exist in the composition" - a stale merge or slot, read )"
    R"(query kind=layers `entries`. )"
    R"(Undo: one step when the layer is created; nothing changes otherwise. )"
    R"(MCP example: {"op":"bloom.merge.ensure-entry-layer","args":{"merge":4,"slot":2}}.)";

const std::string kInsertLayerNode =
    R"(Inserts a Layer node into a Merge entry, between its image source and the slot, as the )"
    R"(Timeline's "Insert Layer Node" does. The picture does not change: the Layer node adopts )"
    R"(the entry's own layer values (transform, opacity, blend mode with their animation, flags, )"
    R"(name, label, timing, masks, matte, parent and pivot) in the same undo step, and becomes )"
    R"(the entry's layer; layers that referenced the entry layer (as parent or matte) now )"
    R"(reference the Layer node's layer. An audio edge of the slot stays where it is. )"
    R"(Outputs: "node" (the Layer node), "layer" (its layer id), "edge" (source -> Layer node) )"
    R"(and "parameter.<role>" for the Layer node's parameters. )"
    R"(Errors: "The Merge entry does not exist in the composition" - a stale merge or slot; )"
    R"("An entry without an image has no Layer node to insert" - an audio-only slot; "Layer is )"
    R"(locked" - unlock the entry's layer first. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.merge.insert-layer-node","args":{"merge":4,"slot":2}}.)";

const std::string kAddAdjustment =
    R"(Adds an Adjustment layer to a Merge, as the Timeline's Add > Adjustment Layer does: a new )"
    R"(Adjustment node, a new entry of the Merge fed by it directly above the entry `above` (or )"
    R"(at the top), and that entry's own layer named "Adjustment Layer N". The Adjustment node's )"
    R"(Image output is the stack below its entry; filters connected between the Adjustment node )"
    R"(and the entry process it, and the entry replaces the stack below it (opacity mixes the )"
    R"(adjusted stack with the unadjusted one; masks, matte, blend and timing apply as on any )"
    R"(layer). Connect a filter with bloom.node.add-fed-by on the Adjustment node's "image" )"
    R"(output. )"
    R"(Outputs: "node" (the Adjustment node), "layerSlot" (the new entry's slot) and "layer" (its )"
    R"(entry layer). )"
    R"(Errors: "The Merge does not exist in the composition" - a stale merge id; "The entry to )"
    R"(add the Adjustment Layer above does not exist in the Merge" - a stale `above` slot. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.merge.add-adjustment-layer","args":{"merge":4}}.)";

const std::string kSetEntryParameter =
    R"(Sets one layer control of a Merge entry by its role -- position, anchor, scale, rotation, )"
    R"(opacity, blendMode or timeRemap -- whatever the entry's layer is: its Layer node, or its )"
    R"(own entry layer, which is created at its defaults in the same undo step when the entry )"
    R"(has none yet. Without `time` the parameter becomes the constant `value` (an animated one )"
    R"(loses its animation, as bloom.parameter.set-source does); with `time` it is keyed at that )"
    R"(time (a constant becomes animated, its constant the first key). A blend mode takes its )"
    R"(stored value or its label ("screen"). An entry layer's parameters are constants or )"
    R"(animation curves; to drive one from the value graph, insert a Layer node )"
    R"((bloom.merge.insert-layer-node). )"
    R"(Outputs: "layer" (the entry's layer), "parameter.<role>" ids when the layer was created, )"
    R"(and the edit's own outputs (a key's "keyframe"). )"
    R"(Errors: "The Merge entry does not exist in the composition" - a stale merge or slot; "Time )"
    R"(Remap is off on this layer" - run bloom.layer.enable-time-remap first; "Layer is locked" )"
    R"(- unlock it; a value of the wrong kind or out of range names what the parameter takes. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.merge.set-entry-parameter","args":{"merge":4,"slot":2,)"
    R"("role":"opacity","value":0.5}}.)";

ArgumentSchema withUnit(ArgumentSchema argument, const std::string_view unit) {
    argument.unit = std::string(unit);
    argument.minimum = 1.0;
    return argument;
}

ArgumentSchema mergeArgument() {
    return withUnit(need("merge", ValueKind::Id, StableId{1},
                         "Id. The Merge node whose entry this is (query kind=layers `entries`)."),
                    kMergeUnit);
}

ArgumentSchema slotArgument() {
    return withUnit(need("slot", ValueKind::Id, StableId{1}, "Id. The entry's stack slot."),
                    kSlotUnit);
}

struct EntryAddress final {
    CompositionId composition;
    NodeId merge;
    LayerSlotId slot;
};

std::optional<EntryAddress> readEntry(Reader& read) {
    EntryAddress address{.composition = read.id<CompositionId>("composition"),
                         .merge = read.id<NodeId>("merge"),
                         .slot = read.id<LayerSlotId>("slot")};
    if (read.failed())
        return std::nullopt;
    return address;
}

Arguments without(Arguments arguments, std::initializer_list<std::string_view> names) {
    for (const auto name : names)
        if (const auto found = arguments.find(name); found != arguments.end())
            arguments.erase(found);
    return arguments;
}

// A parameter edit of one layer whose parameter is found by its role when the edit applies: the
// layer may have been created by the same operation a moment before (EntryLayerEdit), so its
// parameter ids cannot be known when the operation is created.
class LayerRoleParameterEdit final : public commands::Operation {
  public:
    using Factory = std::function<OperationCreateResult(ParameterId)>;

    LayerRoleParameterEdit(CompositionId composition, LayerId layer, std::string role,
                           std::unique_ptr<commands::Operation> probe, Factory factory)
        : composition_(composition), layer_(layer), role_(std::move(role)),
          probe_(std::move(probe)), factory_(std::move(factory)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.merge.set-entry-parameter";
    }
    [[nodiscard]] bool renderAffecting() const noexcept override {
        return probe_->renderAffecting();
    }
    [[nodiscard]] std::optional<CompositionId> parameterEditScope() const noexcept override {
        return probe_->parameterEditScope();
    }
    [[nodiscard]] std::optional<CompositionId> renderScope() const noexcept override {
        return composition_;
    }

    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override {
        const auto* composition = std::as_const(draft).project().findComposition(composition_);
        const auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
        if (layer == nullptr)
            return commands::OperationResult::rejected(
                commands::OperationIssueCode::InvalidTarget,
                "Layer " + std::to_string(layer_.value()) + " is not in the composition");
        const auto* binding = document::layerParameterBinding(composition->graph(), *layer, role_);
        if (binding == nullptr)
            return commands::OperationResult::rejected(
                commands::OperationIssueCode::MissingReference,
                role_ == document::kTimeRemapParameterRole
                    ? std::string("Time Remap is off on this layer; run "
                                  "bloom.layer.enable-time-remap first")
                    : "The layer has no " + role_ + " parameter");
        auto created = factory_(binding->parameterId);
        if (!created) {
            const auto* diagnostic = created.diagnostic();
            return commands::OperationResult::rejected(
                commands::OperationIssueCode::InvalidValue,
                diagnostic ? diagnostic->message : std::string("The edit could not be created"));
        }
        return std::move(created).takeOperation()->apply(draft);
    }

  private:
    CompositionId composition_;
    LayerId layer_;
    std::string role_;
    std::unique_ptr<commands::Operation> probe_;
    Factory factory_;
};

const OperationDescriptor* findDescriptor(const std::vector<OperationDescriptor>& descriptors,
                                          const std::string_view id) {
    const auto found = std::ranges::find(descriptors, id, [](const OperationDescriptor& item) {
        return std::string_view(item.schema.typeId);
    });
    return found == descriptors.end() ? nullptr : &*found;
}

// Every layer edit taking `layer` (from the selection when omitted) also addresses a Merge entry by
// `merge` and `slot`; the entry's layer is created in the same undo step when it has none yet.
void addressEntries(std::vector<OperationDescriptor>& descriptors) {
    for (auto& descriptor : descriptors) {
        auto& arguments = descriptor.schema.arguments;
        const auto layer = std::ranges::find(arguments, "layer", &ArgumentSchema::name);
        if (layer == arguments.end() || layer->context != ContextSource::Layer ||
            layer->kind != ValueKind::Id || descriptor.contextualFactory ||
            std::ranges::any_of(arguments, [](const ArgumentSchema& argument) {
                return argument.name == "merge" || argument.name == "slot";
            }))
            continue;
        layer->required = false;
        layer->contextUnless = "merge";
        layer->summary += " Or give merge and slot instead to edit a Merge entry's layer.";
        arguments.push_back(withUnit(
            maybe("merge", ValueKind::Id, StableId{1},
                  "Id. Instead of layer: the Merge node of the entry to edit (with slot). When "
                  "the entry has no layer yet its own layer is created at its defaults in the "
                  "same undo step; the result then also outputs \"layer\" and "
                  "\"parameter.<role>\"."),
            kMergeUnit));
        arguments.push_back(
            withUnit(maybe("slot", ValueKind::Id, StableId{1},
                           "Id. With merge: the entry's stack slot."),
                     kSlotUnit));
        descriptor.factory = [inner = descriptor.factory](
                                 const std::string& op,
                                 const Arguments& args) -> OperationCreateResult {
            const bool merge = args.contains("merge");
            const bool slot = args.contains("slot");
            if (!merge && !slot) {
                if (!args.contains("layer"))
                    return argumentFailure(op, "layer",
                                           "is required unless merge and slot name a Merge entry");
                return inner(op, args);
            }
            if (merge != slot)
                return argumentFailure(op, merge ? "slot" : "merge",
                                       "is required with the other: merge and slot together "
                                       "name a Merge entry");
            Reader read(op, args);
            const auto entry = readEntry(read);
            // Given with merge and slot, `layer` must name that entry's layer; it is checked when
            // the edit applies, against the entry's layer then.
            const auto named = args.contains("layer") ? read.id<LayerId>("layer") : LayerId{};
            if (!entry || read.failed())
                return read.error();
            auto rest = without(args, {"merge", "slot", "layer"});
            // The rest of the call is checked now, against a placeholder layer, so a malformed
            // call is refused when it is created rather than when it applies.
            auto probe = rest;
            probe["layer"] = Value(StableId{1});
            if (auto checked = inner(op, probe); !checked)
                return checked;
            return made(std::make_unique<commands::EntryLayerEdit>(
                entry->composition, entry->merge, entry->slot,
                [inner, op, named, rest = std::move(rest)](
                    const LayerId layerId) -> std::unique_ptr<commands::Operation> {
                    if (named.isValid() && named != layerId)
                        return nullptr;
                    auto arguments = rest;
                    arguments["layer"] = Value(StableId{layerId.value()});
                    auto created = inner(op, arguments);
                    return created ? std::move(created).takeOperation() : nullptr;
                }));
        };
    }
}

} // namespace

void addMergeEntryOperations(std::vector<OperationDescriptor>& descriptors) {
    // Every layer edit registered so far learns the entry address first; the operations below take
    // `merge` and `slot` themselves.
    addressEntries(descriptors);

    add(descriptors, "bloom.merge.ensure-entry-layer", kEnsure,
        {composition(), mergeArgument(), slotArgument()},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto entry = readEntry(read);
            if (!entry)
                return read.error();
            return made(std::make_unique<commands::EnsureEntryLayer>(entry->composition,
                                                                     entry->merge, entry->slot));
        });

    add(descriptors, "bloom.merge.insert-layer-node", kInsertLayerNode,
        {composition(), mergeArgument(), slotArgument()},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto entry = readEntry(read);
            if (!entry)
                return read.error();
            return made(std::make_unique<commands::InsertLayerNode>(entry->composition,
                                                                    entry->merge, entry->slot));
        });

    add(descriptors, "bloom.merge.add-adjustment-layer", kAddAdjustment,
        {composition(),
         withUnit(need("merge", ValueKind::Id, StableId{1},
                       "Id. The Merge node to add the Adjustment layer to."),
                  "Merge node id (query:nodes)"),
         withUnit(maybe("above", ValueKind::Id, StableId{1},
                        "Id. The entry slot the new entry goes directly above. Default: the top "
                        "of the stack."),
                  kSlotUnit)},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto merge = read.id<NodeId>("merge");
            const auto above = read.id<LayerSlotId>("above", false);
            if (read.failed())
                return read.error();
            return made(std::make_unique<commands::AddAdjustmentLayer>(
                compositionId, merge,
                above.isValid() ? std::optional(above) : std::nullopt));
        });

    // The constant and keyed forms reuse the parameter operations' own factories, so values parse
    // and validate exactly as bloom.parameter.set-source and
    // bloom.animation.set-keyframe-at-time-for-parameter take them.
    const auto* setSource = findDescriptor(descriptors, "bloom.parameter.set-source");
    const auto* setKey =
        findDescriptor(descriptors, "bloom.animation.set-keyframe-at-time-for-parameter");
    if (setSource == nullptr || setKey == nullptr)
        return;
    auto role = need("role", ValueKind::String, "opacity",
                     "String. The layer control: position, anchor, scale, rotation, opacity, "
                     "blendMode or timeRemap.");
    role.unit = "layer parameter role";
    for (const auto& entryRole : document::entryLayerRoles())
        role.choices.push_back({.value = Value(std::string(entryRole.role)),
                                .label = std::string(entryRole.role)});
    auto time = maybe("time", ValueKind::Time, std::int64_t{0},
                      "Time. When given, the value is keyed at this time instead of replacing the "
                      "parameter with a constant.");
    add(descriptors, "bloom.merge.set-entry-parameter", kSetEntryParameter,
        {composition(), mergeArgument(), slotArgument(), role,
         need("value", ValueKind::Value, 0.5,
              "Value. In the parameter's stored units: [x, y] for position, anchor and scale, "
              "degrees for rotation, 0..1 for opacity, a blend mode's value or label, seconds "
              "for timeRemap."),
         time},
        [constant = setSource->factory, keyed = setKey->factory](const std::string& op,
                                                                 const Arguments& args) {
            Reader read(op, args);
            const auto entry = readEntry(read);
            const auto roleName = read.text("role");
            if (!entry)
                return read.error();
            const auto roles = document::entryLayerRoles();
            if (!roleName ||
                std::ranges::none_of(roles, [&](const auto& item) { return item.role == *roleName; }))
                return argumentFailure(op, "role",
                                       "expects one of position, anchor, scale, rotation, "
                                       "opacity, blendMode or timeRemap");
            const bool key = args.contains("time");
            const auto innerId = key ? std::string("bloom.animation.set-keyframe-at-time-for-parameter")
                                     : std::string("bloom.parameter.set-source");
            const auto inner = key ? keyed : constant;
            Arguments innerArguments = without(args, {"merge", "slot", "role"});
            const auto build = [inner, innerId, innerArguments](const ParameterId parameter) {
                auto arguments = innerArguments;
                arguments["parameter"] = Value(StableId{parameter.value()});
                return inner(innerId, arguments);
            };
            // Checked now against a placeholder parameter so a malformed value is refused at once.
            if (auto checked = build(ParameterId::fromRaw(1)); !checked) {
                const auto* diagnostic = checked.diagnostic();
                return argumentFailure(op, diagnostic ? diagnostic->argument : "value",
                                       diagnostic ? diagnostic->message : "is not valid");
            }
            return made(std::make_unique<commands::EntryLayerEdit>(
                entry->composition, entry->merge, entry->slot,
                [composition = entry->composition, role = *roleName,
                 build](const LayerId layer) -> std::unique_ptr<commands::Operation> {
                    auto probe = build(ParameterId::fromRaw(1));
                    if (!probe)
                        return nullptr;
                    return std::make_unique<LayerRoleParameterEdit>(
                        composition, layer, role, std::move(probe).takeOperation(), build);
                }));
        });
}

} // namespace bloom::scripting::detail::parity
