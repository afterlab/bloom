#pragma once

// #724 Scopes: the scope region, shared by every Scopes panel and every Viewer of a session
// (docs/ux/ui-grammar.md, "Scopes panel"). One region per composition, in whole composition pixels;
// the Region toggle's armed flag; and how many Scopes panels are visible. It is session state of
// the application window: never serialized, never part of the document, a command or undo, and it
// changes nothing that renders or exports. A Viewer's gesture only writes here; each panel's
// controller does the measuring.

#include <bloom/document/ids.hpp>
#include <bloom/runtime/scope_measure.hpp>

#include <QObject>

#include <map>
#include <optional>
#include <utility>

namespace bloom::ui {

class ScopesRegionStore final : public QObject {
    Q_OBJECT

  public:
    struct Key final {
        document::ProjectId project;
        document::CompositionId composition;

        friend auto operator<=>(const Key&, const Key&) = default;
    };

    explicit ScopesRegionStore(QObject* parent = nullptr);

    // The region of one composition; nothing: the whole frame.
    [[nodiscard]] std::optional<runtime::ScopeRegion> region(const Key& key) const;
    // An empty rectangle is no region. Emits regionChanged when the stored value changes.
    void setRegion(const Key& key, std::optional<runtime::ScopeRegion> region);
    void clearRegion(const Key& key) { setRegion(key, std::nullopt); }

    // The Region toggle: true while the active Viewer takes a region drag.
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    void setArmed(bool armed);

    // Scopes panels currently shown. With none, no Viewer draws the region or takes input for it.
    [[nodiscard]] int visiblePanels() const noexcept { return visiblePanels_; }
    [[nodiscard]] bool active() const noexcept { return visiblePanels_ > 0; }
    void panelShown();
    void panelHidden();

  signals:
    void regionChanged(const bloom::ui::ScopesRegionStore::Key& key);
    void armedChanged(bool armed);
    void activeChanged(bool active);

  private:
    std::map<Key, runtime::ScopeRegion> regions_;
    bool armed_ = false;
    int visiblePanels_ = 0;
};

} // namespace bloom::ui
