#pragma once

// #642: the chooser an SVG import or drop offers, once for the whole batch. As one layer is the
// default answer (a single vector layer that stays crisp at any scale); As editable shapes is the
// deliberate choice (a shape layer and a path node per element). "Remember my choice" stores the
// answer as the import preference (Preferences > Import changes it again).

#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <cstddef>

namespace bloom::ui {
namespace kit {
class KCheckBox;
class KLabel;
class KRadioGroup;
} // namespace kit

class SvgImportModeDialog final : public kit::KDialog {
    Q_OBJECT
  public:
    // `count` is the number of SVG files the answer applies to.
    SvgImportModeDialog(std::size_t count, commands::SvgImportMode initial, QWidget& owner);

    [[nodiscard]] commands::SvgImportMode mode() const;
    [[nodiscard]] bool remember() const;

    // For tests.
    [[nodiscard]] kit::KRadioGroup* choices() const noexcept { return choices_; }
    [[nodiscard]] kit::KCheckBox* rememberBox() const noexcept { return remember_; }
    [[nodiscard]] kit::KLabel* note() const noexcept { return note_; }

  private:
    void refreshNote();

    kit::KRadioGroup* choices_ = nullptr;
    kit::KCheckBox* remember_ = nullptr;
    kit::KLabel* note_ = nullptr;
};

} // namespace bloom::ui
