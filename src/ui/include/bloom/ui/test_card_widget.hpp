#pragma once

// #587: the Test Card node's card widget, drawn the same on the node card and in Properties. It
// owns which of the node's rows show: only the controls of the chosen card (and Width and Height
// for a Custom size). A Patch also shows what it should look like: its signal and the display value
// the Viewer's current Display and View should produce, to compare a probe reading with. The
// verification is of the pipeline from composition to monitor, not a display calibration: a
// mismatch can come from Bloom, the desktop compositor, the GPU output settings or the monitor's
// profile.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QString>

#include <memory>
#include <optional>
#include <string_view>

namespace bloom::ui {

namespace kit {
class KLabel;
}

class TestCardCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    TestCardCardWidget(CompositionSession& session, document::NodeId node, bool card,
                       QWidget* parent = nullptr);
    ~TestCardCardWidget() override;

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

    // The readout's lines, for tests.
    [[nodiscard]] QString signalText() const;
    [[nodiscard]] QString expectedText() const;
    [[nodiscard]] bool showsReadout() const noexcept { return patch_; }
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    struct State;
    void place();
    void expectationReady(quint64 generation, QString expected, QColor swatch);
    CompositionSession& session_;
    document::NodeId node_;
    std::unique_ptr<State> state_;
    kit::KLabel* signal_ = nullptr;
    kit::KLabel* expected_ = nullptr;
    kit::KLabel* note_ = nullptr;
    bool patch_ = false;
    QColor swatch_;
};

} // namespace bloom::ui
