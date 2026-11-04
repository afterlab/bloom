#pragma once

// #494: interactive node-card widgets. A node type that is edited graphically draws one widget on
// its node card and the same widget in Properties: the Grade node's colour wheels and primaries
// bar, and the Levels node's histogram with its black, white and gamma handles. Each drag is a
// session value edit (ADR 0017) that commits one command on release, so the card never depends on
// Properties for editing.
//
// The widget also decides which of the node's numeric parameter rows are shown beside it: the
// numbers stay available behind the widget (Alt-click a wheel or the graph, or its Values button)
// for exact entry, keys and links, and are hidden otherwise, so a card shows the widget rather than
// a long list of fields. A row whose input is linked always shows, so every link has its socket.
//
// The mechanism is not specific to these nodes: makeNodeCardWidget() is the one factory the node
// card and Properties ask. #498: the Curves node's curve graph is the third (curve_graph.hpp).
// #501: Hue/Saturation draws its hue strip and saturation slider (hue_saturation_widget.hpp), and
// Exposure, Offset and Multiply their amount's slider (scalar_slider_widget.hpp); each draws its
// values' rows itself, with their sockets beside them (socketY). #504: Colorize draws its mode's
// colour chips, gradient ramp and channel mix (colorize_widget.hpp). Every grading widget follows the
// shared rules of grading_controls.hpp. No other existing node has a graphical main control: a
// shape, mask, gradient or transform is drawn and dragged in the Viewer, and every other node's
// controls are values, choices and switches.

#include <QWidget>

#include <bloom/document/ids.hpp>

#include <optional>
#include <string_view>

namespace bloom::ui {

class CompositionSession;

class NodeCardWidget : public QWidget {
    Q_OBJECT

  public:
    using QWidget::QWidget;

    // Whether the node's own value row for `role` is shown next to the widget.
    [[nodiscard]] virtual bool showsRole(std::string_view role) const = 0;
    // The height the widget takes when laid out `width` pixels wide.
    [[nodiscard]] virtual int heightAt(int width) const = 0;
    // The narrowest the widget stays readable.
    [[nodiscard]] virtual int minimumWidthHint() const = 0;
    // #501: where the widget draws the row of `role` itself (its name, number and diamond, a
    // full-width row), as a y in the widget, so the card puts that parameter's socket beside it
    // rather than keeping it behind a hidden row; none for a role it does not draw that way.
    [[nodiscard]] virtual std::optional<int> socketY(std::string_view role) const {
        static_cast<void>(role);
        return std::nullopt;
    }
    // A layout gives the widget heightAt() its width (Properties); a card sizes it itself.
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override { return heightAt(width); }

  Q_SIGNALS:
    // The shown rows or the widget's height changed (a Values group opened or closed).
    void rowsChanged();
};

// True for a node type with a card widget.
[[nodiscard]] bool hasNodeCardWidget(std::string_view typeId) noexcept;
// The card widget of `node`, or null when its type has none. `card` is true on a node card.
[[nodiscard]] NodeCardWidget* makeNodeCardWidget(CompositionSession& session, document::NodeId node,
                                                 std::string_view typeId, bool card,
                                                 QWidget* parent = nullptr);

} // namespace bloom::ui
