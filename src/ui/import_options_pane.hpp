#pragma once

// Import's options (#381), the pane under the Import Media dialog's file type row, After Effects'
// import options: import numbered images as one sequence or each as a still, only a frame range of
// each sequence, skip a sequence's missing frames, and make a composition from what was imported.
// The pane only edits values; AssetController turns them into the ImportAssets command and the
// composition into its follow-up. The check boxes are remembered for the next import (user
// settings, never project data); the frame range is not.

#include <bloom/commands/asset_operations.hpp>

#include <QWidget>

namespace bloom::ui {
namespace kit {
class KCheckBox;
class KValueField;
} // namespace kit

class ImportOptionsPane final : public QWidget {
    Q_OBJECT
  public:
    // `offerComposition` false hides "Create a composition" (an import that already adds layers).
    explicit ImportOptionsPane(bool offerComposition, QWidget* parent = nullptr);

    // The sequence, frame range and missing-frame options; the preferences are the caller's.
    [[nodiscard]] commands::ImportOptions options() const;
    [[nodiscard]] bool createComposition() const;
    // Stores the check boxes for the next import.
    void remember() const;

    [[nodiscard]] kit::KCheckBox* sequence() const noexcept { return sequence_; }
    [[nodiscard]] kit::KCheckBox* skipMissing() const noexcept { return skip_; }
    [[nodiscard]] kit::KCheckBox* range() const noexcept { return range_; }
    [[nodiscard]] kit::KValueField* firstFrame() const noexcept { return first_; }
    [[nodiscard]] kit::KValueField* lastFrame() const noexcept { return last_; }
    [[nodiscard]] kit::KCheckBox* composition() const noexcept { return composition_; }

  private:
    void refresh();

    kit::KCheckBox* sequence_ = nullptr;
    kit::KCheckBox* skip_ = nullptr;
    kit::KCheckBox* range_ = nullptr;
    kit::KValueField* first_ = nullptr;
    kit::KValueField* last_ = nullptr;
    kit::KCheckBox* composition_ = nullptr;
};

} // namespace bloom::ui
