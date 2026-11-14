#pragma once

// The font picker (#489, docs/architecture/fonts.md): a searchable list of Recent,
// Installed and Google Fonts families drawn in their own typefaces, a style selector beside the
// family field, and the helper that edits a text node's font parameter through the session with
// one undo step. Qt lives here; the font model, resolver and catalogue are Qt-free in
// bloom_document and bloom_runtime_fonts.

#include <bloom/document/font_id.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/popup_list.hpp>
#include <bloom/ui/kit/popup_window.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QHash>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QStandardItemModel;

namespace bloom::ui {

class CompositionSession;

namespace fontpicker {

// One family row of the picker.
struct FamilyEntry final {
    document::FontSource source = document::FontSource::System;
    QString family;
    QString section;
    int category = 0; // runtime::fonts::FontCategory
    bool inCatalogue = true;
    bool recent = false;
};

// The ID a family becomes when picked, keeping the current weight and slope where the family offers
// them and otherwise taking the closest style (Regular when it has one).
[[nodiscard]] document::FontId idForFamily(document::FontSource source, const QString& family,
                                           const document::FontId& current);

// Remembers a chosen font in the per-user Recent list (newest first, bounded).
void rememberRecent(const document::FontId& id);
[[nodiscard]] std::vector<document::FontId> recentFonts();

// The styles a family offers as (label, ID) pairs, from the installed faces or the baked index.
struct StyleChoice final {
    QString label;
    document::FontId id;
};
[[nodiscard]] std::vector<StyleChoice> stylesFor(const document::FontId& id);

} // namespace fontpicker

// The popup: filter, category dropdown, sections. Emits previewed() as the current row moves (hover
// or arrow keys), chosen() on commit, and cancelled() when it closes without choosing so the owner
// reverts the live preview.
class FontPickerPopup final : public kit::KPopupWindow {
    Q_OBJECT

  public:
    explicit FontPickerPopup(QWidget* parent = nullptr);
    ~FontPickerPopup() override;

    void openForField(const QWidget& anchor, const document::FontId& current);
    [[nodiscard]] kit::KPopupList* list() const noexcept { return list_; }
    [[nodiscard]] int rowCount() const;
    [[nodiscard]] bool rowIsGoogle(int row) const;
    [[nodiscard]] QString rowFamily(int row) const;
    // True while the popup is open and settled: the installed-fonts scan has finished and the list
    // was rebuilt from it, and every visible Google row has its name-only preview face registered
    // (with no transport installed no preview can arrive, so that part is vacuously met). It goes
    // false when the list is rebuilt (scan result, filter or category change) or the popup closes,
    // and is re-evaluated on the picker's existing 200 ms poll. Tests and callers can await it
    // (readyChanged) instead of sleeping.
    [[nodiscard]] bool isReady() const noexcept { return ready_; }

  Q_SIGNALS:
    void readyChanged(bool ready);
    void previewed(const bloom::document::FontId& id);
    void chosen(const bloom::document::FontId& id);
    void cancelled();

  protected:
    void hideEvent(QHideEvent* event) override;

  private:
    void rebuild();
    void pollFonts();
    // Returns true when every visible Google row has its preview face (or none can arrive).
    [[nodiscard]] bool registerVisiblePreviews();
    void setReady(bool ready);
    [[nodiscard]] const fontpicker::FamilyEntry* entryAt(int row) const;
    [[nodiscard]] QSize wantedFrameSize() const;

    kit::KPopupList* list_ = nullptr;
    kit::KDropdown* category_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QTimer poll_;
    std::vector<fontpicker::FamilyEntry> entries_;
    document::FontId current_;
    bool choseSomething_ = false;
    bool ready_ = false;
    std::uint64_t seenPreviewEpoch_ = 0;
    // Registered preview faces: Google family -> Qt family name of its name-only subset.
    QHash<QString, QString> previewFamilies_;
};

// The closed field: the family dropdown (red when the font is missing), the style dropdown and the
// variable-axis fields of a Google family. Emits the font as text so every host (Properties row,
// node card, context bar) wires the same behavior.
class FontField final : public QWidget {
    Q_OBJECT

  public:
    explicit FontField(QWidget* parent = nullptr);

    void setFontText(const QString& text);
    [[nodiscard]] QString fontText() const { return text_; }
    [[nodiscard]] document::FontId fontId() const { return id_; }
    [[nodiscard]] kit::KDropdown* familyDropdown() const noexcept { return family_; }
    [[nodiscard]] kit::KDropdown* styleDropdown() const noexcept { return style_; }
    // Re-reads the resolver's status so the name turns red or normal.
    void refreshStatus();
    void setDense(bool dense);

  Q_SIGNALS:
    // While the picker's current row moves: the font to show live.
    void fontPreviewed(const QString& text);
    // The picker closed without choosing: put back what was there.
    void previewCancelled();
    // A choice: one undo step.
    void fontCommitted(const QString& text);

  private:
    void openPicker();
    void rebuildStyles();
    void commitId(const document::FontId& id);
    void styleActivated();
    void axisChanged(const QString& tag, double value);

    kit::KDropdown* family_ = nullptr;
    kit::KDropdown* style_ = nullptr;
    QWidget* axes_ = nullptr;
    FontPickerPopup* popup_ = nullptr;
    QString text_;
    document::FontId id_;
    std::vector<fontpicker::StyleChoice> styles_;
    QTimer statusTimer_;
};

// Wires a FontField to a font parameter: previews through beginValueEdit/updateValueEdit, reverts on
// cancel, and commits one "Set Text Font" step. `parameter` is read at each gesture.
void bindFontFieldToParameter(FontField& field, CompositionSession& session,
                              std::function<std::optional<document::ParameterId>()> parameter);

} // namespace bloom::ui
