#pragma once

// #527: the Dither node's card widget, drawn and edited the same on the node card and in Properties:
//
// - a preview (DitherPreview): a dark-to-light ramp quantized without the dither (top) and with it
//   (bottom), drawn with the node's own pattern, matrix size, seed and amplitude. It is quantized to
//   4 bits only so its steps show at card size; the node quantizes nothing itself;
// - Amplitude, a grading slider row in LSBs, and under it a readout of what that is in the chosen
//   bit depth and domain ("1.00 LSB of 8-bit = 0.00392 of full scale, display encoded").
//
// The pattern and the domain (labelled radios), the matrix size (shown for Ordered only), the bit
// depth, the animation, the seed (with its Randomize button) and Monochrome are the node's own rows.
// Every control follows the shared grading rules (grading_controls.hpp): relative drags, Ctrl a
// tenth and Shift ten times the speed, a double-click resets, one gesture is one undo step, and the
// amplitude keeps its keyframe diamond and its socket beside its row.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QImage>
#include <QString>
#include <QWidget>

#include <optional>
#include <string_view>

namespace bloom::ui {

class GradingValueRow;
namespace kit {
class KLabel;
}

class DitherPreview final : public QWidget {
    Q_OBJECT

  public:
    DitherPreview(CompositionSession& session, document::NodeId node, QWidget* parent = nullptr);

    [[nodiscard]] static int fixedHeight();
    // The levels the preview quantizes to (4 bits), so its steps show at card size.
    static constexpr int kPreviewSteps = 15;
    // The picture, at `width` by `height` pixels: the plain ramp's steps on the upper half, the
    // dithered ramp's on the lower (a test seam).
    [[nodiscard]] QImage preview(int width, int height) const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    CompositionSession& session_;
    document::NodeId node_;
};

class DitherCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    DitherCardWidget(CompositionSession& session, document::NodeId node, bool card,
                     QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] DitherPreview* preview() const noexcept { return preview_; }
    [[nodiscard]] GradingValueRow* amplitudeRow() const noexcept { return amplitude_; }
    // What the amplitude is in the chosen bit depth and domain (a test seam).
    [[nodiscard]] QString readout() const;
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    DitherPreview* preview_ = nullptr;
    GradingValueRow* amplitude_ = nullptr;
    kit::KLabel* readout_ = nullptr;
    // Whether the pattern is Ordered (Bayer), the one that reads the matrix size.
    bool ordered_ = false;
};

} // namespace bloom::ui
