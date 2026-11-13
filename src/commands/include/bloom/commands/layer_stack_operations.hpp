#pragma once
// Layer-level delete and duplicate. A layer is its Layer Output boundary node, so both delegate to
// the node commands the Timeline and the Viewer already issue on that boundary (RemoveNodes and
// DuplicateNodes); they only take layer ids, the id space scripts and agents read.
#include <bloom/commands/operation.hpp>
#include <bloom/document/ids.hpp>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::commands {

// Deletes each layer, exactly as Delete in the Timeline does: its stack slot, its masks and its
// links go (for a layer with a Layer Output boundary node, that node), and so does every node
// only the deleted layers use (#714): the sources a layer brought with it, such as an imported
// SVG's paths, shapes, gradients and merges. A node anything else still reads stays, as does
// what that node reads; so a source shared with another layer is kept. One operation, one undo
// step.
class DeleteLayers final : public Operation {
  public:
    DeleteLayers(document::CompositionId composition, std::vector<document::LayerId> layers)
        : composition_(composition), layers_(std::move(layers)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.layer.delete"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    std::vector<document::LayerId> layers_;
};

// Duplicates each layer as the Viewer's Duplicate Layer does: its boundary node is copied in place
// (no layout offset), with its masks, timing, parent and matte, and the copy takes a new stack slot
// directly below the original. Outputs one "layer" per copy in argument order, plus the node
// command's "layer.<original>" and "node.<original>" outputs.
class DuplicateLayers final : public Operation {
  public:
    DuplicateLayers(document::CompositionId composition, std::vector<document::LayerId> layers)
        : composition_(composition), layers_(std::move(layers)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.layer.duplicate";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    std::vector<document::LayerId> layers_;
};

} // namespace bloom::commands
