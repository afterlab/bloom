#pragma once
// #502 Every Merge entry is a layer. A Merge entry without a Layer node keeps its layer controls on
// its own entry layer (a LayerOutputBoundary owned by the slot; see bloom/document/merge_entries.hpp
// and docs/architecture/layer-graph-model.md, "Merge"). These operations create that layer on the
// first edit of any of its controls, insert a Layer node into an entry, and add an Adjustment
// layer. The command stack's reconcile pass (reconcileEntryLayers) keeps entries and Layer nodes in
// step after every transaction: inserting or removing a Layer node adopts or returns the entry's
// values in the same undo step, so the picture never changes.
#include <bloom/commands/operation.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/document/ids.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace bloom::commands {

// Output names. Every entry-layer operation names the entry's layer "layer"; the ones that create
// nodes or slots name them "node" and "layerSlot".
inline constexpr std::string_view kEntryLayerOutput = "layer";
inline constexpr std::string_view kEntryLayerNodeOutput = "node";
inline constexpr std::string_view kEntryLayerSlotOutput = "layerSlot";

// Makes sure the entry (merge, slot) has a layer and outputs its LayerId as "layer". An entry whose
// layer already exists -- a Layer node on its chain or its own entry layer -- is a NoChange that
// still outputs that layer. Otherwise it creates the entry layer at its defaults (the identity:
// position the composition centre, anchor {0,0}, scale {1,1}, rotation 0, opacity 1, Normal,
// pivot Frame, no Time Remap), named after the slot's source (following it) or after the source's
// kind, and also outputs "parameter.<role>" for each parameter it created.
class EnsureEntryLayer final : public Operation {
  public:
    EnsureEntryLayer(document::CompositionId composition, document::NodeId merge,
                     document::LayerSlotId slot)
        : composition_(composition), merge_(merge), slot_(slot) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.merge.ensure-entry-layer";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::NodeId merge_;
    document::LayerSlotId slot_;
};

// Builds the edit to apply to an entry's layer once its LayerId is known.
using EntryLayerOperationBuilder =
    std::function<std::unique_ptr<Operation>(document::LayerId layer)>;

// One undo step for any control of any Merge entry: EnsureEntryLayer, then the operation `build`
// returns for that layer, on the same draft (for example SetLayerSolo, SetParameterSource on a
// parameter found through document::layerParameterBinding, SetTrackMatte, AddLayerMask). Outputs
// EnsureEntryLayer's outputs followed by the edit's. A rejected edit rejects the whole operation;
// an edit that changes nothing is a NoChange even when the layer was just created (the draft is
// then discarded unless another operation of the transaction applied). Render classification
// delegates to the built edit when the layer already existed; creating a layer is whole-render.
class EntryLayerEdit final : public Operation {
  public:
    EntryLayerEdit(document::CompositionId composition, document::NodeId merge,
                   document::LayerSlotId slot, EntryLayerOperationBuilder build)
        : composition_(composition), merge_(merge), slot_(slot), build_(std::move(build)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] bool renderAffecting() const noexcept override;
    [[nodiscard]] std::optional<document::CompositionId>
    parameterEditScope() const noexcept override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::NodeId merge_;
    document::LayerSlotId slot_;
    EntryLayerOperationBuilder build_;
    // The edit built by the last apply, and whether that apply created the layer. Read only by the
    // render classification the command stack asks for after apply.
    mutable std::shared_ptr<Operation> built_;
    mutable bool created_ = false;
};

// Splices a new Layer node between the entry's image source and its slot (image only; an audio edge
// stays where it is). The Layer node becomes the entry's layer; when the entry had its own entry
// layer, the reconcile pass moves that layer's values into the new node in the same step, so the
// picture does not change. Outputs "node" (the Layer node), "layer" (its LayerId) and "edge" (the
// new source -> Layer edge). Refused for an entry without an image or whose layer is locked.
class InsertLayerNode final : public Operation {
  public:
    InsertLayerNode(document::CompositionId composition, document::NodeId merge,
                    document::LayerSlotId slot)
        : composition_(composition), merge_(merge), slot_(slot) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.merge.insert-layer-node";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::NodeId merge_;
    document::LayerSlotId slot_;
};

// The Timeline's "Adjustment Layer": a new Adjustment node, a new slot of `merge` fed by it --
// directly above `above` when given, else at the top -- and that entry's own layer, named
// "Adjustment Layer N". Outputs "node" (the Adjustment), "layerSlot" and "layer".
class AddAdjustmentLayer final : public Operation {
  public:
    AddAdjustmentLayer(document::CompositionId composition, document::NodeId merge,
                       std::optional<document::LayerSlotId> above = std::nullopt)
        : composition_(composition), merge_(merge), above_(above) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.merge.add-adjustment-layer";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::NodeId merge_;
    std::optional<document::LayerSlotId> above_;
};

// What one reconcile pass did, for work-counter tests.
struct EntryLayerReconcileWork final {
    // Compositions whose content changed but whose graph topology (nodes, edges, Merge slots,
    // layer boundaries) did not, so the pass stopped after one comparison.
    std::uint64_t compositionsSkipped = 0;
    // Compositions whose topology changed and whose entries were examined.
    std::uint64_t compositionsReconciled = 0;
    std::uint64_t entriesExamined = 0;
    // Entry layers whose values moved into a Layer node (adopt), entry layers a Layer node replaced
    // without adopting, entry layers created from a Layer node that left the entry (return), and
    // entry layers removed because their slot vanished.
    std::uint64_t adopted = 0;
    std::uint64_t replaced = 0;
    std::uint64_t returned = 0;
    std::uint64_t removed = 0;
};

struct EntryLayerReconcileResult final {
    bool changed = false;
    std::optional<OperationIssue> failure;
};

// The reconcile pass (#502). Compares each composition `draft` changed against `before`, and
// only when its graph topology changed:
//   * an entry whose layer is now a Layer node L but which still has its own entry layer E: when L
//     was created by this transaction, or holds default values and was no entry's layer before,
//     E's values (parameter sources with their curves, flags, label, name, timing, masks, matte,
//     parent and pivot) move into L; otherwise L wins. Either way E and its parameters go, and
//     references to E point at L;
//   * an entry whose layer was a Layer node and now has none gets an entry layer holding a copy of
//     that Layer node's values (pivot included); references to a deleted Layer node follow it;
//   * entry layers whose slot vanished leave with their parameters, and references to them clear.
// CommandStack::execute runs it after the operations and before validation and commit, so it is
// part of the same undo step.
[[nodiscard]] EntryLayerReconcileResult
reconcileEntryLayers(const document::Project& before, document::Draft& draft,
                     EntryLayerReconcileWork* work = nullptr);

} // namespace bloom::commands
