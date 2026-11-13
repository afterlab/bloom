// Genuine `route.preview.ram_preview` proof harness.
//
// It drives the REAL ui::RamPreviewController over a real multi-frame animated composition. Each
// frame is submitted through the real GpuPreviewDisplayService resident overload (Wayland
// presentation) via the controller's submitter, so the controller's shared PreviewFrameCache is
// filled with genuine GPU-resident frames. The run is then replayed: every cached resident frame is
// taken back out of the cache, verified against the CPU oracle (deep plan identity, request
// identity, evaluated geometry, real CPU pixels), and the warm run must perform zero additional
// native dispatches. No fake service, no relabelled viewer proof.
//
// The proof is the real executed provenance: the positive cold native dispatch delta, the actual
// device ownership epoch, the ordered per-frame ProcessFrameIdentity digest, and the CPU-oracle
// sparse-sample evidence digest. Preview routes carry no readback fields.
//
// `--animated-mask` (ADR 0028) is a separate, non-writing run of the same harness: a 24-frame
// composition whose layer mask path is keyed on every frame is RAM-previewed through the scene
// builder carrying the shared OCIO preparer resolved from this target's packaged shader tools.
// The cold fill must dispatch genuine GPU frames that match the CPU oracle while running the
// shader compiler at most once for the whole run (the mask geometry is per-dispatch data), and the
// warm replay must perform zero preparation, dispatch or compile. It publishes no route proof.
//
// `--path-nodes` (#344, #530, #509) is another non-writing run: a 512 x 288, six-frame composition
// of the path nodes drawn by Shape Sources -- a Trim Path whose end is keyed, a Boolean Paths
// subtraction, a dashed Path Stroke and its core through Glow -- previewed in the interactive
// viewer and RAM-previewed through the same service. The viewer frame and every RAM frame must be
// GPU-resident and match the CPU oracle, the warm replay must do no work, and an operand edit must
// re-render every frame on the GPU against the edited CPU oracle.
//
// `--image-statistics` (#349) is another non-writing run of the same shape: a 512 x 288, four-frame
// composition whose keyed Four Color Gradient goes through Denoise with Auto strength and then
// Levels whose input black and white an Image Statistics node of the gradient drives. The viewer
// and RAM preview prepare every frame on the GPU, with each frame's statistics measured once on the
// application's device route (a GpuExportProvider's evaluator, as the application wires it) and
// equal to the CPU reference's; the warm replay measures nothing; an upstream edit measures every
// frame again; and with the device route refused, the frame takes the CPU path, labelled CPU.
// `--montage <dir>` writes the source, the CPU frame and the GPU frame (sampled every 4th pixel)
// of its first and last frames.

#include "await.hpp"
#include "fixed_memory_budget.hpp"
#include "gpu_preview_test_phase.hpp"
#include "gpu_route_proof_harness_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/four_color_gradient_nodes.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/image_statistics.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QVulkanInstance>
#include <QWindow>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace bloom::ui::verticalproof;
namespace routeproof = bloom::ui::routeproof;
using namespace std::chrono_literals;

namespace {

using namespace bloom;

constexpr std::int64_t kFrameRate = 25;
constexpr std::int64_t kFrameCount = 4;

struct HarnessOptions final {
    std::filesystem::path loader;
    std::filesystem::path proofDirectory;
    bool requireDevice = false;
    bool valid = true;
    bool sequence100 = false;
    bool half = false;
    bool expectPartial = false;
    bool expireBeforeAdmission = false;
    bool animatedMask = false;
    bool pathNodes = false;
    bool imageStatistics = false;
    std::filesystem::path montage;
    std::uint64_t poolBytes = 1ULL << 30;
};

[[nodiscard]] HarnessOptions parseHarnessOptions(const int argc, char** argv) {
    HarnessOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loader = argv[++index];
        } else if (argument == "--proof-dir" && index + 1 < argc) {
            options.proofDirectory = argv[++index];
        } else if (argument == "--sequence-100-half") {
            options.sequence100 = true;
            options.half = true;
        } else if (argument == "--sequence-100-full") {
            options.sequence100 = true;
        } else if (argument == "--pool-2gib") {
            options.poolBytes = 2ULL << 30;
        } else if (argument == "--pool-3gib") {
            options.poolBytes = 3ULL << 30;
        } else if (argument == "--expire-before-admission") {
            options.expireBeforeAdmission = true;
        } else if (argument == "--animated-mask") {
            options.animatedMask = true;
        } else if (argument == "--path-nodes") {
            options.pathNodes = true;
        } else if (argument == "--image-statistics") {
            options.imageStatistics = true;
        } else if (argument == "--montage" && index + 1 < argc) {
            options.montage = argv[++index];
        } else if (argument == "--expect-partial") {
            options.expectPartial = true;
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] core::RationalTime frameTime(const std::int64_t index) {
    const auto value = core::RationalTime::create(index, kFrameRate);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] document::CompositionFormat ramFormat(const std::uint32_t width = 1920U,
                                                    const std::uint32_t height = 1080U) {
    const auto rate = document::FrameRate::create(kFrameRate, 1);
    if (!rate.has_value()) {
        std::abort();
    }
    const auto format =
        document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(), *rate);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

// The shared preparer resolved from the packaged shader tools staged beside this executable, or
// null when the build staged none.
[[nodiscard]] std::shared_ptr<const runtime::GpuSceneOcioContext> packagedOcioContext() {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
    runtime::GpuOcioContextRequest request;
    request.applicationExecutable = std::filesystem::path{BLOOM_GPU_RAM_TEST_EXECUTABLE};
    request.toolPackage.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
    request.toolPackage.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
    request.toolPackage.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
    request.toolPackage.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
    request.toolPackage.relocated = static_cast<bool>(BLOOM_GPU_TOOLS_RELOCATED);
#if defined(BLOOM_GPU_TOOLS_BUNDLE_RELATIVE)
    request.toolPackage.bundleRelative = true;
#endif
#if defined(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256)
    request.toolPackage.glslangStagedDigest = core::Sha256Digest::fromLowercaseHex(
        std::string_view{BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256}.substr(7));
#endif
#if defined(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256)
    request.toolPackage.spirvValStagedDigest = core::Sha256Digest::fromLowercaseHex(
        std::string_view{BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256}.substr(7));
#endif
    runtime::GpuOcioContextResolver resolver(std::move(request));
    const auto resolved = resolver.resolve();
    return resolved.hasValue() ? resolved.context : nullptr;
#else
    return nullptr;
#endif
}

// A closed outline in the solid's content space whose position and curved handle move with the
// frame, so every keyed frame samples a different path.
[[nodiscard]] document::PathValue maskPathForFrame(const std::int64_t frame) {
    const double shift = 6.0 * static_cast<double>(frame);
    const double left = 820.0 + shift;
    const double top = 420.0 + shift * 0.5;
    document::PathValue path;
    path.editableContour().closed = true;
    path.editableContour().anchors = {
        document::PathAnchor{document::Vec2d{left, top}, std::nullopt, std::nullopt},
        document::PathAnchor{document::Vec2d{left + 240.0, top}, std::nullopt,
                             document::Vec2d{left + 300.0 + shift, top + 60.0}},
        document::PathAnchor{document::Vec2d{left + 240.0, top + 180.0}, std::nullopt,
                             std::nullopt},
        document::PathAnchor{document::Vec2d{left, top + 180.0}, std::nullopt, std::nullopt}};
    return path;
}

// The `--path-nodes` and `--image-statistics` documents, built through commands as an artist's
// edits would be.
struct NodeDocument final {
    document::Document& document;
    commands::CommandStack& commands;
    document::CompositionId composition;
    Expectations& checks;
    document::NodeId booleanNode;
    document::NodeId gradientNode;

    [[nodiscard]] const document::Composition& current() const {
        return *document.snapshot().project().findComposition(composition);
    }
    template <typename Operation, typename... Arguments> bool run(Arguments&&... arguments) {
        commands::Transaction transaction("Path nodes", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        return commands.execute(std::move(transaction)).changed();
    }
    [[nodiscard]] document::NodeId last(const std::string_view type) const {
        const auto nodes = current().graph().nodes();
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
            if (it->typeId == type)
                return it->id;
        return {};
    }
    [[nodiscard]] document::ParameterId parameter(const document::NodeId node,
                                                  const std::string_view role) const {
        if (const auto* record = current().graph().findNode(node))
            for (const auto& binding : record->parameters)
                if (binding.role == role)
                    return binding.parameterId;
        return {};
    }
    document::NodeId add(const std::string_view type) {
        checks.expect(run<commands::AddNode>(composition, std::string(type), document::Vec2d{}),
                      "a path-nodes node is added");
        return last(type);
    }
    void set(const document::NodeId node, const std::string_view role,
             document::ParameterValue value) {
        checks.expect(
            run<commands::SetParameterSource>(composition, parameter(node, role),
                                              document::ConstantValueSource{std::move(value)}),
            "a path-nodes operand is set");
    }
    void connect(const document::NodeId from, const std::string_view output,
                 const document::NodeId to, const std::string_view input) {
        const bool linked = std::ranges::any_of(current().graph().edges(), [&](const auto& edge) {
            const auto* target = std::get_if<document::NodeInputRef>(&edge.destination);
            return target != nullptr && target->nodeId == to && target->port == input;
        });
        if (linked)
            static_cast<void>(run<commands::DisconnectInput>(
                composition, document::NodeInputRef{to, std::string(input)}));
        checks.expect(run<commands::ConnectPorts>(
                          composition, document::OutputPortRef{from, std::string(output)},
                          document::NodeInputRef{to, std::string(input)}),
                      "a path-nodes link is made");
    }
    // A Shape Source on a layer of its own whose `path` is `from`'s `output`; answers the shape.
    document::NodeId shape(const document::NodeId from, const std::string_view output,
                           const core::Color4d fill) {
        commands::ShapeLayerGeometry geometry;
        commands::ShapeLayerStyle style;
        style.fillColor = fill;
        checks.expect(
            run<commands::AddShapeLayer>(composition, document::ShapeKind::Path, geometry, style),
            "a path-nodes shape layer is added");
        const auto node = last(document::kShapeSourceNodeType);
        connect(from, output, node, "path");
        return node;
    }
    document::NodeId pathNode(document::PathValue value) {
        const auto node = add(document::kPathValueNodeType);
        set(node, document::kValueParameterRole, std::move(value));
        return node;
    }

    void build(const std::int64_t frameCount) {
        using document::PathAnchor;
        using document::PathContour;
        using document::PathValue;
        using document::Vec2d;
        const auto square = [](const double left, const double top, const double size) {
            PathContour contour;
            contour.closed = true;
            contour.anchors = {
                PathAnchor{Vec2d{left, top}, std::nullopt, std::nullopt},
                PathAnchor{Vec2d{left + size, top}, std::nullopt,
                           Vec2d{left + size + 30.0, top + size * 0.5}},
                PathAnchor{Vec2d{left + size, top + size}, std::nullopt, std::nullopt},
                PathAnchor{Vec2d{left, top + size}, std::nullopt, std::nullopt}};
            return contour;
        };
        // 1. Trim Path whose end is keyed from 10 to 90 over the range.
        const auto trimmed = pathNode(PathValue::fromContour(square(20, 20, 90)));
        const auto trim = add(document::kTrimPathNodeType);
        connect(trimmed, document::kValuePortName, trim, "path");
        const auto end = add(document::kScalarValueNodeType);
        const auto keyed = parameter(end, document::kValueParameterRole);
        checks.expect(
            run<commands::SetKeyframeAtTimeForParameter>(composition, keyed, frameTime(0), 10.0) &&
                run<commands::SetKeyframeAtTimeForParameter>(composition, keyed,
                                                             frameTime(frameCount - 1), 90.0),
            "the Trim Path end is keyed");
        connect(end, document::kValuePortName, trim, "end");
        static_cast<void>(shape(trim, "path", {0.9, 0.6, 0.2, 1.0}));
        // 2. Boolean Paths: a curved square less a second one.
        const auto first = pathNode(PathValue::fromContour(square(160, 40, 110)));
        const auto second = pathNode(PathValue::fromContour(square(200, 70, 80)));
        booleanNode = add(document::kBooleanPathsNodeType);
        connect(first, document::kValuePortName, booleanNode, "first");
        connect(second, document::kValuePortName, booleanNode, "second");
        set(booleanNode, "operation", std::int64_t{1});
        static_cast<void>(shape(booleanNode, "path", {0.3, 0.8, 0.4, 1.0}));
        // 3. A dashed Path Stroke with a core, the core's shape through Glow.
        PathContour line;
        line.closed = false;
        line.anchors = {PathAnchor{Vec2d{300, 220}, std::nullopt, Vec2d{360, 150}},
                        PathAnchor{Vec2d{420, 220}, Vec2d{400, 260}, std::nullopt},
                        PathAnchor{Vec2d{490, 170}, std::nullopt, std::nullopt}};
        const auto source = pathNode(PathValue::fromContour(line));
        const auto stroke = add(document::kPathStrokeNodeType);
        connect(source, document::kValuePortName, stroke, "path");
        set(stroke, "width", 12.0);
        set(stroke, "dashLength", 18.0);
        set(stroke, "dashGap", 6.0);
        set(stroke, "coreWidth", 40.0);
        static_cast<void>(shape(stroke, "path", {0.2, 0.4, 1.0, 1.0}));
        const auto core = shape(stroke, "core", {1.5, 1.5, 1.8, 1.0});
        const auto glow = add(document::kGlowNodeType);
        set(glow, "threshold", 0.4);
        set(glow, "size", 14.0);
        connect(core, "image", glow, "input");
        connect(glow, "image", current().graph().layerOutputs().back().nodeId, "image");
    }

    // #349: a keyed Four Color Gradient on the layer `layerNode` shows (`source` is the document
    // then), then the same gradient through Denoise (Auto strength) and Levels whose master input
    // black and white are the gradient's Image Statistics minimum and maximum.
    void buildStatistics(const std::int64_t frameCount, const document::NodeId layerNode,
                         std::optional<document::Snapshot>& source) {
        gradientNode = add(document::kFourColorGradientNodeType);
        const auto color = parameter(gradientNode, "color1");
        checks.expect(run<commands::SetKeyframeAtTimeForParameter>(
                          composition, color, frameTime(0), core::Color4d{0.1, 0.1, 0.1, 1.0}) &&
                          run<commands::SetKeyframeAtTimeForParameter>(
                              composition, color, frameTime(frameCount - 1),
                              core::Color4d{1.6, 0.3, 0.2, 1.0}),
                      "the gradient's first color is keyed");
        set(gradientNode, "color2", core::Color4d{0.3, 0.25, 0.05, 1.0});
        set(gradientNode, "color3", core::Color4d{0.05, 0.2, 0.6, 1.0});
        connect(gradientNode, "image", layerNode, "image");
        source = document.snapshot();
        const auto denoise = add("bloom.denoise");
        set(denoise, "mode", std::int64_t{0});
        set(denoise, "radius", 3.0);
        set(denoise, "autoStrength", true);
        connect(gradientNode, "image", denoise, "input");
        const auto levels = add(document::kLevelsNodeType);
        connect(denoise, "image", levels, "input");
        const auto statistics = add(document::kImageStatisticsNodeType);
        connect(gradientNode, "image", statistics, "input");
        connect(statistics, "minimum", levels, document::levelsRole(0, 0));
        connect(statistics, "maximum", levels, document::levelsRole(0, 1));
        connect(levels, "image", layerNode, "image");
    }
};

// #349: every statistics measure the production stage asked the device route for, with the frame
// it was for, so the run can count them and compare each with the CPU reference's.
struct StatisticsRecord final {
    runtime::ImageStatisticsQuery query;
    runtime::EvaluationRequest frame;
    runtime::ImageStatisticsRun run;
};
struct StatisticsLog final {
    std::mutex mutex;
    std::vector<StatisticsRecord> records;
    // Set: the device route is not available (no evaluator), as before its bootstrap.
    std::atomic_bool refuse{false};
    [[nodiscard]] std::size_t size() {
        const std::scoped_lock lock(mutex);
        return records.size();
    }
    [[nodiscard]] std::vector<StatisticsRecord> since(const std::size_t first) {
        const std::scoped_lock lock(mutex);
        return {records.begin() + static_cast<std::ptrdiff_t>(std::min(first, records.size())),
                records.end()};
    }
};

// The resident frame sampled every `stride` pixels, compared with the CPU oracle (one code per
// colour channel, exact alpha, as the frame evidence compares), and the samples in `gpu`.
[[nodiscard]] bool denseCompare(runtime::GpuPreviewDisplayService& service,
                                const runtime::PreparedPreviewFrame& gpuFrame,
                                const runtime::PreparedPreviewFrame& cpuFrame,
                                const std::uint32_t stride, std::vector<render::Rgba8>& gpu,
                                std::uint32_t& columns, std::uint32_t& rows, std::string& detail) {
    const auto view = cpuFrame.displayBufferView();
    if (!view.has_value() || gpuFrame.residentFrame() == nullptr) {
        detail = "no CPU display buffer or no resident frame";
        return false;
    }
    const auto width = view->displayWindow.extent().width();
    const auto height = view->displayWindow.extent().height();
    columns = (width + stride - 1) / stride;
    rows = (height + stride - 1) / stride;
    std::vector<render::ImagePixelCoordinate> coordinates;
    for (std::uint32_t y = 0; y < height; y += stride)
        for (std::uint32_t x = 0; x < width; x += stride)
            coordinates.push_back({x, y});
    runtime::detail::ResidentSparseSampleResult sampled;
    if (!runtime::detail::GpuPreviewDisplayServiceTestAccess::sampleResidentFrameSparse(
            service, gpuFrame.residentFrame()->lease(), coordinates, sampled,
            std::chrono::seconds(30)) ||
        sampled.pixels.size() != coordinates.size()) {
        detail = "the dense sample was unavailable: " + sampled.diagnostic;
        return false;
    }
    gpu = sampled.pixels;
    std::size_t differing = 0;
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        const auto cpu = view->pixels[static_cast<std::size_t>(coordinates[index].y) * width +
                                      coordinates[index].x];
        const auto& sample = gpu[index];
        const auto delta = [](const std::uint8_t a, const std::uint8_t b) {
            return std::abs(static_cast<int>(a) - static_cast<int>(b));
        };
        if (delta(sample.red, cpu.red) > 1 || delta(sample.green, cpu.green) > 1 ||
            delta(sample.blue, cpu.blue) > 1 || sample.alpha != cpu.alpha)
            ++differing;
    }
    detail =
        std::to_string(differing) + " of " + std::to_string(coordinates.size()) + " samples differ";
    return differing == 0;
}

// A binary PPM of `pixels` (RGB, alpha dropped), `columns` by `rows`.
void writePpm(const std::filesystem::path& path, const std::vector<render::Rgba8>& pixels,
              const std::uint32_t columns, const std::uint32_t rows) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << columns << ' ' << rows << "\n255\n";
    for (const auto& pixel : pixels) {
        const std::array<char, 3> rgb{static_cast<char>(pixel.red), static_cast<char>(pixel.green),
                                      static_cast<char>(pixel.blue)};
        out.write(rgb.data(), rgb.size());
    }
}

// The CPU frame's display pixels sampled every `stride` pixels, as denseCompare samples the GPU's.
[[nodiscard]] std::vector<render::Rgba8> strided(const runtime::PreparedPreviewFrame& frame,
                                                 const std::uint32_t stride) {
    std::vector<render::Rgba8> out;
    const auto view = frame.displayBufferView();
    if (!view.has_value())
        return out;
    const auto width = view->displayWindow.extent().width();
    const auto height = view->displayWindow.extent().height();
    for (std::uint32_t y = 0; y < height; y += stride)
        for (std::uint32_t x = 0; x < width; x += stride)
            out.push_back(view->pixels[static_cast<std::size_t>(y) * width + x]);
    return out;
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    const HarnessOptions options = parseHarnessOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    if (!routeproof::displayPlatformAvailable()) {
        std::cout << "SKIP: RAM route proof needs a real Wayland platform (no display here)\n";
        return options.requireDevice ? 1 : 0;
    }
    QApplication application(argc, argv);
    const QString platformName = QApplication::platformName();
    if (platformName != QStringLiteral("wayland") &&
        platformName != QStringLiteral("wayland-egl")) {
        std::cout << "SKIP: RAM route proof needs a real Wayland platform (platform="
                  << platformName.toStdString() << ")\n";
        return options.requireDevice ? 1 : 0;
    }
    if (options.loader.empty() || (options.proofDirectory.empty() && !options.animatedMask &&
                                   !options.pathNodes && !options.imageStatistics)) {
        std::cout << "SKIP: RAM route proof needs --loader and --proof-dir\n";
        return options.requireDevice ? 1 : 0;
    }
    qputenv("QT_VULKAN_LIB", options.loader.string().c_str());

    const auto settingsRoot =
        std::filesystem::temp_directory_path() /
        ("bloom-ram-route-proof-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(settingsRoot);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(settingsRoot.string()));
    QCoreApplication::setOrganizationName(QStringLiteral("BloomRouteProof"));
    QCoreApplication::setApplicationName(QStringLiteral("RamRouteProof"));

    std::string nonce;
    if (!options.animatedMask && !options.pathNodes && !options.imageStatistics &&
        !routeproof::readProofNonce(options.proofDirectory, nonce)) {
        std::cout << "SKIP: no fresh run nonce in " << options.proofDirectory.string() << '\n';
        return options.requireDevice ? 1 : 0;
    }

    Expectations checks;
    const auto mediaDirectory =
        std::filesystem::temp_directory_path() /
        ("bloom-ram-route-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(mediaDirectory);
    Fixture fixture(mediaDirectory);
    if (fixture.processor == nullptr) {
        std::cout << "SKIP: the Bloom Neutral v1 processor is unavailable\n";
        return options.requireDevice ? 1 : 0;
    }

    std::shared_ptr<const runtime::GpuSceneOcioContext> ocioContext;
    // #349: the device route the Viewer and RAM Preview measure Image Statistics on, as the
    // application wires it: a final-render provider's evaluator, reached through a weak handle so
    // the provider is retired here, before its scheduler.
    // As in the application, the provider and the preview service share one processing root,
    // whose final host payloads retire through a packed host retirement owner.
    runtime::TaskScheduler statisticsScheduler;
    const auto packedHostRetirement = render::PackedHostRetirementOwner::create();
    struct PackedHostTeardown final {
        std::shared_ptr<render::PackedHostRetirementOwner> owner;
        explicit PackedHostTeardown(std::shared_ptr<render::PackedHostRetirementOwner> retiring)
            : owner(std::move(retiring)) {}
        PackedHostTeardown(const PackedHostTeardown&) = delete;
        PackedHostTeardown& operator=(const PackedHostTeardown&) = delete;
        ~PackedHostTeardown() {
            owner->beginShutdown();
            // A destructor cannot assert; a timeout is counted by the helper and fails the exit.
            (void)bloom::test_support::awaitCondition("retirement worker acknowledges its exit",
                                                      [&] { return owner->exitAcknowledged(); });
            (void)bloom::test_support::awaitCondition("retirement worker is collected",
                                                      [&] { return owner->collect(); });
        }
    } packedHostTeardown(packedHostRetirement);
    std::shared_ptr<runtime::GpuProcessingPhasePolicy> processingPolicy;
    std::jthread processingRefresh;
    if (options.imageStatistics) {
        processingPolicy =
            std::make_shared<runtime::GpuProcessingPhasePolicy>(4, packedHostRetirement);
        ui::test::refreshPreviewProcessingPhase(*processingPolicy);
        processingRefresh = ui::test::refreshPreviewProcessingPhaseWhileRunning(processingPolicy);
    }
    std::shared_ptr<host::GpuExportProvider> statisticsProvider;
    struct ProviderRetirement final {
        std::shared_ptr<host::GpuExportProvider>& provider;
        explicit ProviderRetirement(std::shared_ptr<host::GpuExportProvider>& owned)
            : provider(owned) {}
        ProviderRetirement(const ProviderRetirement&) = delete;
        ProviderRetirement& operator=(const ProviderRetirement&) = delete;
        ~ProviderRetirement() {
            if (provider != nullptr)
                static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(30)));
        }
    } statisticsRetirement{statisticsProvider};
    const auto statisticsLog = std::make_shared<StatisticsLog>();
    if (options.imageStatistics) {
        runtime::GpuProcessFrameEvaluatorOptions evaluatorOptions;
        evaluatorOptions.hostMemorySampler = bloom::test_support::fixedMemoryBudgetSample;
        evaluatorOptions.enabled = true;
        evaluatorOptions.loaderPath = options.loader;
        evaluatorOptions.processingPhasePolicy = processingPolicy;
        evaluatorOptions.ocioResolver =
            host::makePackagedGpuOcioResolver(host::currentExecutablePath());
        statisticsProvider = host::GpuExportProvider::create(evaluatorOptions);
        statisticsProvider->prepare(statisticsScheduler);
        if (!awaitProof(
                "the Image Statistics device route prepares",
                [&] { return statisticsProvider->prepared(); }, 90s) ||
            !statisticsProvider->deviceAvailable()) {
            std::cout << "SKIP: the device route that measures Image Statistics is unavailable\n";
            return options.requireDevice ? 1 : 0;
        }
        const auto device = runtime::makeImageStatisticsDeviceMeasure(
            [provider = std::weak_ptr(statisticsProvider),
             log = statisticsLog]() -> std::shared_ptr<runtime::GpuProcessFrameEvaluator> {
                const auto owner = provider.lock();
                return owner == nullptr || log->refuse.load() ? nullptr : owner->evaluator();
            });
        fixture.statisticsMeasure =
            [device, log = statisticsLog](const runtime::ImageStatisticsQuery& query,
                                          const runtime::EvaluationRequest& frame,
                                          const runtime::CancellationToken& token) {
                auto run = device(query, frame, token);
                const std::scoped_lock lock(log->mutex);
                log->records.push_back({query, frame, run});
                return run;
            };
    }
    if (options.animatedMask || options.pathNodes || options.imageStatistics) {
        ocioContext = packagedOcioContext();
        if (ocioContext == nullptr || ocioContext->preparer == nullptr) {
            std::cout << "SKIP: the animated-mask and path-nodes RAM runs need the packaged shader "
                         "tools\n";
            return options.requireDevice ? 1 : 0;
        }
        auto mediaContext = runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator);
        mediaContext.assetBaseDirectory = mediaDirectory;
        // The statistics run's preview frames take their processing phase from the shared root,
        // with the envelope the application's viewer stage states.
        runtime::GpuScenePreparationPhase phase;
        if (processingPolicy != nullptr)
            phase = {
                .policy = processingPolicy,
                .envelope = {.metadataBytes = 16ULL * 1024 * 1024,
                             .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                             .growsRetainedSceneStorage = true,
                             .requestedExecutorBytes = std::nullopt,
                             .deviceWorkingBytes =
                                 512ULL * 1024 * 1024 +
                                 GpuPreviewDisplayServiceOptions{}.packedTransferByteAllowance}};
        fixture.sceneBuilder = std::make_shared<CpuGpuSceneBuilder>(
            nullptr, std::move(mediaContext), *ocioContext, std::move(phase));
    }
    const auto frameCount = options.sequence100    ? std::int64_t{100}
                            : options.animatedMask ? std::int64_t{24}
                            : options.pathNodes    ? std::int64_t{6}
                                                   : kFrameCount;
    const bool smallFrame = options.pathNodes || options.imageStatistics;
    const std::uint32_t frameWidth = smallFrame ? 512U : options.half ? 960U : 1920U;
    const std::uint32_t frameHeight = smallFrame ? 288U : options.half ? 540U : 1080U;
    auto sequenceAsset = fixture.mediaAsset;
    if (options.sequence100) {
        sequenceAsset.kind = document::AssetKind::Sequence;
        sequenceAsset.width = 1920;
        sequenceAsset.height = 1080;
        sequenceAsset.frames = 100;
        sequenceAsset.rate = 25;
        sequenceAsset.manifest.pattern = "ram.####.exr";
        sequenceAsset.manifest.padding = 4;
        sequenceAsset.manifest.first = 0;
        sequenceAsset.manifest.last = 99;
        for (std::int64_t index = 0; index < frameCount; ++index) {
            const auto path = mediaDirectory / ("ram." + std::to_string(index) + ".exr");
            const auto red = static_cast<float>(index + 1) / 128.0F;
            writeExrRgba(
                path, 1920, 1080,
                std::vector<ExrPixel>(std::size_t{1920} * 1080, ExrPixel{red, 0.3F, 0.7F, 1.0F}));
            const auto member = imageAsset(path, "RAM sequence member", 901);
            sequenceAsset.manifest.members.push_back(
                {index, member.locator, member.contentDigest, std::nullopt});
        }
        sequenceAsset.locator = sequenceAsset.manifest.members.front().locator;
        sequenceAsset.contentDigest = *sequenceAsset.manifest.members.front().contentDigest;
        fixture.evaluator.operationCache()->setByteBudget(std::size_t{256} * 1024 * 1024);
    }
    const auto duration = core::RationalTime::create(frameCount, kFrameRate);
    if (!duration.has_value()) {
        return 2;
    }
    auto newProject = document::makeNewProject("RAM Route Proof", "Main", *duration,
                                               smallFrame ? ramFormat(512U, 288U) : ramFormat());
    if (options.sequence100)
        checks.expect(newProject.project.addAsset(sequenceAsset), "the sequence asset validates");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    std::optional<NodeDocument> pathNodes;
    std::optional<document::Snapshot> statisticsSource;
    if (options.sequence100) {
        commands::Transaction addImage("Add RAM sequence");
        addImage.emplace<commands::AddImageLayer>(compositionId, sequenceAsset.id);
        checks.expect(commands.execute(std::move(addImage)).succeeded(),
                      "the genuine sequence layer is added");
    } else if (options.pathNodes) {
        pathNodes.emplace(NodeDocument{document, commands, compositionId, checks, {}, {}});
        pathNodes->build(frameCount);
        checks.expect(session.setCurrentTime(frameTime(0)) || session.currentTime() == frameTime(0),
                      "the path-nodes session is at frame 0");
    } else if (options.imageStatistics) {
        checks.expect(
            session.addSolidLayer(QStringLiteral("Measured"), core::Color4d{0.2, 0.2, 0.2, 1.0}),
            "the statistics layer is created");
        pathNodes.emplace(NodeDocument{document, commands, compositionId, checks, {}, {}});
        const auto layers = session.composition()->graph().layerOutputs();
        checks.expect(layers.size() == 1, "the composition has the one statistics layer");
        if (!layers.empty())
            pathNodes->buildStatistics(frameCount, layers.front().nodeId, statisticsSource);
        checks.expect(session.setCurrentTime(frameTime(0)) || session.currentTime() == frameTime(0),
                      "the statistics session is at frame 0");
    } else if (options.animatedMask) {
        checks.expect(session.addSolidLayer(QStringLiteral("Masked Solid"),
                                            core::Color4d{0.2, 0.4, 0.8, 1.0}),
                      "the masked RAM solid layer is created");
        const auto layers = session.composition()->graph().layerOutputs();
        checks.expect(layers.size() == 1, "the composition has the one masked layer");
        const auto layerId = layers.empty() ? document::LayerId{} : layers.front().layerId;
        checks.expect(session.setCurrentTime(frameTime(0)) || session.currentTime() == frameTime(0),
                      "the RAM session is at frame 0");
        checks.expect(session.addLayerMask(layerId, maskPathForFrame(0)),
                      "the layer mask is added");
        const auto* layer = session.composition()->graph().findLayer(layerId);
        const bool masked = layer != nullptr && layer->masks.size() == 1;
        checks.expect(masked, "the layer carries the mask");
        if (masked) {
            const auto maskId = layer->masks.front().id;
            checks.expect(session.toggleMaskPathKey(layer->masks.front().path),
                          "the mask path is animated");
            for (std::int64_t index = 0; index < frameCount; ++index) {
                checks.expect(session.setCurrentTime(frameTime(index)) ||
                                  session.currentTime() == frameTime(index),
                              "the RAM session moves to a keyed frame");
                checks.expect(session.commitMaskPath(layerId, maskId, maskPathForFrame(index)),
                              "the mask path is keyed on every frame");
            }
        }
        checks.expect(session.setCurrentTime(frameTime(0)), "the RAM session returns to frame 0");
    } else {
        checks.expect(session.addSolidLayer(QStringLiteral("Moving Solid"),
                                            core::Color4d{0.2, 0.4, 0.8, 1.0}),
                      "the animated RAM solid layer is created");
        checks.expect(session.currentTime() == frameTime(0) || session.setCurrentTime(frameTime(0)),
                      "the RAM session is at frame 0");
        checks.expect(session.toggleKeyframe("position"), "the position keyframe is created");
        checks.expect(session.setCurrentTime(frameTime(frameCount - 1)),
                      "the RAM session moves to the last frame");
        checks.expect(session.setSelectedPosition(12.0, 9.0), "the animated position is set");
        checks.expect(session.setCurrentTime(frameTime(0)), "the RAM session returns to frame 0");
    }

    auto scheduling = schedulerConfig();
    if (options.sequence100)
        scheduling.gpuRequestOwnedByteCapacity = 2ULL << 30;
    TaskScheduler scheduler(scheduling);
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    auto nativeOptions = serviceOptions(options.loader);
    if (processingPolicy != nullptr)
        nativeOptions.processingPhasePolicy = processingPolicy;
    if (options.sequence100) {
        const auto configured = runtime::gpuResidentCapacityPlanConfigured(options.poolBytes);
        nativeOptions.residentLeaseBudgets = {configured.leaseBytes,
                                              static_cast<std::size_t>(configured.leaseEntries)};
        nativeOptions.residentSceneCacheBudgets.maxRetainedBytes = configured.sceneCacheBytes;
        nativeOptions.previewByteAllowance = configured.requestBytes;
    }
    GpuPreviewDisplayService service(scheduler, fixture.productionGpuStage(),
                                     fixture.productionCpuStage(),
                                     fixture.productionCpuFallback(displayPreparer), nativeOptions);
    const bool terminal = awaitProof(
        "the RAM resident service leaves Initializing",
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; }, 90s);
    const auto status = service.status();
    const bool residentReady =
        terminal && status.state == GpuPreviewDisplayServiceState::Ready &&
        status.residentQualification != nullptr && status.residentQualification->eligible() &&
        status.presentationClient != nullptr &&
        status.presentationAvailability == render::GpuPresentationAvailability::Ready;
    if (!residentReady) {
        std::cout << "SKIP: RAM resident route unavailable: " << status.residentDetail << " / "
                  << status.presentationDetail << '\n';
        service.beginShutdown();
        return options.requireDevice ? 1 : 0;
    }
    const auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    checks.expect(core != nullptr, "the service core is reachable");
    const auto epoch =
        core == nullptr
            ? 0U
            : runtime::detail::GpuPreviewDisplayServiceTestAccess::ownershipEpoch(*core);
    checks.expect(epoch > 0U, "the service publishes a genuine nonzero device ownership epoch");

    // The resident route requires a live presentation target on this service generation; attach a
    // real Wayland VkSurfaceKHR through the borrowed instance, exactly as the product viewer does.
    const std::shared_ptr<GpuPresentationClient> client = status.presentationClient;
    const GpuBorrowedInstanceView view = client->instanceView();
    checks.expect(view.valid, "the RAM service client publishes the borrowed instance view");
    if (!view.valid) {
        service.beginShutdown();
        return options.requireDevice ? 1 : 0;
    }
    QVulkanInstance instance;
    // clang-format off
    instance.setVkInstance(reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(view.instance_bits)));  // NOLINT(performance-no-int-to-ptr)
    // clang-format on
    if (!instance.create() || !instance.isValid()) {
        std::cout << "SKIP: QVulkanInstance could not adopt the borrowed instance\n";
        service.beginShutdown();
        return options.requireDevice ? 1 : 0;
    }
    auto window = std::make_unique<QWindow>();
    window->setSurfaceType(QSurface::VulkanSurface);
    window->setVulkanInstance(&instance);
    window->setTitle(QString::fromStdString("BloomRamRouteProof-" +
                                            std::to_string(QCoreApplication::applicationPid())));
    window->resize(320, 240);
    window->show();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    checks.expect(awaitProof("the RAM proof QWindow produced a Wayland VkSurfaceKHR",
                             [&] {
                                 surface = QVulkanInstance::surfaceForWindow(window.get());
                                 return surface != VK_NULL_HANDLE;
                             }),
                  "the RAM proof QWindow produced a Wayland VkSurfaceKHR");
    if (surface == VK_NULL_HANDLE) {
        instance.destroy();
        service.beginShutdown();
        return 1;
    }
    GpuBorrowedSurface borrowed;
    borrowed.surface_bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(surface));
    borrowed.epoch = view.epoch;
    const auto attached = client->attach(borrowed, 320U, 240U);
    checks.expect(attached.code == GpuPresentationPortCode::Accepted,
                  "the RAM service client admitted the Wayland surface");
    checks.expect(awaitProof("the RAM service owner activated the presentation target",
                             [&] {
                                 return client->status(attached.target).state ==
                                        GpuPresentationTargetState::Active;
                             }),
                  "the RAM service owner activated the presentation target");

    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds(1));
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    if (options.sequence100)
        settings.resolutionPolicy = options.half ? runtime::PreviewResolutionPolicy::Half
                                                 : runtime::PreviewResolutionPolicy::Full;
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snap,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides)
        -> runtime::TaskSubmission<runtime::PreviewPreparationResultHandle> {
        return service.submit(std::move(request), snap, identity, limit, overrides);
    };
    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    const auto effective = service.status().residentCapacityPlan;
    frameCache->setGpuResidentLimits(static_cast<std::size_t>(effective.cacheBytes),
                                     static_cast<std::size_t>(effective.cacheEntries));
    std::cout << "CAPACITY pool=" << effective.poolBytes << " cacheBytes=" << effective.cacheBytes
              << " cacheEntries=" << effective.cacheEntries
              << " leaseBytes=" << effective.leaseBytes
              << " leaseEntries=" << effective.leaseEntries
              << " available=" << effective.capacity.availableBytes
              << " request=" << effective.requestBytes << '\n';
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                frameCache, nullptr, submitter);
    ui::RamPreviewController ram(session, controller, scheduler, bridge, pipeline, nullptr,
                                 submitter);
    checks.expect(
        awaitProof("the foreground frame settled before the RAM run",
                   [&] { return controller.state().activity != ui::PreviewActivity::Rendering; }),
        "the foreground frame settled before the RAM run");
    const std::string mode = options.imageStatistics ? "statistics" : "path-nodes";
    // #349: the measures since `first`: each made on the device, one per statistics node and frame,
    // `expected` of them, and each equal to the CPU reference's measure of the same picture for the
    // same frame (minimum, maximum, mean, deviation and count, exactly).
    const auto checkMeasures = [&](const std::size_t first, const std::size_t expected,
                                   const std::string& label) {
        const auto records = statisticsLog->since(first);
        std::set<std::pair<std::uint64_t, std::pair<std::int64_t, std::int64_t>>> distinct;
        std::size_t equal = 0;
        for (const auto& record : records) {
            distinct.insert({record.query.node.value(),
                             {record.query.time.numerator(), record.query.time.denominator()}});
            const bool ready =
                record.run.status.outcome == runtime::ImageStatisticsOutcome::Ready &&
                record.run.values.has_value() &&
                record.run.values->route == runtime::ImageStatisticsRoute::Device;
            checks.expect(ready,
                          label + ": the device measured the picture " + record.run.status.detail);
            if (!ready)
                continue;
            const auto cpu = fixture.evaluator.evaluate(
                record.query.plan, runtime::imageStatisticsRequest(record.frame, record.query), {});
            const auto values =
                cpu.frame() != nullptr
                    ? runtime::readImageStatistics(cpu.frame()->processImage(),
                                                   runtime::ImageStatisticsRoute::CpuReference)
                    : std::nullopt;
            const auto& device = *record.run.values;
            const bool same = values.has_value() && values->minimum == device.minimum &&
                              values->maximum == device.maximum && values->mean == device.mean &&
                              values->deviation == device.deviation &&
                              values->count == device.count;
            equal += same ? 1U : 0U;
            checks.expect(same, label + ": the device statistics equal the CPU reference's");
            std::cout << "MEASURE " << label << " node=" << record.query.node.value()
                      << " t=" << record.query.time.numerator() << '/'
                      << record.query.time.denominator() << " device=(" << device.minimum << ' '
                      << device.maximum << ' ' << device.mean << ' ' << device.deviation << ' '
                      << device.count << ")";
            if (values)
                std::cout << " cpu=(" << values->minimum << ' ' << values->maximum << ' '
                          << values->mean << ' ' << values->deviation << ' ' << values->count
                          << ")";
            std::cout << '\n';
        }
        checks.expect(records.size() == expected && distinct.size() == expected,
                      label + ": each frame's statistics were measured once on the device");
        std::cout << "PHASE statistics-" << label << " measures=" << records.size()
                  << " distinct=" << distinct.size() << " expected=" << expected
                  << " equalToCpu=" << equal << '\n';
    };
    if (options.pathNodes || options.imageStatistics) {
        // The interactive viewer route: the foreground frame is the service's GPU-resident frame,
        // nothing fell back to the CPU, and its pixels are the CPU oracle's.
        const auto viewerFrame = controller.state().frame;
        checks.expect(viewerFrame != nullptr && viewerFrame->provenance().provider ==
                                                    runtime::PreviewDisplayProvider::GpuResident,
                      "the " + mode + " viewer frame is GPU-resident");
        checks.expect(service.status().counters.cpuFallbacks == 0U,
                      "the " + mode + " viewer frame did not fall back to the CPU");
        if (options.imageStatistics) {
            checks.expect(statisticsLog->size() == 2U,
                          "the viewer frame measured its two statistics on the device");
            if (viewerFrame == nullptr || viewerFrame->provenance().provider !=
                                              runtime::PreviewDisplayProvider::GpuResident) {
                const auto state = controller.state();
                std::cout << "statistics viewer: frame=" << (viewerFrame != nullptr)
                          << " activity=" << static_cast<int>(state.activity)
                          << " message=" << state.message.toStdString() << '\n';
                for (const auto& diagnostic : state.diagnostics)
                    std::cout << "  diagnostic: " << diagnostic.code << ' ' << diagnostic.summary
                              << ' ' << diagnostic.detail << '\n';
            }
        }
        if (viewerFrame != nullptr) {
            const auto identity = viewerFrame->desiredIdentity();
            const auto reference = snapshotCpuReferenceAtResolution(
                fixture, session.evaluationSnapshotForTime(identity.time), identity);
            routeproof::FrameEvidence viewerEvidence;
            std::string detail;
            checks.expect(reference != nullptr &&
                              routeproof::buildFrameEvidence(service, *viewerFrame, *reference,
                                                             viewerEvidence, detail),
                          "the " + mode + " viewer frame matches the CPU oracle: " + detail);
            std::cout << "PHASE " << mode << "-viewer provider=GpuResident cpuFallbacks="
                      << service.status().counters.cpuFallbacks
                      << " nativeDispatches=" << service.status().counters.nativeDispatches
                      << " sparseSamples=" << viewerEvidence.sparseBytes / sizeof(render::Rgba8)
                      << '\n';
        }
        if (options.imageStatistics && viewerFrame != nullptr) {
            // A warm viewer refresh of the same frame is served from the frame cache: no native
            // work and no measure.
            const auto beforeRefresh = service.status().counters;
            const auto beforeRefreshMeasures = statisticsLog->size();
            controller.requestRefresh();
            checks.expect(awaitProof("the warm viewer refresh settled",
                                     [&] {
                                         return controller.state().frame != nullptr &&
                                                controller.state().activity !=
                                                    ui::PreviewActivity::Rendering;
                                     }),
                          "the warm viewer refresh settled");
            const auto afterRefresh = service.status().counters;
            checks.expect(afterRefresh.nativeDispatches == beforeRefresh.nativeDispatches &&
                              afterRefresh.cpuFallbacks == beforeRefresh.cpuFallbacks &&
                              statisticsLog->size() == beforeRefreshMeasures,
                          "the warm viewer refresh dispatched and measured nothing");
            std::cout << "PHASE statistics-viewer-warm dispatches="
                      << (afterRefresh.nativeDispatches - beforeRefresh.nativeDispatches)
                      << " measures=" << (statisticsLog->size() - beforeRefreshMeasures) << '\n';
        }
    }
    if (options.expireBeforeAdmission) {
        frameCache->setByteBudget(ui::PreviewFrameCache::frameByteCost(*controller.state().frame));
    } else {
        frameCache->clear();
    }

    const auto preparerCounters = [&ocioContext] {
        return ocioContext != nullptr ? ocioContext->preparer->counters()
                                      : runtime::GpuOcioPreparerCounters{};
    };
    const auto beforeColdPreparer = preparerCounters();
    const auto beforeCold = service.status().counters;
    const auto coldStart = std::chrono::steady_clock::now();
    ram.start();
    if (options.expireBeforeAdmission) {
        // Hold UI delivery while genuine production tasks finish, then invalidate their real
        // leases through normal owner shutdown. No fake GPU product or foreign registry mutation.
        // The wait must not pump the event loop: UI delivery is held on purpose.
        checks.expect(bloom::test_support::awaitCondition("pending RAM products settle",
                                                          [&] { return scheduler.isQuiescent(); }),
                      "genuine pending RAM products settled before expiry");
        static_cast<void>(client->retire(attached.target, 5U));
        if (!bloom::test_support::awaitCondition(
                "the expiry-fixture surface retires and is safe to destroy",
                [&] { return client->status(attached.target).surfaceSafeToDestroy; })) {
            std::cerr << "FATAL: unproved expiry-fixture surface retirement\n";
            std::_Exit(1);
        }
        window->hide();
        window.reset();
        instance.destroy();
        service.beginShutdown();
        checks.expect(
            bloom::test_support::awaitCondition(
                "the service owner stops",
                [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; }),
            "owner shutdown invalidated the genuine leases before UI admission");
        auto foreign = *controller.cacheKeyForTime(frameTime(0));
        foreign.resolutionPolicy = runtime::PreviewResolutionPolicy::Half;
        const auto halfExtent = render::ImageExtent::create(960, 540);
        foreign.resolution = runtime::ProxyResolution{*halfExtent.value()};
        runtime::PreviewRequestIdentity foreignIdentity =
            controller.state().frame->desiredIdentity();
        foreignIdentity.resolutionPolicy = foreign.resolutionPolicy;
        foreignIdentity.resolution = foreign.resolution;
        const auto foreignFrame =
            snapshotCpuReferenceAtResolution(fixture, session.snapshot(), foreignIdentity);
        checks.expect(foreignFrame != nullptr, "the foreign-policy CPU oracle is genuine");
        if (foreignFrame) {
            const auto inserted = frameCache->insert(foreignFrame);
            checks.expect(inserted.evictions != 0 && frameCache->statistics().staleDrops != 0,
                          "a foreign-policy admission evicts an expired requested lease with "
                          "explicit stale evidence");
        }
    }
    const bool coldComplete =
        awaitProof("the cold RAM fill finishes", [&] { return !ram.isCaching(); }, 120s);
    const auto coldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - coldStart)
                            .count();
    const auto afterCold = service.status().counters;
    const auto afterColdPreparer = preparerCounters();
    const auto coldDispatches = afterCold.nativeDispatches - beforeCold.nativeDispatches;
    if (options.pathNodes || options.imageStatistics) {
        checks.expect(afterCold.cpuFallbacks == beforeCold.cpuFallbacks,
                      "every " + mode + " frame rendered on the GPU (no CPU fallback)");
        checks.expect(coldDispatches > 0, "the cold " + mode + " fill dispatched native work");
        if (options.imageStatistics)
            checkMeasures(0, 2U * static_cast<std::size_t>(frameCount), "viewer+ram-cold");
        std::cout << "PHASE " << mode << "-cold frames=" << ram.cachedFrameCount()
                  << " coldDispatches=" << coldDispatches
                  << " graphJobs=" << (afterCold.residentGraphJobs - beforeCold.residentGraphJobs)
                  << " cpuFallbacks=" << (afterCold.cpuFallbacks - beforeCold.cpuFallbacks) << '\n';
    }
    if (options.animatedMask) {
        checks.expect(afterCold.cpuFallbacks == beforeCold.cpuFallbacks,
                      "every animated-mask frame rendered on the GPU (no CPU fallback)");
        checks.expect(afterColdPreparer.shaderCompilerRuns <= 1,
                      "the whole animated-mask run compiled at most one shader module");
        std::cout << "PHASE mask-cold frames=" << ram.cachedFrameCount()
                  << " coldDispatches=" << coldDispatches << " prepareRequests="
                  << (afterColdPreparer.preparations - beforeColdPreparer.preparations)
                  << " shaderCompilerRunsCold="
                  << (afterColdPreparer.shaderCompilerRuns - beforeColdPreparer.shaderCompilerRuns)
                  << " shaderCompilerRunsTotal=" << afterColdPreparer.shaderCompilerRuns
                  << " compilerCacheHits="
                  << (afterColdPreparer.shaderCompilerCacheHits -
                      beforeColdPreparer.shaderCompilerCacheHits)
                  << " ms=" << coldMs << '\n';
    }
    checks.expect(coldComplete && ram.totalFrameCount() == static_cast<std::uint64_t>(frameCount),
                  "the requested RAM range terminates");
    const auto expectedCompletion =
        options.expireBeforeAdmission ? ui::RamPreviewController::Completion::Failed
        : options.expectPartial       ? ui::RamPreviewController::Completion::PartialBudget
                                      : ui::RamPreviewController::Completion::Complete;
    checks.expect(ram.lastCompletion() == expectedCompletion,
                  "RAM completion reports measured full/partial/expired admission");
    checks.expect((options.expectPartial || options.expireBeforeAdmission) ? !ram.canAutoPlay()
                                                                           : ram.canAutoPlay(),
                  "only fully retained current coverage permits autoplay");
    std::cout << "RETENTION requested=" << ram.totalFrameCount()
              << " prepared=" << ram.preparedFrameCount() << " retained=" << ram.cachedFrameCount()
              << " charged=" << frameCache->gpuResidentBytes();
    if (ram.playableSpan())
        std::cout << " spanStart=" << ram.playableSpan()->start.toSeconds()
                  << " spanEnd=" << ram.playableSpan()->end.toSeconds()
                  << " spanFrames=" << ram.playableSpan()->frameCount;
    std::cout << '\n';
    checks.expect(afterCold.fullFrameReadbacks == beforeCold.fullFrameReadbacks,
                  "the cold RAM preview performed no full-frame readback");
    std::cout << "PHASE ram-cold frames=" << ram.cachedFrameCount()
              << " coldDispatches=" << coldDispatches << " epoch=" << epoch << '\n';

    const auto identityForIndex = [](const std::int64_t index,
                                     const ui::PreviewFrameCacheKey& key) {
        return runtime::PreviewRequestIdentity{.projectId = key.projectId,
                                               .compositionId = key.compositionId,
                                               .sourceRevision = key.sourceRevision,
                                               .requestGeneration =
                                                   static_cast<std::uint64_t>(index + 1),
                                               .time = key.time,
                                               .output = key.output,
                                               .resolution = key.resolution,
                                               .quality = key.quality,
                                               .colorIntent = key.colorIntent,
                                               .resolutionPolicy = key.resolutionPolicy,
                                               .roi = key.roi,
                                               .viewAdjust = key.viewAdjust,
                                               .displayName = key.displayName,
                                               .viewName = key.viewName,
                                               .showLook = key.showLook};
    };

    std::vector<routeproof::FrameEvidence> evidence;
    std::vector<routeproof::PreparedPreviewFrameHandle> frames;
    for (const auto retainedIndex : ram.retainedFrameIndices()) {
        const auto index = static_cast<std::int64_t>(retainedIndex);
        const auto key = controller.cacheKeyForTime(frameTime(index));
        checks.expect(key.has_value(), "the cached frame has a cache key");
        if (!key.has_value()) {
            continue;
        }
        checks.expect(frameCache->contains(*key), "the cached frame is retained in the cache");
        const auto identity = identityForIndex(index, *key);
        auto frame = frameCache->take(identity);
        checks.expect(frame != nullptr && frame->provenance().provider ==
                                              runtime::PreviewDisplayProvider::GpuResident,
                      "the RAM frame is a genuine GPU-resident frame");
        if (frame == nullptr) {
            continue;
        }
        checks.expect(frame->isDisplayValid(), "the resident RAM lease is live");
        const auto& lease = frame->residentFrame()->lease();
        const auto width = frameWidth;
        const auto height = frameHeight;
        checks.expect(lease.width() == width && lease.height() == height,
                      "the native retained image has the actual Full/Half dimensions");
        checks.expect(lease.allocationBytes() >= static_cast<std::uint64_t>(width) * height * 4U,
                      "actual native RGBA8 allocation charges cover the physical extent");
        std::cout << "FRAME index=" << index << " width=" << lease.width()
                  << " height=" << lease.height() << " nativeBytes=" << lease.allocationBytes()
                  << " retainedBytes=" << ui::PreviewFrameCache::frameByteCost(*frame) << '\n';

        const auto reference = snapshotCpuReferenceAtResolution(
            fixture, session.evaluationSnapshotForTime(key->time), identity);
        checks.expect(reference != nullptr && reference->displayBufferView().has_value(),
                      "the CPU oracle evaluates the RAM frame identity into real pixels");
        if (reference != nullptr) {
            std::string mismatchDetail;
            checks.expect(routeproof::sameOracleIdentity(*frame, *reference, mismatchDetail),
                          "the RAM frame matches the CPU oracle identity/geometry: " +
                              mismatchDetail);
            routeproof::FrameEvidence frameEvidence;
            std::string evidenceDetail;
            checks.expect(routeproof::buildFrameEvidence(service, *frame, *reference, frameEvidence,
                                                         evidenceDetail),
                          "the actual resident GPU pixels match the CPU oracle: " + evidenceDetail);
            if (checks.failures() == 0) {
                evidence.push_back(std::move(frameEvidence));
            }
            if (options.imageStatistics) {
                // Levels follows each frame's measured statistics: every 4th pixel of the GPU
                // frame against the CPU oracle, and the first and last frames into the montage.
                std::vector<render::Rgba8> gpu;
                std::uint32_t columns = 0;
                std::uint32_t rows = 0;
                std::string denseDetail;
                checks.expect(
                    denseCompare(service, *frame, *reference, 4U, gpu, columns, rows, denseDetail),
                    "the statistics frame matches the CPU oracle at every 4th pixel: " +
                        denseDetail);
                std::cout << "DENSE index=" << index << ' ' << denseDetail << '\n';
                if (!options.montage.empty() && (index == 0 || index == frameCount - 1)) {
                    std::filesystem::create_directories(options.montage);
                    const auto stem = "statistics-frame" + std::to_string(index);
                    writePpm(options.montage / (stem + "-gpu.ppm"), gpu, columns, rows);
                    writePpm(options.montage / (stem + "-cpu.ppm"), strided(*reference, 4U),
                             columns, rows);
                    if (statisticsSource) {
                        const auto source =
                            snapshotCpuReferenceAtResolution(fixture, *statisticsSource, identity);
                        if (source != nullptr)
                            writePpm(options.montage / (stem + "-source.ppm"), strided(*source, 4U),
                                     columns, rows);
                    }
                }
            }
        }
        frames.push_back(std::move(frame));
    }
    std::uint64_t sparseBytes = 0;
    std::uint64_t sparseSubmissions = 0;
    for (const auto& frameEvidence : evidence) {
        sparseBytes += frameEvidence.sparseBytes;
        sparseSubmissions += frameEvidence.sparseSubmissions;
    }
    const auto processDigest = routeproof::frameIdentityDigest(frames);
    const auto capturedEvidenceDigest = routeproof::evidenceDigest(evidence);

    // Warm replay: the same range must be answered entirely from the controller cache with zero new
    // native work, and every cached resident frame must actually replay through the real presenter
    // (taking it back out of the controller cache and presenting it), not merely still be held.
    const auto beforeWarm = service.status().counters;
    const auto beforeWarmMeasures = statisticsLog->size();
    const auto beforeWarmPreparer = preparerCounters();
    const auto warmStart = std::chrono::steady_clock::now();
    if (!options.expectPartial && !options.expireBeforeAdmission)
        ram.start();
    const bool warmComplete =
        awaitProof("the warm RAM pass finishes", [&] {
            return !ram.isCaching() &&
                   (options.expectPartial || options.expireBeforeAdmission ||
                    ram.cachedFrameCount() == static_cast<std::uint64_t>(frameCount));
        });
    const auto afterWarm = service.status().counters;
    if (options.animatedMask) {
        const auto afterWarmPreparer = preparerCounters();
        checks.expect(afterWarmPreparer.shaderCompilerRuns == beforeWarmPreparer.shaderCompilerRuns,
                      "the warm animated-mask replay compiled nothing");
        std::cout << "PHASE mask-warm cached=" << ram.cachedFrameCount() << " warmDispatches="
                  << (afterWarm.nativeDispatches - beforeWarm.nativeDispatches) << " warmGraphJobs="
                  << (afterWarm.residentGraphJobs - beforeWarm.residentGraphJobs)
                  << " prepareRequests="
                  << (afterWarmPreparer.preparations - beforeWarmPreparer.preparations)
                  << " shaderCompilerRuns="
                  << (afterWarmPreparer.shaderCompilerRuns - beforeWarmPreparer.shaderCompilerRuns)
                  << " ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - warmStart)
                         .count()
                  << '\n';
    }
    checks.expect(warmComplete, "the warm RAM replay completed from the cache");
    checks.expect(afterWarm.residentGraphJobs == beforeWarm.residentGraphJobs &&
                      afterWarm.cpuFallbacks == beforeWarm.cpuFallbacks &&
                      (options.expectPartial || options.expireBeforeAdmission ||
                       ram.preparedFrameCount() == 0),
                  "warm range reuse performs zero preparation, including no CPU fallback");
    checks.expect(afterWarm.nativeDispatches == beforeWarm.nativeDispatches,
                  "the warm RAM replay performed zero additional native dispatches");
    checks.expect(afterWarm.fullFrameReadbacks == beforeWarm.fullFrameReadbacks,
                  "the warm RAM replay performed no full-frame readback");
    std::size_t replayed = 0;
    std::uint32_t sequence = 0;
    auto replayIndices = ram.retainedFrameIndices();
    if (options.sequence100) {
        const auto forward = replayIndices;
        replayIndices.insert(replayIndices.end(), forward.rbegin(), forward.rend());
    }
    for (const auto retainedIndex : replayIndices) {
        const auto index = static_cast<std::int64_t>(retainedIndex);
        const auto key = controller.cacheKeyForTime(frameTime(index));
        if (!key.has_value()) {
            continue;
        }
        const auto frame = frameCache->take(identityForIndex(index, *key));
        if (frame == nullptr || !frame->isDisplayValid() ||
            frame->provenance().provider != runtime::PreviewDisplayProvider::GpuResident) {
            continue;
        }
        const auto lease = frame->residentFrame()->lease();
        // Give the Wayland compositor time to release the previous swapchain image before the next
        // present; a tight back-to-back present loop can otherwise starve vkAcquireNextImageKHR.
        {
            const auto settleUntil = std::chrono::steady_clock::now() + 150ms;
            while (std::chrono::steady_clock::now() < settleUntil) {
                QApplication::processEvents();
                std::this_thread::sleep_for(2ms);
            }
        }
        const auto update =
            client->update(attached.target, sequence + 1U, makeUpdate(lease, 320U, 240U));
        if (!update.accepted()) {
            continue;
        }
        ++sequence;
        // Wait for the present to COMPLETE (presentCount), not merely to be submitted
        // (appliedSequence), so the next replay never replaces an in-flight present.
        const bool applied = awaitProof("the replayed present completes", [&] {
            return client->status(attached.target).presentCount >= sequence;
        });
        std::cout << "PHASE ram-replay index=" << index << " code=" << static_cast<int>(update.code)
                  << " seq=" << sequence
                  << " presentCount=" << client->status(attached.target).presentCount
                  << " state=" << static_cast<int>(client->status(attached.target).state)
                  << " targetCode=" << static_cast<int>(client->status(attached.target).targetCode)
                  << " message=" << client->status(attached.target).message << '\n';
        if (!applied) {
            continue;
        }
        ++replayed;
    }
    checks.expect(
        replayed == replayIndices.size(),
        "every cached resident frame replayed through the real controller cache and presenter");
    const auto afterPresentation = service.status().counters;
    checks.expect(afterPresentation.residentGraphJobs == beforeWarm.residentGraphJobs &&
                      afterPresentation.cpuFallbacks == beforeWarm.cpuFallbacks &&
                      afterPresentation.nativeDispatches == beforeWarm.nativeDispatches &&
                      afterPresentation.fullFrameReadbacks == beforeWarm.fullFrameReadbacks,
                  "forward/reverse warm presentation performs zero evaluation or pixel dispatch");
    if (options.pathNodes || options.imageStatistics) {
        checks.expect(statisticsLog->size() == beforeWarmMeasures,
                      "the warm replay measured no statistics");
        std::cout << "PHASE " << mode << "-warm cached=" << ram.cachedFrameCount()
                  << " warmDispatches="
                  << (afterWarm.nativeDispatches - beforeWarm.nativeDispatches) << " warmGraphJobs="
                  << (afterWarm.residentGraphJobs - beforeWarm.residentGraphJobs)
                  << " warmMeasures=" << (statisticsLog->size() - beforeWarmMeasures)
                  << " replayed=" << replayed << '\n';
        // An operand edit (Boolean Paths: Subtract to Intersect; the gradient's third color)
        // changes every frame: the next fill re-renders each on the GPU, and each matches the
        // edited document's CPU oracle.
        const auto beforeEditMeasures = statisticsLog->size();
        if (options.imageStatistics)
            pathNodes->set(pathNodes->gradientNode, "color3", core::Color4d{0.05, 0.3, 1.4, 1.0});
        else
            pathNodes->set(pathNodes->booleanNode, "operation", std::int64_t{2});
        checks.expect(awaitProof("the viewer settled after the operand edit",
                                 [&] {
                                     return controller.state().activity !=
                                            ui::PreviewActivity::Rendering;
                                 }),
                      "the viewer settled after the operand edit");
        const auto beforeEdit = service.status().counters;
        ram.start();
        const bool editComplete =
            awaitProof("the edited range refills", [&] { return !ram.isCaching(); }, 120s);
        const auto afterEdit = service.status().counters;
        checks.expect(editComplete &&
                          ram.cachedFrameCount() == static_cast<std::uint64_t>(frameCount),
                      "the edited range refills");
        checks.expect(afterEdit.nativeDispatches > beforeEdit.nativeDispatches &&
                          afterEdit.cpuFallbacks == beforeEdit.cpuFallbacks,
                      "the edit re-renders the range on the GPU, not from the stale cache");
        std::size_t editedMatches = 0;
        for (const auto retainedIndex : ram.retainedFrameIndices()) {
            const auto index = static_cast<std::int64_t>(retainedIndex);
            const auto key = controller.cacheKeyForTime(frameTime(index));
            if (!key.has_value())
                continue;
            const auto identity = identityForIndex(index, *key);
            const auto frame = frameCache->take(identity);
            if (frame == nullptr ||
                frame->provenance().provider != runtime::PreviewDisplayProvider::GpuResident)
                continue;
            const auto reference = snapshotCpuReferenceAtResolution(
                fixture, session.evaluationSnapshotForTime(key->time), identity);
            routeproof::FrameEvidence editedEvidence;
            std::string detail;
            const bool matched =
                reference != nullptr &&
                routeproof::buildFrameEvidence(service, *frame, *reference, editedEvidence, detail);
            checks.expect(matched,
                          "an edited " + mode + " frame matches the edited CPU oracle: " + detail);
            editedMatches += matched ? 1U : 0U;
        }
        checks.expect(editedMatches == static_cast<std::size_t>(frameCount),
                      "every edited frame was verified against the CPU oracle");
        std::cout << "PHASE " << mode << "-edit frames=" << editedMatches
                  << " dispatches=" << (afterEdit.nativeDispatches - beforeEdit.nativeDispatches)
                  << " cpuFallbacks=" << (afterEdit.cpuFallbacks - beforeEdit.cpuFallbacks) << '\n';
        if (options.imageStatistics)
            checkMeasures(beforeEditMeasures, 2U * static_cast<std::size_t>(frameCount), "edit");
    }
    if (options.imageStatistics) {
        // With the device route refused (no evaluator, as before its bootstrap), the frame is not
        // prepared with unmeasured statistics: it takes the CPU path, labelled CPU, and shows the
        // CPU reference's pixels.
        statisticsLog->refuse.store(true);
        const auto beforeRefusal = service.status().counters;
        const auto beforeRefusalMeasures = statisticsLog->size();
        const auto previous = controller.state().frame;
        pathNodes->set(pathNodes->gradientNode, "color3", core::Color4d{0.4, 0.1, 0.9, 1.0});
        const bool shown = awaitProof(
            "the refused-measure edit shows a new frame",
            [&] {
                const auto frame = controller.state().frame;
                return frame != nullptr && frame != previous &&
                       controller.state().activity != ui::PreviewActivity::Rendering;
            },
            60s);
        const auto frame = controller.state().frame;
        const auto afterRefusal = service.status().counters;
        const auto provider = frame != nullptr ? frame->provenance().provider
                                               : runtime::PreviewDisplayProvider::GpuResident;
        checks.expect(shown && (provider == runtime::PreviewDisplayProvider::CpuReference ||
                                provider == runtime::PreviewDisplayProvider::CpuOcio),
                      "with the device route refused the viewer frame is a CPU frame");
        constexpr auto subset =
            static_cast<std::size_t>(runtime::GpuPreviewCpuFallbackReason::UnsupportedGpuSubset);
        checks.expect(afterRefusal.cpuFallbacksByReason[subset] >
                          beforeRefusal.cpuFallbacksByReason[subset],
                      "the refused frame was counted as a CPU fallback outside the GPU subset");
        const auto refused = statisticsLog->since(beforeRefusalMeasures);
        checks.expect(
            !refused.empty() &&
                std::ranges::all_of(refused,
                                    [](const auto& record) {
                                        return record.run.status.outcome ==
                                                   runtime::ImageStatisticsOutcome::Unavailable &&
                                               !record.run.values.has_value();
                                    }),
            "the refused measure is Unavailable and gives no values");
        bool samePixels = false;
        if (frame != nullptr) {
            const auto reference = snapshotCpuReferenceAtResolution(
                fixture, session.evaluationSnapshotForTime(frame->desiredIdentity().time),
                frame->desiredIdentity());
            const auto shownView = frame->displayBufferView();
            const auto referenceView =
                reference != nullptr ? reference->displayBufferView() : std::nullopt;
            samePixels = shownView && referenceView &&
                         std::ranges::equal(shownView->pixels, referenceView->pixels);
        }
        checks.expect(samePixels, "the CPU frame shows the CPU reference's pixels");
        std::cout << "PHASE statistics-refused provider=" << static_cast<int>(provider)
                  << " subsetFallbacks="
                  << (afterRefusal.cpuFallbacksByReason[subset] -
                      beforeRefusal.cpuFallbacksByReason[subset])
                  << " measures=" << refused.size() << " detail="
                  << (refused.empty() ? std::string{} : refused.front().run.status.detail)
                  << " samePixels=" << samePixels << '\n';
        for (const auto& diagnostic : controller.state().diagnostics)
            std::cout << "  diagnostic: " << diagnostic.code << ' ' << diagnostic.summary << '\n';
        statisticsLog->refuse.store(false);
    }
    std::cout << "PHASE ram-warm cached=" << ram.cachedFrameCount()
              << " warmDispatches=" << (afterWarm.nativeDispatches - beforeWarm.nativeDispatches)
              << " replayed=" << replayed
              << " sparseSamples=" << sparseBytes / sizeof(render::Rgba8)
              << " sparseBytes=" << sparseBytes << " sparseSubmissions=" << sparseSubmissions
              << " productionFullFrameReadbacks=" << afterWarm.fullFrameReadbacks << '\n';

    // The expiry case already retired this surface before invalidating the borrowed instance.
    if (window) {
        static_cast<void>(client->retire(attached.target, 5U));
        const bool safe = awaitProof("the RAM proof surface retires and is safe to destroy", [&] {
            return client->status(attached.target).surfaceSafeToDestroy;
        });
        if (!safe) {
            std::cerr << "FATAL: unproved RAM proof surface retirement\n";
            std::_Exit(1);
        }
        window->hide();
        window.reset();
        instance.destroy();
    }

    ram.beginShutdown();
    controller.beginShutdown();
    bridge.beginShutdown();
    checks.expect(awaitProof("scheduler quiescent after the bridge shutdown",
                             [&] { return scheduler.isQuiescent(); }),
                  "the RAM proof scheduler reaches quiescence");
    service.beginShutdown();
    checks.expect(awaitProof("the service owner stops",
                             [&] {
                                 return service.status().state ==
                                        GpuPreviewDisplayServiceState::Stopped;
                             }),
                  "the RAM proof service stops");
    checks.expect(service.status().counters.fullFrameReadbacks == 0U,
                  "the whole RAM run performed no full-frame readback");
    std::error_code ignored;
    std::filesystem::remove_all(mediaDirectory, ignored);

    if (checks.failures() != 0) {
        std::cerr << "RAM route proof: " << checks.failures()
                  << " verification failure(s); no proof written\n";
        return 1;
    }
    if (options.imageStatistics) {
        std::cout << "PASS: Image Statistics measured on the device in the viewer and RAM preview "
                     "(frames="
                  << frames.size() << ", measures=" << statisticsLog->size() << ")\n";
        return 0;
    }
    if (options.pathNodes) {
        std::cout << "PASS: path-nodes viewer and RAM preview on the GPU (frames=" << frames.size()
                  << ", coldDispatches=" << coldDispatches << ")\n";
        return 0;
    }
    if (options.animatedMask) {
        std::cout << "PASS: animated-mask RAM preview (frames=" << frames.size()
                  << ", coldDispatches=" << coldDispatches
                  << ", shader compiles=" << preparerCounters().shaderCompilerRuns << ")\n";
        return 0;
    }
    if (options.expireBeforeAdmission) {
        checks.expect(ram.cachedFrameCount() == 0 && !ram.canAutoPlay(),
                      "expired admission is failure with zero live coverage");
        std::cout << "PASS: genuine expired RAM admission reports failure, not budget completion\n";
        return checks.failures() == 0 ? 0 : 1;
    }
    const auto written = routeproof::publishPreviewProof(
        options.proofDirectory, nonce, "route.preview.ram_preview",
        runtime::GpuRouteHarnessKind::RamPreview, epoch, coldDispatches, frames.size(),
        processDigest, capturedEvidenceDigest);
    if (!written.written) {
        std::cerr << "RAM route proof was rejected: " << written.detail << '\n';
        return 1;
    }
    std::cout << "PASS: route.preview.ram_preview proof written (frames=" << frames.size()
              << ", coldDispatches=" << coldDispatches << ", epoch=" << epoch << ")\n";
    return 0;
}
