#pragma once

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QWidget>

#include <cstddef>
#include <optional>
#include <vector>

namespace bloom::ui::kit {

// #541: a multi-stop colour gradient, edited on the gradient itself (a Colorama-style ramp). The
// bar draws the gradient; a handle under it marks each stop and is filled with the stop's colour.
//
// - Drag a stop's handle to move it along the bar; it may pass its neighbours, which reorders the
//   stops. Ctrl (Cmd on macOS) moves it at a tenth of the speed. Pull it well below the bar to
//   remove it (it comes back if the pointer returns before the release); the editor keeps at least
//   minimumStops().
// - Press the bare bar to add a stop there, coloured as the gradient already is at that point, and
//   drag on to place it.
// - Double-click a handle (or press Enter on the selected one) to ask for its colour
//   (stopColorRequested: the owner opens its colour picker).
// - Keyboard: the selected stop carries the focus ring; Left/Right move it by kKeyStepFraction of
//   the bar (Shift or Page Up/Down ten times as far), Tab and Shift+Tab select the next and
//   previous stop before focus leaves the control, Delete removes it.
// - The right-click menu offers Add Stop, Remove Stop, Distribute Evenly and Reverse.
//
// Colours are straight RGBA in ColorSpace::Reference (scene-linear, the space the node reads),
// drawn through the owner's KColorConverter. The kit knows no document types.
//
// #543: a stop whose colour or position is set elsewhere (a link) is marked linked: its handle is
// drawn with a dashed outline in the linked kind's socket colour, and the linked part is not
// edited here (a position-linked stop is not dragged, stepped, distributed or reversed; a
// colour-linked one asks for no colour). Every stop keeps the index it had in the list setStops()
// was given (`origin`) through moves and removals, so the owner can tell which stored stop each
// one is after an edit reorders them.
//
// It keeps KSlider's edit-session contract, so properties::bindValueEdit() binds it unchanged: a
// drag, a run of keys or one menu action is one edit (editStarted(), stopsChanged() for each step,
// editFinished(), or editCancelled() after which the stops are back as they were).
class KGradientEditor final : public QWidget {
    Q_OBJECT

  public:
    struct Stop final {
        double position = 0.0;
        KColor color{0.0F, 0.0F, 0.0F, 1.0F, ColorSpace::Reference};
        // The stop's index in the list setStops() was given; none for a stop added since.
        std::optional<std::size_t> origin{};
        bool colorLinked = false;
        bool positionLinked = false;
        friend bool operator==(const Stop&, const Stop&) noexcept = default;
    };

    explicit KGradientEditor(QWidget* parent = nullptr);

    void setColorConverter(KColorConverter converter);

    // Stops are kept sorted by position (equal positions in the order given), each numbered with
    // its index in `stops` as its origin; a projection arriving mid-edit is refused.
    void setStops(std::vector<Stop> stops);
    [[nodiscard]] const std::vector<Stop>& stops() const noexcept { return stops_; }
    void setStopLimits(std::size_t minimum, std::size_t maximum);
    [[nodiscard]] std::size_t minimumStops() const noexcept { return minimumStops_; }
    [[nodiscard]] std::size_t maximumStops() const noexcept { return maximumStops_; }

    [[nodiscard]] std::optional<std::size_t> selectedStop() const noexcept { return selected_; }
    void setSelectedStop(std::optional<std::size_t> index);

    // One complete edit each (editStarted, stopsChanged, editFinished).
    bool addStop(double position);
    bool removeStop(std::size_t index);
    bool setStopPosition(std::size_t index, double position);
    bool setStopColor(std::size_t index, const KColor& color);
    void distributeStops();
    void reverseStops();

    // The colour at `position` in [0, 1], straight Reference RGBA (linear between stops, held past
    // the ends), and the same drawn for the display.
    [[nodiscard]] KColor colorAt(double position) const;
    [[nodiscard]] QColor displayColorAt(double position) const;

    [[nodiscard]] bool hasLiveEdit() const noexcept { return interactionActive_; }
    // Programmatic cancellation: the stops the edit started from come back.
    void cancelEdit();
    // True while a dragged stop is pulled far enough off the bar to be removed on release.
    [[nodiscard]] bool removalPending() const noexcept { return removalPending_; }

    // Geometry (test seams).
    [[nodiscard]] QRectF barRect() const;
    [[nodiscard]] QPointF stopCentre(std::size_t index) const;
    [[nodiscard]] std::optional<std::size_t> stopAt(QPointF point) const;
    [[nodiscard]] double positionAt(double x) const;

    [[nodiscard]] State visualState() const;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void stopsChanged();
    void selectedStopChanged();
    void stopColorRequested(std::size_t index);
    void editStarted();
    void editFinished();
    void editCancelled();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    bool event(QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void beginInteraction();
    void finishInteraction(bool keep);
    // Moves stop `index` to `position` and re-sorts, keeping it selected; returns its new index.
    std::size_t moveStop(std::size_t index, double position);
    // Stable-sorts the stops by position, keeping the selected stop selected.
    void resort();
    void emitChanged();
    [[nodiscard]] QColor displayed(const KColor& color) const;

    KColorConverter converter_;
    std::vector<Stop> stops_;
    std::vector<Stop> base_;
    std::size_t minimumStops_ = 2;
    std::size_t maximumStops_ = 64;
    std::optional<std::size_t> selected_;
    std::optional<std::size_t> dragged_;
    // The dragged stop as it was before it was pulled off the bar.
    std::optional<Stop> pulled_;
    double grabOffset_ = 0.0;
    double segmentX_ = 0.0;
    double segmentBase_ = 0.0;
    Qt::KeyboardModifiers segmentModifiers_;
    bool removalPending_ = false;
    bool moved_ = false;
    bool keyStepping_ = false;
    bool hovered_ = false;
    bool interactionActive_ = false;
};

} // namespace bloom::ui::kit
