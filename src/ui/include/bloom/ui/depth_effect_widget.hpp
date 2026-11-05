#pragma once

// #483: the Z Defocus and Depth Fog card widget, drawn and edited the same on the node card and in
// Properties:
//
// - the depth input's checks: a colour transform on the file the depth comes from (a depth pass is
//   data; an sRGB or Linear read bends its values) with a one-click "Read as data" fix, and a
//   colour picture connected as the depth (its red, green and blue differ where a depth pass is
//   grey, ImagePreviewController::depthChannelSpread, read on a worker);
// - the near and far distances, and Depth Fog's start and end, each a RangeRow over a track in the
//   depth's own units (0-1 for normalised or mist depth, scene units for Z);
// - Z Defocus's focus point on a point pad with "Pick focus in Viewer", which arms the Viewer's
//   focus pick (CompositionSession::requestFocusPick), and the polygon's blade rotation on a dial;
// - Depth Fog's falloff curve on a curve graph.
//
// Every other value is the node's own row (its number, keyframe diamond and socket; the depth
// channel a picker of R, G, B or A of an Image or Mask, or a channel set's channels by name from
// ChannelSetController's worker listing: the first channel by default, never a luminance mix). The
// rows the current mode, interpretation, shape or falloff does not read are hidden, so the card
// shows only what changes the picture.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::ui {

class AngleDialRow;
class CurveGraphCanvas;
class PointPadRow;
class RangeRow;
namespace kit {
class KButton;
class KLabel;
} // namespace kit

class DepthEffectCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    DepthEffectCardWidget(CompositionSession& session, document::NodeId node, bool card,
                          QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    // Test seams.
    [[nodiscard]] bool defocus() const noexcept { return defocus_; }
    [[nodiscard]] kit::KLabel* colorWarning() const noexcept { return colorWarning_; }
    [[nodiscard]] kit::KButton* readAsDataButton() const noexcept { return readAsData_; }
    [[nodiscard]] kit::KLabel* spreadWarning() const noexcept { return spreadWarning_; }
    [[nodiscard]] kit::KButton* pickFocusButton() const noexcept { return pickFocus_; }
    [[nodiscard]] PointPadRow* focusPad() const noexcept { return pad_; }
    [[nodiscard]] AngleDialRow* rotationDial() const noexcept { return rotation_; }
    [[nodiscard]] CurveGraphCanvas* curveGraph() const noexcept { return curve_; }
    // The range row drawn now for the near/far pair or the fog start/end (null when hidden).
    [[nodiscard]] RangeRow* nearFarRow() const;
    [[nodiscard]] RangeRow* fogRangeRow() const;
    // Applies the colour-transform fix (what "Read as data" does).
    bool readDepthAsData();
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct State final {
        std::int64_t mode = 0;
        std::int64_t interpretation = 0;
        std::int64_t focusFrom = 0;
        std::int64_t shape = 0;
        std::int64_t falloff = 0;
        std::int64_t background = 0;
        bool depthConnected = false;
        bool channelSet = false;
        friend bool operator==(const State&, const State&) = default;
    };
    [[nodiscard]] State readState() const;
    // Whether the current state reads `role` at all.
    [[nodiscard]] bool reads(std::string_view role) const;
    [[nodiscard]] std::vector<QWidget*> shownWidgets() const;
    [[nodiscard]] int widgetHeight(const QWidget* widget, int width) const;
    void refreshWarnings();
    void place();

    CompositionSession& session_;
    document::NodeId node_;
    bool defocus_ = false;
    State state_{};
    bool stateKnown_ = false;
    kit::KLabel* colorWarning_ = nullptr;
    kit::KButton* readAsData_ = nullptr;
    kit::KLabel* spreadWarning_ = nullptr;
    // Each pair: the track over 0-1 (normalised or mist depth) and over scene units (Z).
    RangeRow* nearFarUnit_ = nullptr;
    RangeRow* nearFarScene_ = nullptr;
    RangeRow* fogUnit_ = nullptr;
    RangeRow* fogScene_ = nullptr;
    PointPadRow* pad_ = nullptr;
    kit::KButton* pickFocus_ = nullptr;
    AngleDialRow* rotation_ = nullptr;
    kit::KLabel* curveLabel_ = nullptr;
    CurveGraphCanvas* curve_ = nullptr;
    QString colorWarningText_;
    QString spreadWarningText_;
};

} // namespace bloom::ui
