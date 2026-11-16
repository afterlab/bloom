#pragma once

#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/update_controller.hpp>

#include <QString>

class QTextBrowser;

namespace bloom::ui {

namespace kit {
class KButton;
class KLabel;
class KProgressBar;
} // namespace kit

// Where releases are published; the window opens it for a copy that is updated by hand.
inline constexpr char kReleasesPageUrl[] = "https://github.com/afterlab/bloom/releases";

// "Check for Updates…" (ADR 0041, #734): one small, non-modal window whose content follows the
// controller. Closing it never cancels a download: that runs as a job and shows again when the
// window is opened. The window only reads the controller and presses its commands.
class UpdateWindow final : public kit::KDialog {
    Q_OBJECT

  public:
    UpdateWindow(UpdateController& controller, QWidget& owner);

    // The sentence for a refusal of a feed or a file, in plain words, without the typed name.
    [[nodiscard]] static QString refusalSentence(update::UpdateRefusal refusal);
    // "12.5 MiB of 80.0 MiB".
    [[nodiscard]] static QString progressText(std::uint64_t received, std::uint64_t total);

  private:
    void refresh();
    [[nodiscard]] QString downloadNoteText() const;

    UpdateController& controller_;
    kit::KLabel* title_ = nullptr;
    kit::KLabel* body_ = nullptr;
    kit::KLabel* note_ = nullptr;
    QTextBrowser* notes_ = nullptr;
    kit::KProgressBar* progress_ = nullptr;
    kit::KLabel* progressText_ = nullptr;
    kit::KButton* primary_ = nullptr;
    kit::KButton* cancel_ = nullptr;
    kit::KButton* close_ = nullptr;
    // What the primary button does now.
    UpdateScreen primaryFor_ = UpdateScreen::Checking;
};

} // namespace bloom::ui
