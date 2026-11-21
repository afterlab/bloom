// #757: bloom.layer.stroke-outline, the node menu's Stroke Text and Stroke Masks
// (commands::StrokeLayerOutline) as an operation, so MCP, Python and bloom-cli make
// the same undo step: Text Outline (or Mask Path) -> Path Stroke -> Shape Source on a new layer
// directly above the source layer, parented to it at the identity.

#include <bloom/commands/path_outline_stroke.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include "operation_values.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bloom::scripting::detail {
namespace {

[[nodiscard]] ArgumentSchema argument(std::string name, const ValueKind kind, const bool required,
                                      const bool inExample, const ContextSource context,
                                      Value example, std::string summary) {
    return {.name = std::move(name),
            .kind = kind,
            .required = required,
            .inExample = inExample,
            .context = context,
            .example = std::move(example),
            .summary = std::move(summary)};
}

[[nodiscard]] const Value* find(const Arguments& arguments, const std::string& name) {
    const auto iterator = arguments.find(name);
    return iterator == arguments.end() ? nullptr : &iterator->second;
}

[[nodiscard]] OperationCreateResult create(const std::string& op, const Arguments& args) {
    const auto* compositionValue = find(args, "composition");
    const auto composition = compositionValue ? idOf(*compositionValue) : std::nullopt;
    if (!composition)
        return argumentFailure(op, "composition", "expects a positive stable id or a live proxy");
    const auto* layerValue = find(args, "layer");
    const auto layer = layerValue ? idOf(*layerValue) : std::nullopt;
    if (!layer)
        return argumentFailure(op, "layer", "expects a positive stable id or a live proxy");
    auto source = commands::OutlineStrokeSource::Text;
    if (const auto* value = find(args, "source")) {
        const auto* name = std::get_if<std::string>(&value->storage);
        if (name != nullptr && *name == "masks")
            source = commands::OutlineStrokeSource::Masks;
        else if (name == nullptr || *name != "text")
            return argumentFailure(op, "source", R"(expects "text" or "masks")");
    }
    return OperationCreateResult(std::make_unique<commands::StrokeLayerOutline>(
                                     document::CompositionId::fromRaw(*composition),
                                     document::LayerId::fromRaw(*layer), source),
                                 std::nullopt);
}

} // namespace

void addLayerOutlineStrokeOperations(std::vector<OperationDescriptor>& descriptors) {
    std::vector<ArgumentSchema> arguments{
        argument("composition", ValueKind::Id, true, false, ContextSource::Composition, StableId{1},
                 "The composition to edit. Defaults to the current composition."),
        argument("layer", ValueKind::Id, true, false, ContextSource::Layer, StableId{2},
                 "The layer whose letters or masks are stroked: a layer showing a Text Source, or "
                 "one with path masks. Defaults to the selected layer."),
        argument("source", ValueKind::String, false, true, ContextSource::None, "text",
                 R"(What is stroked: "text" (the letters of the Text Source the layer shows, as )"
                 R"(Text Outline reads them) or "masks" (the layer's path masks, as Mask Path )"
                 R"(reads them). Defaults to "text".)"),
    };
    arguments.back().choices = {{"text", "Text"}, {"masks", "Masks"}};
    arguments.back().defaultText = "text";
    descriptors.push_back(
        {.schema = {.typeId = "bloom.layer.stroke-outline", .arguments = std::move(arguments)},
         .factory = [](const std::string& op, const Arguments& args) { return create(op, args); }});
}

} // namespace bloom::scripting::detail
