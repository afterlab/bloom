#pragma once

// #507: the Lens Flare node's card widget, drawn and edited the same on the node card and in
// Properties:
//
// - Preset: a starting look, one undo step (document::lensFlarePresets);
// - the light on a point pad (PointPadRow, kit::KPointPad) over a picture of the frame showing where
//   the elements land as the node computes them (document::lensFlareLayout), so dragging the light
//   or changing a size redraws the flare. The light's own X/Y row, with its diamond and socket,
//   stays beside the card's other rows;
// - the rotation on a dial, and Intensity, Size and Chromatic split, each a grading slider row;
// - an Element picker that shows one part of the stack at a time: the light source's own readings
//   (the 3D position, the brightness from the source, the occlusion) or one element's values. The
//   picker is a view of the node, not part of it.
//
// The light colour and the anamorphic colour are colour chips (the picker). A count, a pattern or a
// choice is the node's own row, shown while its part is picked. A linked value always shows, so every
// link has its socket. Every control follows the shared grading rules (grading_controls.hpp).

#include <bloom/document/ids.hpp>
#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QImage>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

class QMenu;

namespace bloom::ui {

class AngleDialRow;
class ColorizeColorRow;
class GradingValueRow;
class PointPadRow;
namespace kit {
class KButton;
class KDropdown;
} // namespace kit

class LensFlareCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    LensFlareCardWidget(CompositionSession& session, document::NodeId node, bool card,
                        QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    // Test seams.
    [[nodiscard]] PointPadRow* padRow() const noexcept { return pad_; }
    [[nodiscard]] AngleDialRow* rotationDial() const noexcept { return rotation_; }
    [[nodiscard]] kit::KButton* presetButton() const noexcept { return presets_; }
    [[nodiscard]] kit::KDropdown* elementPicker() const noexcept { return element_; }
    [[nodiscard]] GradingValueRow* valueRow(std::string_view role) const;
    [[nodiscard]] ColorizeColorRow* colorRow(std::string_view role) const;
    // The part of the stack the picker shows.
    [[nodiscard]] document::LensFlareGroup group() const noexcept { return group_; }
    void setGroup(document::LensFlareGroup group);
    // The picture the pad draws, at `width` pixels across.
    [[nodiscard]] QImage preview(int width) const;
    // Applies preset `index` of document::lensFlarePresets (what its menu entry does).
    bool applyPreset(std::size_t index);
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct State final {
        std::int64_t lightMode = 0;
        bool occlusionConnected = false;
        friend bool operator==(const State&, const State&) = default;
    };
    [[nodiscard]] State readState() const;
    // Whether the value `role` shows now: its part is picked and the state reads it.
    [[nodiscard]] bool shown(const document::LensFlareParameterSpec& spec) const;
    [[nodiscard]] std::vector<GradingValueRow*> shownRows() const;
    [[nodiscard]] QMenu* makePresetMenu();
    void place();

    CompositionSession& session_;
    document::NodeId node_;
    document::LensFlareGroup group_ = document::LensFlareGroup::Light;
    State state_{};
    bool stateKnown_ = false;
    kit::KButton* presets_ = nullptr;
    PointPadRow* pad_ = nullptr;
    AngleDialRow* rotation_ = nullptr;
    kit::KDropdown* element_ = nullptr;
    std::vector<GradingValueRow*> always_;
    std::vector<GradingValueRow*> values_;
    ColorizeColorRow* lightColor_ = nullptr;
    ColorizeColorRow* anamorphicColor_ = nullptr;
};

} // namespace bloom::ui
