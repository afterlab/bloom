#include <bloom/ui/crash_notice.hpp>

#include "reveal_in_file_manager.hpp"

#include <bloom/crash/crash_reporter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QLocale>
#include <QMetaObject>
#include <QUrl>

#include <chrono>
#include <utility>

namespace bloom::ui {
namespace {

[[nodiscard]] QString programDescription(const std::string& program) {
    if (program == "bloom")
        return QObject::tr("Bloom");
    if (program == "bloom-cli")
        return QObject::tr("The bloom-cli command-line tool");
    if (program == "bloom-mcp")
        return QObject::tr("The bloom-mcp agent server");
    return QString::fromStdString(program);
}

[[nodiscard]] QString whenText(const std::chrono::system_clock::time_point time) {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(seconds), QLocale::ShortFormat);
}

} // namespace

QString crashNoticeDetail(const std::vector<crash::CrashReport>& reports) {
    if (reports.empty())
        return {};
    const auto& newest = reports.front();
    QString detail =
        QObject::tr("%1 quit unexpectedly on %2. A crash report was saved on this "
                    "computer; nothing was sent anywhere. Sharing it helps fix the "
                    "problem: Report on GitHub opens a new issue with the text report, "
                    "and you can attach the minidump from the crash folder.")
            .arg(programDescription(newest.program), whenText(newest.time));
    if (reports.size() > 1)
        detail += QStringLiteral("\n\n") +
                  QObject::tr("%n more crash report(s) since the last notice are in the crash "
                              "folder.",
                              nullptr, static_cast<int>(reports.size() - 1));
    return detail;
}

CrashNotice::CrashNotice(QWidget& owner, Options options, QObject* parent)
    : QObject(parent), owner_(owner), options_(std::move(options)) {
    if (!options_.waitForCollection)
        options_.waitForCollection = [] { crash::waitForCollection(); };
}

CrashNotice::~CrashNotice() {
    if (reader_.joinable())
        reader_.join();
    if (marker_.joinable())
        marker_.join();
}

void CrashNotice::start() {
    if (reader_.joinable() || options_.directory.empty()) {
        Q_EMIT finished(false);
        return;
    }
    // The folder is read and the report's text loaded on a worker thread; the answer comes back
    // to this object on the UI thread.
    reader_ = core::WorkerThread(
        [this, directory = options_.directory, wait = options_.waitForCollection] {
            std::vector<crash::CrashReport> reports;
            std::string text;
            try {
                wait();
                reports = crash::reportsToOffer(directory);
                if (!reports.empty())
                    text = crash::readTextReport(reports.front());
            } catch (...) {
                reports.clear();
            }
            QMetaObject::invokeMethod(
                this,
                [this, reports = std::move(reports), text = std::move(text)]() mutable {
                    present(std::move(reports), std::move(text));
                },
                Qt::QueuedConnection);
        });
}

void CrashNotice::present(std::vector<crash::CrashReport> reports, std::string text) {
    if (reports.empty() || (options_.mayShow && !options_.mayShow())) {
        Q_EMIT finished(false);
        return;
    }
    const kit::KConfirmSpec spec{
        .title = tr("Bloom"),
        .objectName = QStringLiteral("crashReportNotice"),
        .tone = kit::KPromptTone::Warning,
        .heading = tr("Bloom quit unexpectedly"),
        .detail = crashNoticeDetail(reports),
        .detailToolTip = {},
        .document = {},
        .acceptText = tr("Report on GitHub"),
        .destructiveText = {},
        .alternateText = {},
        .rejectText = tr("Dismiss"),
        .defaultRole = kit::KDialog::Role::Reject,
        // Never blocks the window: it may appear while an agent is already working.
        .modality = Qt::NonModal,
    };
    auto* dialog = kit::buildConfirm(owner_, spec);
    // Copy Report and Open Crash Folder leave the notice open; Report on GitHub and Dismiss close
    // it. Reporting also shows the minidump in its folder, ready to attach to the issue.
    (void)dialog->addButton(kit::KDialog::Role::Apply, tr("Copy Report"));
    (void)dialog->addButton(kit::KDialog::Role::Reset, tr("Open Crash Folder"));
    const auto newest = reports.front();
    const QString reportText = QString::fromStdString(text);
    connect(dialog, &kit::KDialog::applyRequested, dialog, [reportText] {
        if (auto* clipboard = QGuiApplication::clipboard())
            clipboard->setText(reportText);
    });
    connect(dialog, &kit::KDialog::resetRequested, dialog,
            [dialog, minidump = newest.minidump()] { revealInFileManager(minidump, dialog); });
    QPointer<QWidget> owner(&owner_);
    dialog_ = dialog;
    dialog->open([owner, minidump = newest.minidump(), text](const kit::KDialog::Role role) {
        if (role != kit::KDialog::Role::Accept)
            return;
        QDesktopServices::openUrl(QUrl(QString::fromStdString(crash::newIssueAddress(text))));
        if (owner)
            revealInFileManager(minidump, owner);
    });

    // Offered once: the next start does not offer these crashes again. A file write, so off the
    // UI thread.
    if (marker_.joinable())
        marker_.join();
    marker_ = core::WorkerThread([this, reports = std::move(reports)] {
        for (const auto& report : reports) {
            try {
                static_cast<void>(crash::markOffered(report));
            } catch (...) {
            }
        }
        QMetaObject::invokeMethod(this, [this] { Q_EMIT finished(true); }, Qt::QueuedConnection);
    });
}

} // namespace bloom::ui
