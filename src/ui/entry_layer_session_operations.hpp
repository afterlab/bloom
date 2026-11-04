#pragma once

// #502 Every Merge entry is a layer. Two small adapters the session composes the commands layer's
// own operations with, so an edit of an entry without a layer yet stays ONE undo step:
//
//   * EntryRoleEdit applies an operation built for the parameter a layer binds to a role, found on
//     the draft at apply time -- the entry layer EntryLayerEdit creates in the same transaction
//     has parameter ids nobody can know before it runs.
//   * MaterializedEdit replays a finished transaction's operations after making sure an entry has
//     its layer, on the same draft. The session uses it to fold the layer a live gesture created
//     at its start into the gesture's commit (CompositionSession::runTransaction): the creation is
//     withdrawn first, and because the id allocator is part of the snapshot the layer is recreated
//     with the very ids the gesture's commit names.
//
// Neither adds behaviour of its own: every document change is made by an existing command.

#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/operation.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/ui/composition_session.hpp>

#include <vector>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bloom::ui {

using ParameterOperationBuilder =
    std::function<std::unique_ptr<commands::Operation>(document::ParameterId parameter)>;

class EntryRoleEdit final : public commands::Operation {
  public:
    EntryRoleEdit(document::CompositionId composition, document::LayerId layer, std::string role,
                  ParameterOperationBuilder build)
        : composition_(composition), layer_(layer), role_(std::move(role)),
          build_(std::move(build)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.ui.entry-role-edit";
    }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    std::string role_;
    ParameterOperationBuilder build_;
};

class MaterializedEdit final : public commands::Operation {
  public:
    MaterializedEdit(document::CompositionId composition, document::NodeId merge,
                     document::LayerSlotId slot, commands::Transaction edit)
        : composition_(composition), merge_(merge), slot_(slot),
          edit_(std::make_shared<commands::Transaction>(std::move(edit))) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.ui.materialized-edit";
    }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::NodeId merge_;
    document::LayerSlotId slot_;
    std::shared_ptr<commands::Transaction> edit_;
};

// Appends `build(layer)` for every target, each layer once: directly on a target that has a
// layer, and through commands::EntryLayerEdit (creating the plain entry's layer in the same
// transaction) on one that has none.
void appendLayerEdits(commands::Transaction& transaction, document::CompositionId composition,
                      const std::vector<LayerTarget>& targets,
                      const commands::EntryLayerOperationBuilder& build);

} // namespace bloom::ui
