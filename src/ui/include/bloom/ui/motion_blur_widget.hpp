#pragma once

// #484: the motion-blur card widget, drawn and edited the same on the node card and in Properties:
//
// - the shutter angle on a dial (and, for a Custom shutter, its phase), the shutter curve on a curve
//   editor when the curve is Custom;
// - Directional Blur's direction on a dial, Spin's turn on a dial, Spin's and Zoom's centre on a
//   point pad (the Viewer draws the same values as handles, viewer_editor_motion.cpp);
// - what the current footage mode, quality, convention, offset, curve or connection does not read
//   is hidden, so the card shows only what changes the picture;
// - how many times a Motion Blur renders its upstream a frame, because that is its cost.
//
// Every other value is the node's own row (its number, keyframe diamond and socket).

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::ui {

class AngleDialRow;
class CompositionSession;
class CurveGraphCanvas;
class PointPadRow;
namespace kit {
class KLabel;
} // namespace kit

// Whether the current values of a motion-blur node (or a Transform 2D's motion blur) read `role`.
[[nodiscard]] bool motionRoleRead(const CompositionSession& session, document::NodeId node,
                                  std::string_view typeId, std::string_view role);

class MotionBlurCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    MotionBlurCardWidget(CompositionSession& session, document::NodeId node,
                         std::string_view typeId, bool card, QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    // Test seams.
    [[nodiscard]] AngleDialRow* shutterDial() const noexcept { return shutter_; }
    [[nodiscard]] AngleDialRow* phaseDial() const noexcept { return phase_; }
    [[nodiscard]] AngleDialRow* directionDial() const noexcept { return direction_; }
    [[nodiscard]] PointPadRow* centrePad() const noexcept { return pad_; }
    [[nodiscard]] CurveGraphCanvas* curveEditor() const noexcept { return curve_; }
    [[nodiscard]] kit::KLabel* costLabel() const noexcept { return cost_; }
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    [[nodiscard]] std::vector<QWidget*> shownWidgets() const;
    [[nodiscard]] int widgetHeight(const QWidget* widget, int width) const;
    [[nodiscard]] std::vector<bool> readState() const;
    void place();

    CompositionSession& session_;
    document::NodeId node_;
    std::string typeId_;
    std::vector<bool> state_;
    AngleDialRow* shutter_ = nullptr;
    AngleDialRow* phase_ = nullptr;
    AngleDialRow* direction_ = nullptr;
    PointPadRow* pad_ = nullptr;
    kit::KLabel* curveLabel_ = nullptr;
    CurveGraphCanvas* curve_ = nullptr;
    kit::KLabel* cost_ = nullptr;
};

} // namespace bloom::ui
