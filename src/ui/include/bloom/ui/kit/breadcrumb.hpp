#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <vector>

namespace bloom::ui::kit {

// A breadcrumb: where you are, outermost first, as a row of flat (Ghost) crumbs separated by
// chevrons. Every crumb but the last is a place to go back to; clicking one, or pressing Enter or
// Space on the focused one, emits crumbActivated(index). The last crumb names where you are and
// does nothing. `suffix` is a non-interactive note after the last crumb ("Points").
//
// When the row is narrower than the whole path, the middle crumbs elide first, outermost middle
// first, into one "…" that still goes to the innermost crumb it stands for; only then do the
// first and last crumbs elide their own text. The tooltip always carries the full path, so
// nothing is lost to the eliding.
//
// Keyboard: Tab reaches the row as one stop; Left and Right move between the crumbs, Home and End
// to the first and last, Enter or Space activates the focused crumb.
class KBreadcrumb final : public QWidget {
    Q_OBJECT

  public:
    struct Crumb final {
        QString text;
        // The crumb's own tooltip; empty uses its text.
        QString toolTip;
    };

    explicit KBreadcrumb(QWidget* parent = nullptr);

    void setCrumbs(std::vector<Crumb> crumbs, QString suffix = {});
    [[nodiscard]] const std::vector<Crumb>& crumbs() const noexcept { return crumbs_; }
    [[nodiscard]] const QString& suffix() const noexcept { return suffix_; }
    // The whole path in words: "Main ▸ Logo ▸ Badge · Points".
    [[nodiscard]] QString pathText() const;

    // What is drawn now, one entry per visible crumb: the text it shows (elided or "…") and the
    // crumb index a click on it goes to. Public so tests read what the artist sees.
    struct Shown final {
        QString text;
        int target = -1;
        QRect rect;
        bool elided = false;
    };
    [[nodiscard]] std::vector<Shown> shown() const;
    [[nodiscard]] int focusedCrumb() const noexcept { return focused_; }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void crumbActivated(int index);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] int textWidth(const QString& text) const;
    [[nodiscard]] int crumbWidth(const QString& text) const;
    [[nodiscard]] int chevronWidth() const;
    [[nodiscard]] int suffixWidth() const;
    [[nodiscard]] std::vector<Shown> layoutFor(int width) const;
    [[nodiscard]] int shownIndexAt(const QPoint& point) const;
    [[nodiscard]] int shownIndexOfTarget(int target) const;
    void relayout();
    // A widget resized while hidden gets its resize event only when shown: lay out again first.
    void relayoutIfResized();
    void moveFocus(int step);
    void activate(int target);

    std::vector<Crumb> crumbs_;
    QString suffix_;
    std::vector<Shown> shown_;
    int layoutWidth_ = -1;
    int hovered_ = -1;
    int pressed_ = -1;
    // The crumb index keyboard focus is on (a target of a shown entry).
    int focused_ = -1;
};

} // namespace bloom::ui::kit
