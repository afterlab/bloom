#pragma once

// #487: the channel/layer checklist that edits a channel pattern parameter -- Channel Copy and
// Select Channels `select`, and the Composition Output "Channel Layers" -- from the channels the
// node's input channel set actually carries (ChannelSetController). Free text stays available as
// an "advanced pattern" behind a toggle, for globs and `@role` tokens a checklist cannot show. The
// same widget serves Properties and the node cards.

#include <QPointer>
#include <QString>
#include <QWidget>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/channel_inspection_listing.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::ui {

class CompositionSession;
namespace kit {
class KButton;
class KIconToggle;
class KLineEdit;
class KPopupWindow;
} // namespace kit

enum class ChannelChecklistKind : std::uint8_t { Copy, Remove, OutputLayers };

// The checklist schemas, and which kind each is.
[[nodiscard]] std::optional<ChannelChecklistKind> channelChecklistKind(std::string_view schemaKey);

struct ChannelChecklistEntry final {
    // The pattern token the entry stands for: a layer name, or a root channel's own name.
    QString token;
    QString label;
    friend bool operator==(const ChannelChecklistEntry&, const ChannelChecklistEntry&) = default;
};

// One entry per non-root layer (its every channel) and, for Copy and Select Channels, one per
// root-layer channel; the Composition Output lists layers only (RGBA always travels).
[[nodiscard]] std::vector<ChannelChecklistEntry>
channelChecklistEntries(const runtime::ChannelInspectionListing& listing,
                        ChannelChecklistKind kind);
// Which entries `pattern` checks; nullopt when the pattern holds a token the checklist cannot show
// (a glob, a role, a name not in the set), which keeps the advanced pattern field open. "Every
// entry" is `*` for Copy and Select Channels and empty for the Composition Output.
[[nodiscard]] std::optional<std::vector<bool>>
channelChecklistSelection(const QString& pattern, const std::vector<ChannelChecklistEntry>& entries,
                          ChannelChecklistKind kind);
[[nodiscard]] QString channelChecklistPattern(const std::vector<bool>& checked,
                                              const std::vector<ChannelChecklistEntry>& entries,
                                              ChannelChecklistKind kind);

class ChannelChecklistField final : public QWidget {
    Q_OBJECT

  public:
    // `role` names the pattern parameter on `node`; an older Composition Output that has no layer
    // list yet binds it on the first edit.
    ChannelChecklistField(CompositionSession& session, document::NodeId node, std::string role,
                          ChannelChecklistKind kind, QWidget* parent = nullptr);
    ~ChannelChecklistField() override;

    // Re-reads the stored pattern and the listing (requesting it when stale).
    void refresh();

    [[nodiscard]] kit::KButton* summary() const noexcept { return summary_; }
    [[nodiscard]] kit::KLineEdit* patternField() const noexcept { return pattern_; }
    [[nodiscard]] kit::KIconToggle* patternToggle() const noexcept { return toggle_; }
    [[nodiscard]] const std::vector<ChannelChecklistEntry>& entries() const noexcept {
        return entries_;
    }
    // Opens the checklist popup (the summary's click); its content holds one KCheckBox per entry.
    void openChecklist();
    [[nodiscard]] QWidget* checklist() const;
    // Checks or unchecks one entry: one undoable edit of the pattern.
    void setEntryChecked(std::size_t index, bool checked);

  private:
    void applyAdvanced(bool on);
    [[nodiscard]] document::ParameterId parameter() const;
    void write(const QString& pattern);

    CompositionSession& session_;
    document::NodeId node_;
    std::string role_;
    ChannelChecklistKind kind_;
    kit::KButton* summary_ = nullptr;
    kit::KLineEdit* pattern_ = nullptr;
    kit::KIconToggle* toggle_ = nullptr;
    QPointer<kit::KPopupWindow> popup_;
    std::vector<ChannelChecklistEntry> entries_;
    std::optional<std::vector<bool>> selection_;
    bool userAdvanced_ = false;
    bool refreshing_ = false;
};

} // namespace bloom::ui
