// The next-start crash notice (issue #736, crash_notice.hpp): with crashes not yet offered it
// says Bloom quit unexpectedly, never modal, with Report on GitHub, Copy Report, Open Crash
// Folder and Dismiss; Copy Report copies the newest crash's text report; showing it records the
// offer, so the next start offers nothing; and while it may not be shown (headless, an agent
// session) it stays away and records nothing. Report on GitHub and Open Crash Folder are not
// pressed here: they open a browser and a file manager.

#include <bloom/crash/crash_reports.hpp>
#include <bloom/ui/crash_notice.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <QApplication>
#include <QClipboard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWidget>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << '\n';
        ++failures;
    }
}

void write(const std::filesystem::path& file, const std::string_view content) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary);
    stream << content;
}

// A crash folder as a collection makes it.
void makeReport(const std::filesystem::path& folder, const std::string& name,
                const std::string& program) {
    write(folder / name / "minidump.dmp", "MDMP");
    write(folder / name / "report.txt",
          "Bloom quit unexpectedly\n\nProgram: " + program + "\nFolder: " + name + "\n");
}

// Runs one start's look at the folder; true when a notice appeared.
bool look(QWidget& window, const std::filesystem::path& folder, const bool mayShow,
          bloom::ui::CrashNotice*& notice) {
    notice = new bloom::ui::CrashNotice(window, {.directory = folder,
                                                 .mayShow = [mayShow] { return mayShow; },
                                                 .waitForCollection = [] {}});
    QSignalSpy finished(notice, &bloom::ui::CrashNotice::finished);
    notice->start();
    if (!finished.wait(10000)) {
        expect(false, "the look finishes");
        return false;
    }
    return finished.front().front().toBool();
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir scratch;
    expect(scratch.isValid(), "a scratch folder");
    const std::filesystem::path folder = scratch.path().toStdString();
    makeReport(folder, "2026-11-19_09-00-00_bloom-cli_aaaaaaaa", "bloom-cli");
    makeReport(folder, "2026-11-20_09-00-00_bloom_bbbbbbbb", "bloom");

    QWidget window;
    window.resize(800, 600);
    window.show();

    // Headless or agent-driven: no notice, and nothing recorded.
    {
        bloom::ui::CrashNotice* notice = nullptr;
        expect(!look(window, folder, false, notice), "no notice while it may not be shown");
        expect(notice->dialog() == nullptr, "no dialog while it may not be shown");
        expect(bloom::crash::reportsToOffer(folder).size() == 2,
               "a crash not shown is still offered on the next start");
        delete notice;
    }

    // The first interactive start: the notice, about the newest crash.
    {
        bloom::ui::CrashNotice* notice = nullptr;
        expect(look(window, folder, true, notice), "the notice appears");
        auto* dialog = notice->dialog();
        expect(dialog != nullptr, "the notice is open");
        if (dialog != nullptr) {
            expect(dialog->objectName() == QStringLiteral("crashReportNotice"),
                   "the notice's name");
            expect(!dialog->isModal() && dialog->windowModality() == Qt::NonModal,
                   "the notice never blocks the window");
            expect(dialog->isVisible(), "the notice is shown");
            const auto text = [dialog](const bloom::ui::kit::KDialog::Role role) {
                auto* button = dialog->button(role);
                return button != nullptr ? button->text() : QString();
            };
            using Role = bloom::ui::kit::KDialog::Role;
            expect(text(Role::Accept) == QStringLiteral("Report on GitHub"), "Report on GitHub");
            expect(text(Role::Apply) == QStringLiteral("Copy Report"), "Copy Report");
            expect(text(Role::Reset) == QStringLiteral("Open Crash Folder"), "Open Crash Folder");
            expect(text(Role::Reject) == QStringLiteral("Dismiss"), "Dismiss");
            expect(dialog->defaultRole() == Role::Reject, "Return dismisses");

            QGuiApplication::clipboard()->clear();
            expect(dialog->activate(Role::Apply), "Copy Report is pressed");
            const QString copied = QGuiApplication::clipboard()->text();
            expect(copied.contains(QStringLiteral("Folder: 2026-11-20_09-00-00_bloom_bbbbbbbb")),
                   "Copy Report copies the newest crash's text report");
            expect(dialog->isVisible(), "Copy Report leaves the notice open");

            QSignalSpy destroyed(dialog, &QObject::destroyed);
            expect(dialog->activate(Role::Reject), "Dismiss is one click");
            expect(destroyed.wait(5000) || destroyed.count() == 1, "Dismiss closes the notice");
        }
        expect(bloom::crash::reportsToOffer(folder).empty(),
               "showing the notice records both crashes as offered");
        delete notice;
    }

    // The next start: nothing to offer.
    {
        bloom::ui::CrashNotice* notice = nullptr;
        expect(!look(window, folder, true, notice), "a second start shows no notice");
        expect(bloom::crash::listReports(folder).size() == 2, "the offered reports are kept");
        delete notice;
    }

    // The detail text names the program and counts the rest.
    {
        const auto reports = bloom::crash::listReports(folder);
        const QString detail = bloom::ui::crashNoticeDetail(reports);
        expect(detail.startsWith(QStringLiteral("Bloom quit unexpectedly on ")),
               "the detail names the newest crash's program");
        expect(detail.contains(QStringLiteral("nothing was sent anywhere")),
               "the detail says nothing was sent");
        expect(detail.contains(QStringLiteral("1 more crash report")),
               "the detail counts the other crashes");
        expect(bloom::ui::crashNoticeDetail({}).isEmpty(), "no crashes, no detail");
    }

    if (failures != 0)
        std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
