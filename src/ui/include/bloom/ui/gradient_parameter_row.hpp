#pragma once

// #541: one gradient value of a node, edited the same on the node card and in Properties: its
// name, a kit::KGradientEditor (any number of stops, added, moved, removed and recoloured on the
// bar) and its keyframe diamond, and under them the selected stop's colour chip and position. A
// drag on the bar, a colour pick or a position scrub is one session value edit (one undo step; a
// key at the playhead when animated, the whole gradient as one key).
//
// Colorize's Gradient Map, the Linear and Radial Gradient generators' colours and Glow's tint
// gradient draw this row. A node saved without its gradient bound shows what it renders with (the
// spec's fallback, else the node's default) and binds the gradient on its first edit.

#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>

class QAction;
class QMenu;

namespace bloom::ui {

class CompositionSession;
class KeyframeDiamond;
namespace kit {
class KColorChip;
class KGradientEditor;
class KLabel;
class KValueField;
} // namespace kit

class GradientParameterRow final : public QWidget {
    Q_OBJECT

  public:
    struct Spec final {
        std::string role;
        // A colour schema of the node: the stops' colours are authored in its encoding.
        std::string colorSchemaKey;
        QString name;
        QString description;
        // The object names: <prefix>Row, <prefix>Label, <prefix>Editor, <prefix>Diamond,
        // <prefix>StopLabel, <prefix>StopColor, <prefix>StopPosition, <prefix>Key, <prefix>Reset.
        QString objectPrefix;
        // What an unbound gradient renders with; unset means the node's default.
        std::function<std::optional<document::GradientValue>()> fallback;
    };

    GradientParameterRow(CompositionSession& session, document::NodeId node, Spec spec,
                         QWidget* parent = nullptr);

    [[nodiscard]] const std::string& role() const noexcept { return spec_.role; }
    [[nodiscard]] kit::KGradientEditor* editor() const noexcept { return editor_; }
    [[nodiscard]] kit::KColorChip* stopChip() const noexcept { return chip_; }
    [[nodiscard]] kit::KValueField* stopPosition() const noexcept { return position_; }
    [[nodiscard]] kit::KLabel* label() const noexcept { return label_; }
    [[nodiscard]] std::optional<document::ParameterId> parameter() const;
    // The gradient now: the bound parameter's live value, or what an unbound one renders with.
    [[nodiscard]] std::optional<document::GradientValue> value() const;
    void setLabelWidth(int width);
    [[nodiscard]] int labelWidthHint() const;
    [[nodiscard]] int heightHint() const;
    [[nodiscard]] int minimumWidthHint() const;
    bool reset();
    // The right-click menu of the row: Reset, then the value's key. The caller owns it.
    [[nodiscard]] QMenu* makeContextMenu();
    void refresh(bool locked);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    // The bound parameter, binding the gradient first on a node saved without it.
    [[nodiscard]] std::optional<document::ParameterId> ensureParameter();
    [[nodiscard]] std::optional<document::GradientValue> defaultValue() const;
    [[nodiscard]] document::GradientValue editorValue() const;
    void write(document::GradientValue value);
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    Spec spec_;
    kit::KLabel* label_ = nullptr;
    kit::KGradientEditor* editor_ = nullptr;
    KeyframeDiamond* diamond_ = nullptr;
    kit::KLabel* stopLabel_ = nullptr;
    kit::KColorChip* chip_ = nullptr;
    kit::KValueField* position_ = nullptr;
    QAction* keyAction_ = nullptr;
    int labelWidth_ = 0;
    bool refreshing_ = false;
};

} // namespace bloom::ui
