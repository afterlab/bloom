// Captures of the Profiler panel (#423) over the deterministic fake feed, for visual review: a
// wide and a narrow panel after five minutes of history, and a panel with no source. Writes
// <dir>/profiler-<state>-dpr<scale>.png, where <dir> is BLOOM_PROFILER_CAPTURE_DIR (or the first
// argument) and the scale follows QT_SCALE_FACTOR. Without either it captures into a temporary
// directory and only checks that each capture rendered.
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/profiler_editor.hpp>
#include <bloom/ui/profiler_fake_feed.hpp>
#include <bloom/ui/profiler_history.hpp>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTest>

#include <iostream>

namespace {

namespace ui = bloom::ui;

// The panel's whole content at its natural height for `width`, so one image shows every section.
QImage grabWhole(ui::ProfilerEditor& panel, const int width) {
    auto* content = panel.findChild<QWidget*>(QStringLiteral("profilerContent"));
    panel.resize(width, content != nullptr ? content->sizeHint().height() : width);
    QTest::qWait(30);
    if (content != nullptr)
        panel.resize(width, content->heightForWidth(width) > 0 ? content->heightForWidth(width)
                                                               : content->sizeHint().height());
    QTest::qWait(30);
    return panel.grab().toImage();
}

int save(const QImage& image, const QString& directory, const QString& name, const QString& scale) {
    const QString path = directory + QStringLiteral("/profiler-%1-dpr%2.png").arg(name, scale);
    if (image.isNull() || image.width() == 0 || !image.save(path)) {
        std::cerr << "cannot write " << path.toStdString() << '\n';
        return 1;
    }
    std::cout << path.toStdString() << '\n';
    return 0;
}

int captureWithHistory(const QString& directory, const QString& scale, const QString& name,
                       const int width) {
    ui::FakeProfilerFeed feed;
    ui::ProfilerEditor panel(&feed);
    panel.resize(width, width);
    panel.show();
    // Five minutes of history ending on a stalled ledger sample during an export, so the capture
    // shows full rings, the eviction drop, stale ages and the absent fields.
    feed.setNextSerial(1 + 2 * 240 + 175 - ui::ProfilerHistory::kCapacity);
    for (int sample = 0; sample < ui::ProfilerHistory::kCapacity; ++sample)
        (void)feed.pump();
    return save(grabWhole(panel, width), directory, name, scale);
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("BloomProfilerPanelCapture"));
    application.setApplicationName(QStringLiteral("ProfilerPanelCapture"));
    ui::kit::installKinetikTheme(application);
    QTemporaryDir scratch;
    QString directory = qEnvironmentVariable("BLOOM_PROFILER_CAPTURE_DIR");
    if (directory.isEmpty() && argc > 1)
        directory = QString::fromLocal8Bit(argv[1]);
    if (directory.isEmpty())
        directory = scratch.path();
    QDir().mkpath(directory);
    const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")).remove('.');
    int failures = 0;
    failures += captureWithHistory(directory, scale, QStringLiteral("wide"), 960);
    failures += captureWithHistory(directory, scale, QStringLiteral("narrow"), 380);
    {
        ui::ProfilerEditor panel(nullptr);
        panel.show();
        failures += save(grabWhole(panel, 760), directory, QStringLiteral("no-source"), scale);
    }
    return failures;
}
