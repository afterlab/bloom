#include "update_window.hpp"

#include "window_status_bar_segments.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QDesktopServices>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace bloom::ui {

namespace {

[[nodiscard]] QString toQString(const std::string& text) { return QString::fromStdString(text); }

} // namespace

QString UpdateWindow::refusalSentence(const update::UpdateRefusal refusal) {
    using R = update::UpdateRefusal;
    switch (refusal) {
    case R::None:
        return {};
    case R::NotConfigured:
        return tr("This build of Bloom was not set up to check for updates.");
    case R::FeedTooLarge:
        return tr("The update list is larger than an update list can be.");
    case R::FeedMalformed:
        return tr("The update list is not in the form Bloom reads.");
    case R::FeedUnsigned:
        return tr("The update list carries no signature.");
    case R::FeedUnknownKey:
        return tr("The update list is signed with a key this version of Bloom does not trust.");
    case R::FeedSignatureInvalid:
        return tr("The signature on the update list does not match what it says.");
    case R::FeedContentMalformed:
        return tr("The update list is signed, but what it says is not in a form this version of "
                  "Bloom understands.");
    case R::FeedChannelMismatch:
        return tr("The update list is for another release channel.");
    case R::FeedAddressRefused:
        return tr("The update list names a download address that is not secure.");
    case R::FileUnreadable:
        return tr("The downloaded file could not be read.");
    case R::FileSizeMismatch:
        return tr("The downloaded file is not the size the update list gives.");
    case R::FileChecksumMismatch:
        return tr("The downloaded file does not match its checksum.");
    case R::FileSignatureInvalid:
        return tr("The signature on the downloaded file does not match the file.");
    case R::VerificationUnavailable:
        return tr("This copy of Bloom cannot check signatures because a file of its installation "
                  "is missing. Install Bloom again.");
    case R::Cancelled:
        return tr("The check was cancelled.");
    }
    return {};
}

QString UpdateWindow::progressText(const std::uint64_t received, const std::uint64_t total) {
    if (total == 0) {
        return statusbar::formatBinaryBytes(received);
    }
    return tr("%1 of %2").arg(statusbar::formatBinaryBytes(received),
                              statusbar::formatBinaryBytes(total));
}

UpdateWindow::UpdateWindow(UpdateController& controller, QWidget& owner)
    : kit::KDialog(tr("Check for Updates"), QStringLiteral("updateWindow"), owner, Qt::NonModal),
      controller_(controller) {
    title_ = new kit::KLabel(QString{}, this, kit::TypeRole::Title);
    title_->setObjectName(QStringLiteral("updateTitle"));
    title_->setWordWrap(true);
    body_ = new kit::KLabel(QString{}, this);
    body_->setObjectName(QStringLiteral("updateBody"));
    body_->setWordWrap(true);
    note_ = new kit::KLabel(QString{}, this);
    note_->setObjectName(QStringLiteral("updateNote"));
    note_->setWordWrap(true);
    note_->setTone(kit::Color::Warn);

    notes_ = new QTextBrowser(this);
    notes_->setObjectName(QStringLiteral("updateReleaseNotes"));
    notes_->setReadOnly(true);
    // A link in the notes opens in the browser, and only an https address does.
    notes_->setOpenLinks(false);
    notes_->setOpenExternalLinks(false);
    kit::styleDocumentView(*notes_);
    notes_->setFont(kit::font(kit::TypeRole::UiSmall));
    notes_->setMinimumHeight(kit::px(kit::Size::DiagnosticHeight));
    connect(notes_, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
        if (url.scheme() == QStringLiteral("https")) {
            QDesktopServices::openUrl(url);
        }
    });

    progress_ = new kit::KProgressBar(this);
    progress_->setObjectName(QStringLiteral("updateProgress"));
    progressText_ = new kit::KLabel(QString{}, this, kit::TypeRole::UiSmall);
    progressText_->setObjectName(QStringLiteral("updateProgressText"));
    progressText_->setTone(kit::Color::Muted);

    auto* layout = contentLayout();
    layout->addWidget(title_);
    layout->addWidget(body_);
    layout->addWidget(note_);
    layout->addWidget(notes_, 1);
    layout->addWidget(progress_);
    layout->addWidget(progressText_);
    layout->addStretch(1);

    primary_ = addButton(Role::Apply, QString{});
    primary_->setVariant(kit::KButton::Variant::Primary);
    primary_->setObjectName(QStringLiteral("updatePrimaryButton"));
    cancel_ = addButton(Role::Reset, tr("Cancel"));
    cancel_->setObjectName(QStringLiteral("updateCancelButton"));
    close_ = addButton(Role::Reject, tr("Close"));
    close_->setObjectName(QStringLiteral("updateCloseButton"));

    connect(this, &KDialog::applyRequested, this, [this] {
        switch (primaryFor_) {
        case UpdateScreen::Offer:
            controller_.startDownload();
            break;
        case UpdateScreen::Ready:
            controller_.restartAndUpdate();
            break;
        case UpdateScreen::Unreachable:
            controller_.checkNow();
            break;
        case UpdateScreen::RunInstallerAgain:
        case UpdateScreen::DownloadManually:
            controller_.openDownloadPage();
            break;
        default:
            break;
        }
    });
    connect(this, &KDialog::resetRequested, this, [this] { controller_.cancel(); });
    connect(&controller_, &UpdateController::changed, this, [this] { refresh(); });
    refresh();
}

QString UpdateWindow::downloadNoteText() const {
    switch (controller_.downloadNote()) {
    case UpdateDownloadNote::None:
        return {};
    case UpdateDownloadNote::Cancelled:
        return tr("The download was cancelled.");
    case UpdateDownloadNote::Unreachable:
        return tr("The download could not be completed. Check your connection and try again.");
    case UpdateDownloadNote::Refused:
        return tr("The downloaded file was refused, so nothing was installed. %1")
            .arg(refusalSentence(controller_.downloadRefusal()));
    case UpdateDownloadNote::NotPrepared:
        return tr("The download finished, but the new version could not be prepared. Nothing was "
                  "installed.");
    }
    return {};
}

void UpdateWindow::refresh() {
    const auto screen = controller_.screen();
    const auto current = toQString(controller_.currentVersion());
    const auto next = toQString(controller_.newVersion());
    QString title;
    QString body;
    QString note;
    QString primary;
    QString closeText = tr("Close");
    bool showNotes = false;
    bool showProgress = false;
    bool showCancel = false;

    switch (screen) {
    case UpdateScreen::Checking:
        title = tr("Checking for updates…");
        body = tr("Bloom is asking whether a newer version exists.");
        showProgress = true;
        showCancel = true;
        break;
    case UpdateScreen::UpToDate:
        title = tr("Bloom is up to date");
        body = tr("Bloom %1 is the newest version.").arg(current);
        break;
    case UpdateScreen::NotPublished:
        title = tr("Bloom %1 is the newest version for this system").arg(current);
        body = tr("A newer version exists, but it is not published for this system yet.");
        break;
    case UpdateScreen::Offer:
        title = tr("Bloom %1 is available").arg(next);
        body = tr("You have %1.").arg(current);
        note = downloadNoteText();
        primary = tr("Download and Install");
        closeText = tr("Later");
        showNotes = !controller_.releaseNotes().empty();
        break;
    case UpdateScreen::Downloading:
        title = tr("Downloading Bloom %1").arg(next);
        body = tr("You can close this window. The download goes on and shows in the Jobs panel.");
        showProgress = true;
        showCancel = true;
        break;
    case UpdateScreen::Ready:
        title = tr("Bloom %1 is ready").arg(next);
        body = tr("Bloom closes, installs it and opens again. If the new version cannot start, "
                  "the version you have now is put back.");
        primary = tr("Restart and Update");
        closeText = tr("Later");
        break;
    case UpdateScreen::Refused:
        title = tr("Update not installed");
        body = tr("The update list could not be verified, so nothing was installed. %1")
                   .arg(refusalSentence(controller_.refusal()));
        break;
    case UpdateScreen::Unreachable:
        title = tr("Bloom could not reach the update server");
        body = tr("Check your connection and try again.");
        primary = tr("Try Again");
        break;
    case UpdateScreen::NotConfigured:
        title = tr("Updates are not set up");
        body = tr("This build of Bloom was not set up to check for updates.");
        break;
    case UpdateScreen::BuildFolder:
        title = tr("Updates are off for this copy");
        body = tr("This copy of Bloom runs from a build folder and is not updated.");
        break;
    case UpdateScreen::PackageManager: {
        const auto manager = toQString(controller_.packageManager());
        title = tr("Bloom %1 is available").arg(next);
        body = tr("This copy of Bloom was installed by %1. Update it there.")
                   .arg(manager.isEmpty() ? tr("your package manager") : manager);
        break;
    }
    case UpdateScreen::RunInstallerAgain:
        title = tr("Bloom %1 is available").arg(next);
        body = tr("This copy is installed for all users of this computer, so it cannot update "
                  "itself. Run the installer again to update it.");
        primary = tr("Open the Download Page");
        break;
    case UpdateScreen::DownloadManually:
        title = tr("Bloom %1 is available").arg(next);
        body = tr("You have %1. Download the new version from the releases page.").arg(current);
        primary = tr("Open the Download Page");
        break;
    }

    primaryFor_ = screen;
    title_->setText(title);
    body_->setText(body);
    note_->setText(note);
    note_->setVisible(!note.isEmpty());
    if (showNotes) {
        notes_->setMarkdown(toQString(controller_.releaseNotes()));
    } else {
        notes_->clear();
    }
    notes_->setVisible(showNotes);
    progress_->setVisible(showProgress);
    progressText_->setVisible(showProgress && screen == UpdateScreen::Downloading);
    if (screen == UpdateScreen::Downloading) {
        progress_->setProgress(controller_.downloadReceived(), controller_.downloadTotal());
        progressText_->setText(
            progressText(controller_.downloadReceived(), controller_.downloadTotal()));
    } else if (showProgress) {
        progress_->setBusy();
    }
    primary_->setText(primary);
    primary_->setVisible(!primary.isEmpty());
    cancel_->setVisible(showCancel);
    close_->setText(closeText);
    close_->setVisible(!showCancel);
    setDefaultRole(primary.isEmpty() ? Role::Reject : Role::Apply);
}

} // namespace bloom::ui
