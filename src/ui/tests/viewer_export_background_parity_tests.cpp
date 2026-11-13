// Issue #447. The composition background colour is Viewer presentation only (docs/ux/ui-grammar.md,
// "Viewer canvas and empty state"). This test evaluates one composition with a non-black opaque
// background through the real Viewer preview path and through the real still export route and
// proves that
//   * the Viewer's process frame (its packed display product) equals the exported PNG still: RGB
//     within one code, alpha exactly;
//   * the background never enters that frame (the uncovered region stays zero alpha and colour,
//     and an edit of the background re-presents without changing the frame); and
//   * the Viewer adds the background only when it paints/presents: the uncovered region of the
//     widget shows the background colour while the covered region shows the frame.
//
// The CPU reference half always runs (CPU preview pipeline against the CPU export provider). The
// GPU half runs the GPU-prepared packed preview product the Viewer paints against the GPU export
// provider when --loader names a Vulkan loader and a qualified device exists; otherwise it prints
// NotRun explicitly, and --require-device turns that into a failure.

#include "await_qt.hpp"
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#if defined(BLOOM_UI_BACKGROUND_PARITY_GPU)
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool ok, const std::string& message) {
        if (ok) {
            return;
        }
        ++failures_;
        std::cerr << "FAIL: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

bool gRequireDevice = false;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 32;
// k/255 values, so the 8-bit quantization of the authored background is exact.
constexpr core::Color4d kBackground{204.0 / 255.0, 51.0 / 255.0, 26.0 / 255.0, 1.0};

struct Pixels final {
    bool ok = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // Straight RGBA8, row-major.

    [[nodiscard]] const std::uint8_t* at(const std::uint32_t x, const std::uint32_t y) const {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

[[nodiscard]] Pixels pixelsOf(const ui::PreparedPreviewFrameHandle& frame) {
    Pixels out;
    const auto view = frame != nullptr ? frame->displayBufferView() : std::nullopt;
    if (!view.has_value() || view->layout.rowStrideBytes != static_cast<std::size_t>(view->displayWindow.extent().width()) * 4U) {
        return out;
    }
    out.width = view->displayWindow.extent().width();
    out.height = view->displayWindow.extent().height();
    if (view->pixels.size() != static_cast<std::size_t>(out.width) * out.height) {
        return out;
    }
    out.rgba.reserve(view->pixels.size() * 4U);
    for (const auto& pixel : view->pixels) {
        out.rgba.insert(out.rgba.end(), {pixel.red, pixel.green, pixel.blue, pixel.alpha});
    }
    out.ok = true;
    return out;
}

[[nodiscard]] Pixels decodePng(const std::filesystem::path& path) {
    Pixels out;
    QImage image(QString::fromStdString(path.string()));
    if (image.isNull()) {
        return out;
    }
    // Straight-to-straight: no premultiplication round trip.
    image = image.convertToFormat(QImage::Format_RGBA8888);
    out.width = static_cast<std::uint32_t>(image.width());
    out.height = static_cast<std::uint32_t>(image.height());
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4U);
    for (int y = 0; y < image.height(); ++y) {
        std::copy_n(image.constScanLine(y), static_cast<std::size_t>(image.width()) * 4U,
                    out.rgba.begin() + static_cast<std::ptrdiff_t>(y) * image.width() * 4);
    }
    out.ok = true;
    return out;
}

// RGB within one 8-bit code, alpha exactly.
[[nodiscard]] bool framesMatch(const Pixels& a, const Pixels& b) {
    if (!a.ok || !b.ok || a.width != b.width || a.height != b.height) {
        return false;
    }
    for (std::size_t index = 0; index < a.rgba.size(); ++index) {
        const int difference = std::abs(static_cast<int>(a.rgba[index]) -
                                        static_cast<int>(b.rgba[index]));
        if (index % 4U == 3U ? difference != 0 : difference > 1) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasZeroAlphaColorAt(const Pixels& frame, const std::uint32_t x,
                                       const std::uint32_t y) {
    const auto* pixel = frame.at(x, y);
    return pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 0;
}

[[nodiscard]] bool closeTo(const QColor& color, const std::uint8_t r, const std::uint8_t g,
                           const std::uint8_t b) {
    return std::abs(color.red() - r) <= 1 && std::abs(color.green() - g) <= 1 &&
           std::abs(color.blue() - b) <= 1;
}

// The same entry point the Composition Settings dialog commits through.
[[nodiscard]] bool setBackground(ui::CompositionSession& session,
                                 const document::CompositionId composition,
                                 const core::Color4d color) {
    const auto* current = session.snapshot().project().findComposition(composition);
    if (current == nullptr) {
        return false;
    }
    auto settings = commands::compositionSettingsOf(*current);
    settings.background = color;
    return ui::applyCompositionSettings(session, composition, settings);
}

struct Route final {
    const char* name;
    bool gpu;
};

// One authored composition shared by the Viewer session and the export route: a solid scaled to
// half, so the frame has a covered centre and an uncovered border.
struct Authored final {
    std::unique_ptr<scripting::Session> scripting;
    std::unique_ptr<ui::CompositionSession> session;
    document::CompositionId composition;
};

[[nodiscard]] std::optional<Authored> author(Expectations& checks) {
    const auto format = document::CompositionFormat::create(kWidth, kHeight);
    checks.expect(format.has_value(), "the composition format is valid");
    if (!format.has_value()) {
        return std::nullopt;
    }
    auto created = scripting::Session::createNew("Background parity", "Main",
                                                 core::RationalTime::fromInteger(2), *format);
    checks.expect(static_cast<bool>(created), "the authoring session is created");
    if (!created) {
        return std::nullopt;
    }
    Authored authored;
    authored.scripting = std::move(created).takeSession();
    authored.composition = authored.scripting->snapshot().project().compositions().front().id();
    authored.session = std::make_unique<ui::CompositionSession>(
        *authored.scripting->document(), *authored.scripting->commandStack(),
        authored.composition);
    checks.expect(authored.session->addSolidLayer(QStringLiteral("Marker"),
                                                  core::Color4d{0.25, 0.5, 0.75, 1.0}),
                  "the marker solid is authored");
    checks.expect(authored.session->setSelectedScale(0.5, 0.5),
                  "the marker leaves an uncovered border");
    checks.expect(setBackground(*authored.session, authored.composition, kBackground),
                  "the non-black opaque background is set");
    return authored;
}

struct PreviewRig final {
    runtime::TaskScheduler scheduler;
    runtime::NodeDefinitionRegistry definitions;
    std::unique_ptr<runtime::SnapshotCompiler> compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    std::unique_ptr<ui::TaskUiBridge> bridge;
};

// Exercises one route: the Viewer's process frame against the exported still.
void exerciseRoute(Expectations& checks, const Route& route, const std::filesystem::path& workDir,
                   [[maybe_unused]] const std::filesystem::path& loader) {
    const std::string prefix = std::string(route.name) + ": ";
    auto authored = author(checks);
    if (!authored.has_value()) {
        return;
    }
    auto& session = *authored->session;

    PreviewRig rig;
    checks.expect(runtime::registerBuiltInNodeDefinitions(rig.definitions),
                  prefix + "node definitions register");
    rig.definitions.freeze();
    rig.compiler = std::make_unique<runtime::SnapshotCompiler>(rig.definitions);
    rig.qualified.publish(runtime::buildBloomNeutralQualifiedDisplayProcessor());
    rig.bridge = std::make_unique<ui::TaskUiBridge>(rig.scheduler, nullptr, 1ms);

    auto pipeline = ui::makeCompositionPreviewPipeline(*rig.compiler, rig.evaluator,
                                                       rig.displayPreparer, rig.qualified);
    ui::CompositionPreviewSettings settings;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    ui::PreviewPreparationSubmitter submitter;
    runtime::PreviewDisplayProvider expectedProvider = runtime::PreviewDisplayProvider::CpuOcio;

#if defined(BLOOM_UI_BACKGROUND_PARITY_GPU)
    std::unique_ptr<runtime::GpuPreviewDisplayService> service;
    if (route.gpu) {
        auto resolver = host::makePackagedGpuOcioResolver(
            QCoreApplication::applicationFilePath().toStdString());
        auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
            *rig.compiler, rig.evaluator, rig.qualified,
            std::make_shared<runtime::GpuSceneCoverageCache>(), nullptr, resolver);
        runtime::GpuPreviewDisplayServiceOptions options;
        options.enabled = true;
        options.loaderPath = loader;
        options.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
        options.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
        options.previewByteAllowance = std::size_t{1} << 28U;
        service = std::make_unique<runtime::GpuPreviewDisplayService>(
            rig.scheduler, gpuStage,
            ui::makeCompositionPreviewCpuStage(*rig.compiler, rig.evaluator, rig.qualified),
            ui::makeCompositionPreviewCpuDisplayFallback(rig.displayPreparer), options);
        const bool initialized = bloom::test_support::awaitQtCondition(
            "the GPU preview service leaves Initializing",
            [&] {
                return service->status().state !=
                       runtime::GpuPreviewDisplayServiceState::Initializing;
            },
            90s);
        if (!initialized ||
            service->status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
            std::cout << "NotRun: " << route.name << " preview: no qualified device ("
                      << service->status().residentDetail << ")\n";
            checks.expect(!gRequireDevice, prefix + "--require-device needs a GPU device");
            service->beginShutdown();
            return;
        }
        submitter = [svc = service.get()](runtime::TaskRequest request,
                                          const document::Snapshot& snapshot,
                                          const runtime::PreviewRequestIdentity& identity,
                                          const std::size_t bytes,
                                          const std::vector<runtime::SnapshotParameterOverride>&
                                              overrides) {
            return svc->submit(std::move(request), snapshot, identity, bytes, overrides);
        };
        settings.pixelStorageByteLimit = std::size_t{1} << 28U;
        expectedProvider = runtime::PreviewDisplayProvider::GpuPreparedPacked;
    }
#endif

    ui::CompositionPreviewController controller(session, rig.scheduler, *rig.bridge, pipeline,
                                                settings, nullptr, nullptr, submitter);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(480, 320);
    viewer.show();
    controller.requestRefresh();
    const bool ready = bloom::test_support::awaitQtCondition(
        "Viewer displays a current frame from the expected route", [&] {
            const auto frame = viewer.displayedFrameForTest();
            return controller.state().activity == ui::PreviewActivity::Ready && frame != nullptr &&
                   frame->provenance().provider == expectedProvider &&
                   viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
        });
    checks.expect(ready, prefix + "the Viewer displays a current frame from the expected route");
    const auto displayed = viewer.displayedFrameForTest();
    const Pixels processFrame = pixelsOf(displayed);
    checks.expect(processFrame.ok && processFrame.width == kWidth && processFrame.height == kHeight,
                  prefix + "the Viewer process frame is readable at the composition size");
    if (!ready || !processFrame.ok) {
        return;
    }

    // The background is not part of the process frame: the uncovered border is exact zero.
    checks.expect(hasZeroAlphaColorAt(processFrame, 1, 1) &&
                      hasZeroAlphaColorAt(processFrame, kWidth - 2, kHeight - 2),
                  prefix + "the process frame keeps zero alpha and colour outside the layer");
    checks.expect(processFrame.at(kWidth / 2, kHeight / 2)[3] == 255,
                  prefix + "the process frame is opaque where the layer covers it");

    // The still export route, rendered from the same document.
    runtime::TaskScheduler exportScheduler;
    const runtime::SnapshotCompiler exportCompiler(document::builtInNodeDefinitions());
    std::shared_ptr<host::GpuExportProvider> provider;
#if defined(BLOOM_UI_BACKGROUND_PARITY_GPU)
    if (route.gpu) {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = true;
        options.loaderPath = loader;
        options.ocioResolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
        provider = host::GpuExportProvider::create(options);
    }
#endif
    if (!route.gpu) {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = false;
        provider = host::GpuExportProvider::create(options);
    }
    provider->prepare(exportScheduler);
    const auto still = workDir / (std::string(route.name) + "-still.png");
    const auto result = scripting::Render::run(
        *authored->scripting, exportScheduler, exportCompiler,
        {.composition = authored->composition,
         .frame = 0,
         .range = std::nullopt,
         .preset = output::OutputPresetV1::PngRgba8SrgbV1,
         .destination = still},
        {}, provider);
    checks.expect(result.succeeded, prefix + "the still publishes: " + result.diagnostic);
    if (route.gpu) {
        checks.expect(result.gpuEvaluatedFrames == 1,
                      prefix + "the still was genuinely evaluated on the device");
    } else {
        checks.expect(result.gpuEvaluatedFrames == 0,
                      prefix + "the CPU reference still never touched a device");
    }
    const Pixels exported = decodePng(still);
    checks.expect(exported.ok, prefix + "the exported still decodes");
    checks.expect(framesMatch(processFrame, exported),
                  prefix + "the Viewer process frame matches the exported still (RGB within one "
                           "code, exact alpha)");
    checks.expect(provider->shutdownAndWait(10s), prefix + "the export provider retires");

    // The Viewer adds the background at paint only.
    const auto mapping = viewer.mappingForTest();
    checks.expect(mapping.has_value(), prefix + "the Viewer reports its frame mapping");
    if (mapping.has_value()) {
        const QRectF frameRect = mapping->displayRect;
        const auto widgetPixelAt = [&](const double fx, const double fy) {
            const QImage image = viewer.grab().toImage();
            return image.pixelColor(
                static_cast<int>(frameRect.left() + frameRect.width() * fx),
                static_cast<int>(frameRect.top() + frameRect.height() * fy));
        };
        const QColor uncovered = widgetPixelAt(0.04, 0.06);
        checks.expect(closeTo(uncovered, 204, 51, 26),
                      prefix + "the Viewer paints the background where the frame is transparent");
        const QColor covered = widgetPixelAt(0.4, 0.4);
        const auto* inside = processFrame.at(kWidth * 2 / 5, kHeight * 2 / 5);
        checks.expect(closeTo(covered, inside[0], inside[1], inside[2]),
                      prefix + "the Viewer paints the frame itself where the layer covers it");

        // A background edit re-presents; it never changes the frame the Viewer holds.
        checks.expect(setBackground(session, authored->composition, {0.0, 0.2, 0.4, 1.0}),
                      prefix + "the background is edited");
        QCoreApplication::processEvents();
        const QColor recoloured = widgetPixelAt(0.04, 0.06);
        checks.expect(closeTo(recoloured, 0, 51, 102),
                      prefix + "the edited background shows at present");
        const Pixels after = pixelsOf(viewer.displayedFrameForTest());
        checks.expect(after.ok && after.rgba == processFrame.rgba,
                      prefix + "the background edit leaves the process frame byte-identical");
    }

    controller.beginShutdown();
    rig.bridge->beginShutdown();
    checks.expect(
        bloom::test_support::awaitQtCondition("the rig scheduler reaches quiescence",
                                              [&] { return rig.scheduler.isQuiescent(); }),
        "the rig scheduler reaches quiescence");
#if defined(BLOOM_UI_BACKGROUND_PARITY_GPU)
    if (service != nullptr) {
        service->beginShutdown();
        checks.expect(bloom::test_support::awaitQtCondition(
                          "the GPU preview service stops",
                          [&] {
                              return service->status().state ==
                                     runtime::GpuPreviewDisplayServiceState::Stopped;
                          }),
                      "the GPU preview service stops");
    }
#endif
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    std::filesystem::path loader;
    std::vector<char*> forwarded{argv[0]};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            loader = argv[++index];
        } else if (argument == "--require-device") {
            gRequireDevice = true;
        } else {
            forwarded.push_back(argv[index]);
        }
    }
    int forwardedCount = static_cast<int>(forwarded.size());
    QApplication application(forwardedCount, forwarded.data());
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomBackgroundParity");
    QCoreApplication::setApplicationName("ViewerExportBackgroundParity");
    QSettings().setValue("viewer/resolution", "Full");

    Expectations checks;
    const std::filesystem::path workDir = temporary.path().toStdString();
    exerciseRoute(checks, {"cpu", false}, workDir, loader);

#if defined(BLOOM_UI_BACKGROUND_PARITY_GPU)
    if (loader.empty()) {
        std::cout << "NotRun: gpu route: no Vulkan loader was given (--loader)\n";
        checks.expect(!gRequireDevice, "--require-device needs --loader");
    } else {
        exerciseRoute(checks, {"gpu", true}, workDir, loader);
    }
#else
    std::cout << "NotRun: gpu route: this build has no Vulkan support\n";
    checks.expect(!gRequireDevice, "--require-device needs a Vulkan build");
#endif

    if (checks.failures() + bloom::test_support::awaitFailureCount() != 0) {
        std::cerr << "viewer/export background parity: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: viewer/export background parity\n";
    return 0;
}
