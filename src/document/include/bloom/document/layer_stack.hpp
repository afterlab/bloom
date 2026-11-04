#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>

#include <optional>
#include <span>
#include <vector>

namespace bloom::document {

struct LayerStackEntry {
    LayerSlotId slotId;
    LayerId layerId;

    friend bool operator==(const LayerStackEntry&, const LayerStackEntry&) = default;
};

class LayerStack final {
  public:
    explicit LayerStack(NodeId nodeId) noexcept : nodeId_(nodeId) {}

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }

    [[nodiscard]] NodeId nodeId() const noexcept { return nodeId_; }
    [[nodiscard]] std::span<const LayerStackEntry> entries() const noexcept { return entries_; }
    [[nodiscard]] const LayerStackEntry* find(LayerSlotId slotId) const noexcept;

    [[nodiscard]] bool append(LayerStackEntry entry);
    [[nodiscard]] bool erase(LayerSlotId slotId);
    [[nodiscard]] bool moveBefore(LayerSlotId slotId, std::optional<LayerSlotId> beforeSlotId);
    // #502: re-names the layer a slot carries in place (an entry layer created, adopted or
    // returned), keeping its position. Refused for a missing slot or a layer another slot names.
    [[nodiscard]] bool setLayerId(LayerSlotId slotId, LayerId layerId);

    [[nodiscard]] ValidationResult validate() const;

  private:
    bool enabled_ = true;
    NodeId nodeId_;
    std::vector<LayerStackEntry> entries_;
};

} // namespace bloom::document
