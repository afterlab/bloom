// Review captures for the disk cache, audio and fonts group (#627, #632 to #636, #718).
// A tool, not a test:
//   bloom_lane_media_audio_capture <directory> [parts]
// writes <issue>-<name>.png. Parts: cards, bake, statusbar, preferences.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "node_interaction_test_support.hpp"
#include "preferences_window.hpp"
#include "window_fixture.hpp"

#include <QDir>
#include <QImage>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <iostream>
#include <memory>

namespace {
using namespace bloom;
using namespace bloom::ui;

QString gDirectory;
int gFailures = 0;

// Delivers what is posted (layout, polish, repaint) before a picture is taken.
void settle() {
    for (int turn = 0; turn < 4; ++turn) {
        QCoreApplication::sendPostedEvents();
        QApplication::processEvents();
    }
}

void save(const QImage& image, const char* issue, const QString& name) {
    const QString path = gDirectory + QLatin1Char('/') + QLatin1String(issue) + QLatin1Char('-') +
                         name + QStringLiteral(".png");
    if (image.isNull() || !image.save(path)) {
        std::cerr << "could not write " << path.toStdString() << '\n';
        ++gFailures;
        return;
    }
    std::cout << path.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
}

// #627, #632 to #635: the card and the Properties of the Number List node and of the five audio
// analysis nodes.
void captureCards() {
    struct Card final {
        const char* issue;
        std::string_view type;
        const char* name;
    };
    for (const auto& [issue, type, name] :
         {Card{"627", document::kNumberListValueNodeType, "number-list"},
          Card{"632", document::kAudioBandsNodeType, "audio-bands"},
          Card{"633", document::kAudioSampleNodeType, "audio-sample"},
          Card{"634", document::kAudioFeaturesNodeType, "audio-features"},
          Card{"635", document::kAudioBeatNodeType, "audio-beat"},
          Card{"635", document::kAudioPitchNodeType, "audio-pitch"}}) {
        test::Fixture f;
        const auto node = f.add(type, {0, 0});
        settle();
        auto* card = f.card(node);
        if (card == nullptr) {
            ++gFailures;
            continue;
        }
        f.view()->centerOn(card);
        settle();
        const QRect region =
            f.view()
                ->mapFromScene(card->sceneBoundingRect().adjusted(-16, -16, 16, 16))
                .boundingRect()
                .intersected(f.view()->viewport()->rect());
        save(f.view()->viewport()->grab(region).toImage(), issue,
             QLatin1String(name) + QStringLiteral("-card"));
        f.session.selectNode(node);
        PropertiesEditor properties(f.session);
        properties.resize(420, 900);
        properties.show();
        settle();
        save(properties.grab().toImage(), issue,
             QLatin1String(name) + QStringLiteral("-properties-420"));
    }
}

// #636: the node menu of a node with one number output and of one with several.
void captureBakeMenu() {
    for (const auto& [type, name] :
         {std::pair{document::kAudioBandsNodeType, "node-menu-one-output"},
          std::pair{document::kAudioBeatNodeType, "node-menu-several-outputs"}}) {
        test::Fixture f;
        const auto node = f.add(type, {0, 0});
        f.session.selectNode(node);
        settle();
        const std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest(true));
        if (!menu) {
            ++gFailures;
            continue;
        }
        menu->ensurePolished();
        menu->resize(menu->sizeHint());
        save(menu->grab().toImage(), "636", QLatin1String(name));
        if (auto* outputs = menu->findChild<QMenu*>(QStringLiteral("nodeBakeToKeyframesMenu"))) {
            outputs->ensurePolished();
            outputs->resize(outputs->sizeHint());
            save(outputs->grab().toImage(), "636", QStringLiteral("node-menu-outputs-to-bake"));
        }
    }
}

// #630, #636: the status bar while a sound is rendered and analyzed and while a bake reads frames.
void captureStatusBar() {
    test::WindowFixture fixture;
    settle();
    auto* bar = fixture.window->findChild<WindowStatusBar*>();
    if (bar == nullptr) {
        ++gFailures;
        return;
    }
    runtime::AudioSoundActivity rendering;
    rendering.phase = runtime::AudioSoundPhase::Rendering;
    rendering.framesDone = 12'000;
    rendering.framesTotal = 48'000;
    runtime::AudioSoundActivity analyzing = rendering;
    analyzing.phase = runtime::AudioSoundPhase::Analyzing;
    analyzing.framesDone = 24'000;
    analyzing.queued = 2;
    for (const auto& [readout, name] :
         {std::pair{backgroundWorkReadout(rendering, 0, 0), "status-bar-rendering-sound"},
          std::pair{backgroundWorkReadout(analyzing, 0, 0), "status-bar-analyzing-sound"},
          std::pair{backgroundWorkReadout({}, 24, 96), "status-bar-baking-keyframes"}}) {
        bar->showBackgroundWorkForTest(readout);
        settle();
        save(bar->grab().toImage(), std::string_view(name).ends_with("keyframes") ? "636" : "630",
             QLatin1String(name));
    }
}

// #718: Preferences, Memory & Caches, with a folder rule for local copies.
void capturePreferences() {
    QWidget owner;
    PreferencesWindow preferences(owner, defaultApplicationPreferences());
    preferences.show();
    settle();
    if (auto* rail = preferences.findChild<QListWidget*>(QStringLiteral("preferencesCategoryRail")))
        for (int row = 0; row < rail->count(); ++row)
            if (rail->item(row)->text().contains(QStringLiteral("Memory")))
                rail->setCurrentRow(row);
    preferences.addLocalCopyFolder(QDir::rootPath() + QStringLiteral("projects/shots"));
    settle();
    save(preferences.grab().toImage(), "718", QStringLiteral("preferences-memory-and-caches"));
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: bloom_lane_media_audio_capture <directory> [parts]\n";
        return 2;
    }
    QApplication app(argc, argv);
    app.setOrganizationName("BloomLaneCapture");
    app.setApplicationName("LaneMediaAudioCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    kit::installAfterlabTheme(app);
    gDirectory = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(gDirectory);
    const QStringList only =
        argc > 2 ? QString::fromLocal8Bit(argv[2]).split(QLatin1Char(',')) : QStringList{};
    const auto wanted = [&](const char* name) {
        return only.isEmpty() || only.contains(QLatin1String(name));
    };
    try {
        if (wanted("cards"))
            captureCards();
        if (wanted("bake"))
            captureBakeMenu();
        if (wanted("statusbar"))
            captureStatusBar();
        if (wanted("preferences"))
            capturePreferences();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return gFailures == 0 ? 0 : 1;
}
