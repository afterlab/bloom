#include <bloom/ui/scopes_region_store.hpp>

namespace bloom::ui {

ScopesRegionStore::ScopesRegionStore(QObject* const parent) : QObject(parent) {}

std::optional<runtime::ScopeRegion> ScopesRegionStore::region(const Key& key) const {
    const auto found = regions_.find(key);
    return found == regions_.end() ? std::nullopt : std::optional{found->second};
}

void ScopesRegionStore::setRegion(const Key& key, std::optional<runtime::ScopeRegion> region) {
    if (region && (region->width == 0 || region->height == 0))
        region.reset();
    const auto found = regions_.find(key);
    if (!region) {
        if (found == regions_.end())
            return;
        regions_.erase(found);
    } else if (found != regions_.end()) {
        if (found->second == *region)
            return;
        found->second = *region;
    } else {
        regions_.emplace(key, *region);
    }
    emit regionChanged(key);
}

void ScopesRegionStore::setArmed(const bool armed) {
    if (armed_ == armed)
        return;
    armed_ = armed;
    emit armedChanged(armed_);
}

void ScopesRegionStore::panelShown() {
    ++visiblePanels_;
    if (visiblePanels_ == 1)
        emit activeChanged(true);
}

void ScopesRegionStore::panelHidden() {
    if (visiblePanels_ == 0)
        return;
    --visiblePanels_;
    if (visiblePanels_ == 0) {
        // The toggle belongs to a visible panel; nothing stays armed with none.
        setArmed(false);
        emit activeChanged(false);
    }
}

} // namespace bloom::ui
