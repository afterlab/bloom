#pragma once

// #541: one gradient value of a node, edited the same on the node card and in Properties: its
// name, a kit::KGradientEditor (any number of stops, added, moved, removed and recoloured on the
// bar) and its keyframe diamond, and under them the selected stop's colour chip and position. A
// drag on the bar, a colour pick or a position scrub is one session value edit (one undo step; a
// key at the playhead when animated, the whole gradient as one key).
//
// #543: under them a Values disclosure shows a row for each stop's colour and position, each with
// its socket on a node card, so a value node can drive a stop as it drives any other value. A
// linked row always shows; it reads "Linked to <node>" in place of its control, and the stop's
// handle on the bar is drawn linked (the editor shows the driven colour and position). Editing the
// gradient keeps each link on its stop: a stop keeps its stored index through moves, and removing
// one removes its links (CompositionSession::setGradientValue).
//
// #546: under the Stop row, a Color mode dropdown (RGB, HSV, HSL) and either an Interpolation
// dropdown (Constant, Linear, Ease, Cardinal, B-Spline, Exponential) for RGB or a Hue dropdown
// (Near, Far, Clockwise, Counter-clockwise) for HSV and HSL, as Blender's Color Ramp offers them;
// each choice is one edit, and the bar is drawn in the chosen blend.
//
// Colorize's Gradient Map, the Linear and Radial Gradient generators' colours and Glow's tint
// gradient draw this row. A node saved without its gradient bound shows what it renders with (the
// spec's fallback, else the node's default) and binds the gradient on its first edit.

#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/ui/kit/gradient_editor.hpp>

#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QAction;
class QMenu;

namespace bloom::ui {

class CompositionSession;
class KeyframeDiamond;
namespace kit {
class KButton;
class KColorChip;
class KDropdown;
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
        // <prefix>StopLabel, <prefix>StopColor, <prefix>StopPosition, <prefix>Key, <prefix>Reset,
        // <prefix>ColorMode(Label), <prefix>Interpolation(Label) and <prefix>Hue.
        QString objectPrefix;
        // What an unbound gradient renders with; unset means the node's default.
        std::function<std::optional<document::GradientValue>()> fallback;
    };

    // #543: one stop component's row: "Stop 2 color" with its chip, or "Stop 2 position" with
    // its field, and the label that replaces the control while it is linked.
    struct StopRow final {
        std::uint32_t stop = 0;
        document::GradientStopComponent component = document::GradientStopComponent::Color;
        kit::KLabel* label = nullptr;
        kit::KColorChip* chip = nullptr;
        kit::KValueField* field = nullptr;
        kit::KLabel* link = nullptr;
        int y = 0;
        bool shown = false;
    };

    GradientParameterRow(CompositionSession& session, document::NodeId node, Spec spec,
                         QWidget* parent = nullptr);

    [[nodiscard]] const std::string& role() const noexcept { return spec_.role; }
    [[nodiscard]] kit::KGradientEditor* editor() const noexcept { return editor_; }
    [[nodiscard]] kit::KColorChip* stopChip() const noexcept { return chip_; }
    [[nodiscard]] kit::KValueField* stopPosition() const noexcept { return position_; }
    [[nodiscard]] kit::KLabel* label() const noexcept { return label_; }
    [[nodiscard]] kit::KDropdown* colorModeDropdown() const noexcept { return colorMode_; }
    [[nodiscard]] kit::KDropdown* interpolationDropdown() const noexcept { return interpolation_; }
    [[nodiscard]] kit::KDropdown* hueDropdown() const noexcept { return hue_; }
    [[nodiscard]] kit::KButton* valuesButton() const noexcept { return disclosure_; }
    [[nodiscard]] bool valuesShown() const noexcept { return expanded_; }
    void setValuesShown(bool shown);
    [[nodiscard]] const std::vector<StopRow>& stopRows() const noexcept { return stopRows_; }
    // Where the socket of stop port `port` (document::gradientStopPortName) sits in the row, while
    // its stop row shows.
    [[nodiscard]] std::optional<int> socketY(std::string_view port) const;
    // The link record of a stop component, when a value node drives it.
    [[nodiscard]] std::optional<document::ParameterId>
    stopLink(std::uint32_t stop, document::GradientStopComponent component) const;
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

  Q_SIGNALS:
    // The row's height changed (the Values rows opened or closed, a stop or a link came or went).
    void layoutChanged();

  protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    // The gradient as it renders now: value() with each link's driven value over its stop.
    [[nodiscard]] std::optional<document::GradientValue> shownValue() const;
    // Writes stops shaped as the editor's (each with its origin), kept in their stored order.
    void writeStops(std::vector<kit::KGradientEditor::Stop> stops);
    // Writes a gradient in stored order whose stop `i` was stored stop `newIndexOfOld^-1(i)`.
    void writeStored(document::GradientValue value,
                     std::vector<std::optional<std::uint32_t>> newIndexOfOld,
                     const QString& label = {});
    // #546: writes the authored gradient with `modes`, its stops as they are.
    void writeModes(document::GradientModes modes);
    void rebuildStopRows(std::size_t stops);
    [[nodiscard]] int stopRowsTop() const;
    // The bound parameter, binding the gradient first on a node saved without it.
    [[nodiscard]] std::optional<document::ParameterId> ensureParameter();
    [[nodiscard]] std::optional<document::GradientValue> defaultValue() const;
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
    kit::KLabel* colorModeLabel_ = nullptr;
    kit::KDropdown* colorMode_ = nullptr;
    kit::KLabel* interpolationLabel_ = nullptr;
    kit::KDropdown* interpolation_ = nullptr;
    kit::KDropdown* hue_ = nullptr;
    QAction* keyAction_ = nullptr;
    kit::KButton* disclosure_ = nullptr;
    std::vector<StopRow> stopRows_;
    // The authored gradient the editor's stops were last set from (their origins index it).
    std::optional<document::GradientValue> projected_;
    int labelWidth_ = 0;
    int lastHeight_ = 0;
    bool expanded_ = false;
    bool refreshing_ = false;
};

} // namespace bloom::ui
