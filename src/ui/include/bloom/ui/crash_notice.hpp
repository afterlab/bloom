#pragma once

// The next-start notice after a crash (issue #736; docs/architecture/platform-support.md,
// "Crash Reports"): "Bloom quit unexpectedly", with Open Crash Folder, Copy Report and Report on
// GitHub, and Dismiss.
//
// The crash folder is read off the UI thread, after the start's own collection (src/crash). A
// notice is shown at most once per crash: offering it records that it was offered, so the next
// start does not offer it again. It is never modal, and it is not shown at all while the session
// is headless or an agent drives it (`mayShow` answers false): the crash then stays in the folder
// for the next interactive start.

#include <bloom/core/worker_thread.hpp>
#include <bloom/crash/crash_reports.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <QObject>
#include <QPointer>
#include <QString>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class QWidget;

namespace bloom::ui {

// The notice's detail text for these reports (newest first). Pure, for tests.
[[nodiscard]] QString crashNoticeDetail(const std::vector<crash::CrashReport>& reports);

class CrashNotice final : public QObject {
    Q_OBJECT

  public:
    struct Options final {
        std::filesystem::path directory;
        // Asked on the UI thread just before the notice would appear.
        std::function<bool()> mayShow;
        // Waits for the start's collection before the folder is read (the default waits for
        // bloom::crash::startCrashReporter's; a test hands its own).
        std::function<void()> waitForCollection;
    };

    CrashNotice(QWidget& owner, Options options, QObject* parent = nullptr);
    ~CrashNotice() override;
    CrashNotice(const CrashNotice&) = delete;
    CrashNotice& operator=(const CrashNotice&) = delete;

    // Reads the folder off the UI thread and shows the notice when there is a crash to offer.
    void start();
    // The open notice, or null.
    [[nodiscard]] kit::KDialog* dialog() const noexcept { return dialog_; }

  Q_SIGNALS:
    // The look is over: `shown` is whether a notice appeared (and its crashes are now recorded
    // as offered).
    void finished(bool shown);

  private:
    void present(std::vector<crash::CrashReport> reports, std::string text);

    QWidget& owner_;
    Options options_;
    core::WorkerThread reader_;
    core::WorkerThread marker_;
    QPointer<kit::KDialog> dialog_;
};

} // namespace bloom::ui
