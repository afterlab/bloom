#include <bloom/ui/scopes_region_store.hpp>

#include <bloom/render/scope_table_layout.hpp>

namespace bloom::ui {

ScopesRegionStore::ScopesRegionStore(QObject* const parent)
    : QObject(parent), colors_(std::make_shared<runtime::ScopeSignalColorCache>()) {}

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
    requests_.erase(key);
    emit regionChanged(key);
    if (active())
        emit measureChanged();
}

void ScopesRegionStore::setArmed(const bool armed) {
    if (armed_ == armed)
        return;
    armed_ = armed;
    emit armedChanged(armed_);
}

void ScopesRegionStore::measureSettingsChanged() {
    requests_.clear();
    emit drawingChanged();
    if (active())
        emit measureChanged();
}

void ScopesRegionStore::setSignal(const runtime::ScopeSignal signal) {
    if (signal_ == signal)
        return;
    signal_ = signal;
    measureSettingsChanged();
}

void ScopesRegionStore::setWorkingAxisMaximum(const std::uint32_t axisMaximum) {
    if (!runtime::validScopeAxisMaximum(axisMaximum) || axisMaximum_ == axisMaximum)
        return;
    axisMaximum_ = axisMaximum;
    if (signal_ == runtime::ScopeSignal::Working)
        measureSettingsChanged();
    else
        emit drawingChanged();
}

void ScopesRegionStore::setIgnoreTransparent(const bool ignore) {
    if (ignoreTransparent_ == ignore)
        return;
    ignoreTransparent_ = ignore;
    measureSettingsChanged();
}

runtime::ScopeMeasureSettings ScopesRegionStore::settings(const Key& key) const {
    runtime::ScopeMeasureSettings settings;
    settings.scopes = render::kScopeTableAll;
    settings.signal = signal_;
    settings.axisMaximum = signal_ == runtime::ScopeSignal::Working ? axisMaximum_ : 1U;
    settings.region = region(key);
    settings.ignoreTransparent = ignoreTransparent_;
    return settings;
}

std::shared_ptr<const runtime::PreviewScopeRequest>
ScopesRegionStore::request(const Key& key) const {
    if (!active())
        return nullptr;
    auto& slot = requests_[key];
    if (slot == nullptr)
        slot = std::make_shared<const runtime::PreviewScopeRequest>(runtime::PreviewScopeRequest{
            .settings = settings(key), .colors = colors_, .ocio = ocio_});
    return slot;
}

void ScopesRegionStore::setOcio(
    std::function<std::shared_ptr<const runtime::GpuSceneOcioContext>()> ocio) {
    ocio_ = std::move(ocio);
    requests_.clear();
}

void ScopesRegionStore::setDrawing(const Drawing& drawing) {
    if (drawing_ == drawing)
        return;
    drawing_ = drawing;
    emit drawingChanged();
}

void ScopesRegionStore::panelShown() {
    ++visiblePanels_;
    if (visiblePanels_ == 1) {
        requests_.clear();
        emit activeChanged(true);
        emit measureChanged();
    }
}

void ScopesRegionStore::panelHidden() {
    if (visiblePanels_ == 0)
        return;
    --visiblePanels_;
    if (visiblePanels_ == 0) {
        // The toggle belongs to a visible panel; nothing stays armed with none.
        setArmed(false);
        // Frames keep the tables they carry; no request asks for more until a panel shows again.
        requests_.clear();
        emit activeChanged(false);
    }
}

} // namespace bloom::ui
