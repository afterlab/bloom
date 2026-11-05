#pragma once

// #541: interactive controls for the values that have a natural visual form, drawn the same on the
// node card and in Properties (node_card_widget.hpp), so no node asks for an angle, a range or a
// position as bare numbers:
//
// - AngleDialRow: an angle's row (its name, its number with its unit, its keyframe diamond, its
//   socket beside it) with a kit::KAngleDial beside the number. Drop Shadow's direction, Glow's
//   streak angle, Transform 2D's and the Layer node's rotation, Hue Shift's and the vector nodes'
//   degrees, and Radians to Degrees' radians.
// - RangeRow: a low/high pair as two value rows over one kit::KRangeSelector whose two ends drag
//   both values as one gesture (one undo step). Luma Key's low and high, Map Range's from and to,
//   Clamp's, In Range's, Random's and Wrap's min and max, and Smoothstep's edges. The ends may pass
//   each other where a reversed pair means something (a remap, a smoothstep).
// - PointPadRow: positions dragged over a picture of their frame (kit::KPointPad): a gradient's
//   start and end, a Transform 2D's pivot and offset. The numbers stay in the node's own X/Y rows
//   beside it, with their diamonds and sockets.
//
// - GradientParameterRow (gradient_parameter_row.hpp): a multi-stop gradient value on a
//   kit::KGradientEditor. The Linear and Radial Gradient generators' colours and Glow's tint
//   gradient; Colorize's Gradient Map draws the same row on its own card.
//
// Every control is a session value edit (ADR 0017): a drag is one command on
// release, a key at the playhead when the value is animated.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::ui {

class GradientParameterRow;
class GradingValueRow;
namespace kit {
class KAngleDial;
class KPointPad;
class KRangeSelector;
} // namespace kit

class AngleDialRow final : public QWidget {
    Q_OBJECT

  public:
    struct Spec final {
        std::string role;
        std::string schemaKey;
        // Where zero points and which way a positive angle turns.
        bool zeroRight = false;
        bool counterClockwise = false;
        // The stored value is in radians (the dial still reads degrees).
        bool radians = false;
        double neutral = 0.0;
    };

    AngleDialRow(CompositionSession& session, document::NodeId node, Spec spec,
                 QWidget* parent = nullptr);

    [[nodiscard]] const std::string& role() const noexcept { return spec_.role; }
    [[nodiscard]] GradingValueRow* valueRow() const noexcept { return row_; }
    [[nodiscard]] kit::KAngleDial* dial() const noexcept { return dial_; }
    void setLabelWidth(int width);
    [[nodiscard]] int labelWidthHint() const;
    [[nodiscard]] int heightHint() const;
    [[nodiscard]] int minimumWidthHint() const;
    [[nodiscard]] int socketY() const;
    void refresh(bool locked);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    CompositionSession& session_;
    document::NodeId node_;
    Spec spec_;
    GradingValueRow* row_ = nullptr;
    kit::KAngleDial* dial_ = nullptr;
};

class RangeRow final : public QWidget {
    Q_OBJECT

  public:
    struct Spec final {
        std::string lowRole;
        std::string highRole;
        std::string schemaKey;
        // The high end's own schema, when it has one (Luma Key's Low and High luma).
        std::string highSchemaKey;
        QString name;
        // The span the track shows, in stored units.
        double minimum = 0.0;
        double maximum = 1.0;
        bool mayCross = true;
    };

    RangeRow(CompositionSession& session, document::NodeId node, Spec spec,
             QWidget* parent = nullptr);

    [[nodiscard]] GradingValueRow* lowRow() const noexcept { return low_; }
    [[nodiscard]] GradingValueRow* highRow() const noexcept { return high_; }
    [[nodiscard]] kit::KRangeSelector* selector() const noexcept { return selector_; }
    [[nodiscard]] bool draws(std::string_view role) const;
    void setLabelWidth(int width);
    [[nodiscard]] int labelWidthHint() const;
    [[nodiscard]] int heightHint() const;
    [[nodiscard]] int minimumWidthHint() const;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const;
    void refresh(bool locked);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    Spec spec_;
    GradingValueRow* low_ = nullptr;
    GradingValueRow* high_ = nullptr;
    kit::KRangeSelector* selector_ = nullptr;
    // The values a selector gesture writes: the end not dragged keeps its own (perhaps outside the
    // track) value rather than the clamped one the track shows.
    double editLow_ = 0.0;
    double editHigh_ = 1.0;
    bool editing_ = false;
};

class PointPadRow final : public QWidget {
    Q_OBJECT

  public:
    struct Point final {
        std::string role;
        QString label;
        // The pad draws the value from the frame's centre (an offset) rather than from its origin.
        bool fromCentre = false;
    };
    struct Spec final {
        std::vector<Point> points;
        bool connected = false;
        // The frame the points are drawn against, read each refresh.
        std::function<QRectF()> frame;
        QString commandLabel;
    };

    PointPadRow(CompositionSession& session, document::NodeId node, Spec spec,
                QWidget* parent = nullptr);

    [[nodiscard]] kit::KPointPad* pad() const noexcept { return pad_; }
    [[nodiscard]] int heightAt(int width) const;
    [[nodiscard]] int minimumWidthHint() const;
    void refresh(bool locked);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    [[nodiscard]] std::vector<document::ParameterId> parameters() const;
    CompositionSession& session_;
    document::NodeId node_;
    Spec spec_;
    kit::KPointPad* pad_ = nullptr;
    bool editing_ = false;
};

// The node types whose card draws these controls.
[[nodiscard]] bool hasNodeControlsWidget(std::string_view typeId) noexcept;

class NodeControlsCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    NodeControlsCardWidget(CompositionSession& session, document::NodeId node,
                           std::string_view typeId, bool card, QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] const std::vector<AngleDialRow*>& angleRows() const noexcept { return angles_; }
    [[nodiscard]] const std::vector<RangeRow*>& rangeRows() const noexcept { return ranges_; }
    [[nodiscard]] PointPadRow* padRow() const noexcept { return pad_; }
    [[nodiscard]] const std::vector<GradientParameterRow*>& gradientRows() const noexcept {
        return gradients_;
    }
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    PointPadRow* pad_ = nullptr;
    std::vector<GradientParameterRow*> gradients_;
    std::vector<RangeRow*> ranges_;
    std::vector<AngleDialRow*> angles_;
};

} // namespace bloom::ui
