// Offscreen captures of the disk cache's two faces (#732): the Preferences window's Memory &
// Caches page and the status bar with its Disk cell, over a source mirror on an injected
// 500 GiB drive with 120 GiB free and a few mirrored sources. Writes
// 732-<tag>-preferences-<width>-dpr<d>.png and 732-<tag>-statusbar-<width>-dpr<d>.png (and a .txt
// with the Disk cell's tooltip) to the directory given as the first argument; the second argument
// is the tag ("before" or "after"). The device pixel ratio follows QT_SCALE_FACTOR.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include <bloom/commands/command_stack.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/media/cache/source_mirror.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/theme.hpp>
#include "preferences_window.hpp"
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>

#include <chrono>
#include <functional>
#include <optional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>


namespace {
using namespace bloom;
namespace cache = bloom::media::cache;

constexpr std::uint64_t kGiB = std::uint64_t{1} << 30U;

void settle() {
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

void mirrorSource(cache::SourceMirror& mirror, const char tag, const std::size_t bytes) {
    auto data = std::make_shared<std::vector<std::byte>>(bytes, static_cast<std::byte>(tag));
    platform::FileIdentity identity;
    identity.size = data->size();
    identity.modifiedNanoseconds = 1;
    identity.fileIdLow = static_cast<std::uint64_t>(tag);
    const std::span<const std::byte> view(*data);
    mirror.keep(std::filesystem::temp_directory_path() / "bloom-dcache-capture" /
                    (std::string(1, tag) + ".exr"),
                identity, std::move(data), view);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomDiskCacheCapture");
    app.setApplicationName("DiskCacheCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    ui::kit::installAfterlabTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    const QString tag = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("before");
    QDir().mkpath(directory);
    const QString dpr = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")) == "1.5"
                            ? QStringLiteral("dpr1.5")
                            : QStringLiteral("dpr1");

    QTemporaryDir cacheDirectory;
    cache::SourceMirrorConfig config;
    config.rootDirectory = std::filesystem::path(cacheDirectory.filePath("media").toStdString());
    config.driveSpace = [] {
        return std::optional(cache::SourceMirrorDriveSpace{500 * kGiB, 120 * kGiB});
    };
    cache::SourceMirror mirror(config);
    mirrorSource(mirror, 'a', std::size_t{24} << 20U);
    mirrorSource(mirror, 'b', std::size_t{12} << 20U);
    mirrorSource(mirror, 'c', std::size_t{40} << 20U);
    mirrorSource(mirror, 'd', std::size_t{8} << 20U);
    mirror.flush();
    // The mirror measures the drive on its own thread: give it a moment, never longer than 5 s.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        const auto stats = mirror.statistics();
        if (stats.indexLoaded && stats.entryCount == 4 && stats.availableBytes != 0)
            break;
        QTest::qWait(20);
    }

    auto seed = document::makeNewProject("Disk Cache Capture", "Shot010",
                                         core::RationalTime::fromInteger(24));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    ui::CompositionSession session(doc, stack, seed.initialCompositionId);

    const std::vector<std::pair<QString, int>> widths = {{"2000", 2000}, {"900", 900}, {"640", 640}};
    int failures = 0;
    const auto save = [&](const QImage& image, const QString& kind, const QString& width) {
        const QString name = QStringLiteral("732-%1-%2-%3-%4").arg(tag, kind, width, dpr);
        if (!image.save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
        return name;
    };

    for (const auto& [label, width] : widths) {
        {
            QWidget owner;
            ui::PreferencesWindow window(owner, ui::ApplicationPreferences{});
            // After #732 the window takes the cache's live figures; before it, it has no such
            // interface and the same tool captures the page as it was.
            if constexpr (requires {
                              window.setDiskCacheFigures(
                                  std::function<std::optional<cache::SourceMirrorStatistics>()>{});
                          }) {
                window.setDiskCacheFigures(
                    [&mirror]() -> std::optional<cache::SourceMirrorStatistics> {
                        return mirror.statistics();
                    });
            }
            window.resize(width, 900);
            window.show();
            settle();
            for (auto* rail : window.findChildren<QListWidget*>()) {
                for (int row = 0; row < rail->count(); ++row)
                    if (rail->item(row)->text() == QStringLiteral("Memory & Caches"))
                        rail->setCurrentRow(row);
            }
            settle();
            save(window.grab().toImage(), QStringLiteral("preferences"), label);
        }
        {
            QWidget host;
            auto* layout = new QVBoxLayout(&host);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->addStretch(1);
            ui::WindowStatusBar bar(session, nullptr, &mirror);
            layout->addWidget(&bar);
            host.resize(width, 120);
            host.show();
            settle();
            const auto name = save(host.grab().toImage(), QStringLiteral("statusbar"), label);
            QFile text(directory + '/' + name + ".txt");
            if (text.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                text.write((QStringLiteral("cell: ") + bar.diskTextForTest() + QStringLiteral("\n\n") +
                            bar.diskToolTipForTest() + QStringLiteral("\n"))
                               .toUtf8());
            } else {
                ++failures;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
