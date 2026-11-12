// #343: the desktop route behind File > Import for an SVG (the asset controller). The file is read
// on a task thread with progress, the artist can cancel it before anything is added, the items the
// file holds that cannot be carried over are listed by name on the job (the Jobs panel's
// diagnostics) and on the controller's diagnostics, and a finished import is one undo step.

#include "import_progress_test_support.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <bloom/commands/transaction.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <iostream>
#include <stdexcept>

namespace {
using namespace bloom;

void require(const bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <typename Predicate> bool waitUntil(Predicate predicate, const int timeoutMs = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(5);
    return predicate();
}

constexpr auto kSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">
<defs><filter id="glow"><feGaussianBlur stdDeviation="2"/><feOffset dx="2" dy="2"/></filter></defs>
<path id="ring" fill="#336699" fill-rule="evenodd" d="M10 10H90V90H10Z M30 30H70V70H30Z"/>
<rect id="boxed" x="100" y="10" width="60" height="40" fill="#cc3333" filter="url(#glow)"/>
<text id="title" x="100" y="80" font-family="Zz Not Installed Face" font-size="14">Hello</text>
</svg>
)svg";

struct Harness final {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host{scheduler};
    ui::CompositionSession session{*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler};
    ui::AssetController controller{session, host, scheduler, bridge};
    QStringList diagnostics;
    Harness() {
        if (session.composition() == nullptr) {
            commands::Transaction seed("Seed", session.snapshot().revision());
            seed.emplace<commands::AddComposition>("Composition 1", document::CompositionFormat{},
                                                   core::RationalTime::fromInteger(10));
            const auto seeded = session.executeTransaction(std::move(seed));
            const auto id = seeded.succeeded()
                                ? seeded.outputId<document::CompositionId>(commands::kAddCompositionOutput)
                                : std::nullopt;
            require(id && session.setComposition(*id), "seed a composition");
        }
        QObject::connect(&controller, &ui::AssetController::diagnostic,
                         [this](const QString& text) { diagnostics.push_back(text); });
    }
    ~Harness() {
        controller.setImportProgressHookForTesting({});
        waitUntil([this] { return !controller.busy(); });
    }
    [[nodiscard]] std::size_t layerCount() const {
        const auto& project = session.snapshot().project();
        const auto* composition = project.findComposition(session.compositionId());
        return composition == nullptr ? 0 : composition->graph().layerOutputs().size();
    }
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomSvgImportRoute"));
    QApplication::setApplicationName(QStringLiteral("SvgImportRoute"));
    QStandardPaths::setTestModeEnabled(true);
    ui::kit::installAfterlabTheme(app);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary directory");
        const QString path = directory.filePath("art.svg");
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write(kSvg) > 0, "write the SVG");
        file.close();

        // Cancel while the worker is held at the start of the read: nothing is added.
        {
            Harness harness;
            const auto before = harness.layerCount();
            auto gate = std::make_shared<ui::test::ImportGate>(ui::test::itemStart(0));
            harness.controller.setImportProgressHookForTesting(
                [gate](const commands::ImportProgress& state) { (*gate)(state); });
            harness.controller.importFiles({path});
            require(waitUntil([&] { return gate->reached(); }), "the SVG read reports progress");
            require(harness.controller.busy(), "the import is a running job");
            harness.controller.cancel();
            gate->release();
            require(waitUntil([&] { return !harness.controller.busy(); }), "the cancelled job ends");
            require(harness.layerCount() == before, "a cancelled SVG import adds nothing");
            require(harness.session.undoLabel() != QStringLiteral("Import SVG"),
                    "a cancelled import leaves no undo step");
        }

        // A normal import: warnings by name on the job, one undo step, a recorded source.
        {
            Harness harness;
            const auto before = harness.layerCount();
            const auto assetsBefore = harness.session.snapshot().project().assets().size();
            harness.controller.importFiles({path});
            require(waitUntil([&] { return !harness.controller.busy(); }), "the import ends");
            require(harness.layerCount() > before, "the SVG adds its layers");
            // #642: with nobody to ask (no window), the default is one layer: one Vector Artwork node.
            {
                const auto* composition =
                    harness.session.snapshot().project().findComposition(harness.session.compositionId());
                require(composition != nullptr &&
                            std::ranges::count_if(composition->graph().nodes(),
                                                  [](const auto& node) {
                                                      return node.typeId == document::kVectorArtworkNodeType;
                                                  }) == 1 &&
                            harness.layerCount() == before + 1,
                        "the desktop imports an SVG as one layer by default");
            }
            require(harness.session.undoLabel() == QStringLiteral("Import SVG"),
                    "the import is one undo step named Import SVG");
            require(harness.session.snapshot().project().assets().size() == assetsBefore + 1,
                    "the source file is recorded in the project");
            // The record is a source file, not footage: the Assets panel does not mark an SVG
            // that is on disk as missing or undecodable.
            {
                const auto& assets = harness.session.snapshot().project().assets();
                require(!assets.empty(), "the source record exists");
                const auto id = assets.back().id;
                require(waitUntil([&] { return !harness.controller.busy(); }), "thumbnails settle");
                require(waitUntil([&] { return harness.controller.fileSize(id).has_value(); }),
                        "the SVG record's preview was computed");
                require(!harness.controller.missing(id) && harness.controller.problem(id).isEmpty(),
                        "an SVG source on disk is not flagged missing or unshowable");
            }

            // The Jobs panel's lines are the finished task's diagnostics.
            // The Jobs panel lists the job's warnings: the task's diagnostics.
            bool named = false;
            for (const auto& task : harness.scheduler.snapshots())
                for (const auto& diagnostic : task.diagnostics)
                    named = named || (diagnostic.summary.find("boxed") != std::string::npos &&
                                      diagnostic.summary.find("feOffset") != std::string::npos);
            bool font = false;
            for (const auto& text : harness.diagnostics) {
                std::cout << "diagnostic: " << text.toStdString() << '\n';
                font = font || text.contains(QStringLiteral("Zz Not Installed Face"));
            }
            require(font, "the font that is not installed is listed by name");

            require(harness.session.undo() && harness.layerCount() == before,
                    "one Undo removes the whole artwork");
            require(harness.session.snapshot().project().assets().size() == assetsBefore,
                    "and the source record with it");
        }
        std::cout << "svg import route: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
