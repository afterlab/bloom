#pragma once
// ADR 0030: footage and compositions become layers, as in a layer-based compositor.
// AddDroppedLayers is the one undoable step behind a drop on the Timeline or the Viewer, the
// Viewer's Add > Import and Add…, and bloom.layer.add-from-assets; the UI only works out where the
// pointer is.
#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>

#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::document {
class Project;
}

namespace bloom::commands {

struct SvgAssetSources;

using LayerDropItem = std::variant<document::AssetId, document::CompositionId>;

// Where dropped items join the composition.
struct LayerDropPlacement final {
    enum class Stack { Top, Before, Bottom };
    Stack stack = Stack::Top;
    // For Before: the new layers go directly above this layer.
    document::LayerSlotId before{};
    // The first layer's in point, on the composition frame grid.
    core::RationalTime start{};
    // End to end (a compositor's sequence-layers command): each layer starts where the one before
    // ends.
    bool sequence = false;
    // For Before, when `before` is not given: the layer whose stack slot the new layers go above.
    // Scripts and agents name layers, not slots.
    document::LayerId beforeLayer{};

    friend bool operator==(const LayerDropPlacement&, const LayerDropPlacement&) = default;
};

// Footage (image, sequence, video, audio) and compositions other than `target` become layers;
// fonts, LUTs and the target itself do not.
[[nodiscard]] bool droppableAsLayer(const document::Project& project,
                                    document::CompositionId target, const LayerDropItem& item);

// Adds one layer per item, in order, through the ordinary add-layer command for its kind
// (AddImageLayer, AddAudioLayer, or a nested composition layer), so each layer keeps its media's
// own length (ADR 0024). The first item is uppermost; the layers start at the placement time, or
// end to end when sequenced, through the Timeline's own move math. Outputs one "layer" per item.
class AddDroppedLayers final : public Operation {
  public:
    AddDroppedLayers(document::CompositionId composition, std::vector<LayerDropItem> items,
                     LayerDropPlacement placement)
        : composition_(composition), items_(std::move(items)), placement_(placement) {}
    // #646: the source and mode of the SVG assets among the items (an imported SVG becomes its
    // artwork, never an image layer). Without a prepared source for one, applying prepares it from
    // the project's file or stored copy; every interface route prepares them earlier, off the
    // thread that applies.
    AddDroppedLayers& withSvgSources(SvgAssetSources sources);
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.layer.add-dropped";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    // Only the target composition's graph changes (and so whatever renders it).
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    std::vector<LayerDropItem> items_;
    LayerDropPlacement placement_;
    std::shared_ptr<const SvgAssetSources> svg_;
};

// Moves a layer just added to `composition` to where a drop with `placement` puts it: its stack
// position, and its start (end to end is not meaningful for one layer). Used by the SVG import so
// its artwork lands as footage does.
[[nodiscard]] OperationResult placeDroppedLayer(document::Draft& draft, document::CompositionId composition,
                                                document::LayerId layer, const LayerDropPlacement& placement);

} // namespace bloom::commands
