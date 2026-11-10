// #645 Viewer isolation: while the Viewer is inside a group, every layer outside it draws at 30% of
// its own opacity (docs/user-guide/viewer.md, "Drilling into groups"). The compiler stamps the
// factor on the layers document::drillIsolationDimmed names and both routes read it through the
// one layer-opacity resolution, so this proves, on a real imported SVG (three nested groups, a
// gradient fill, a clip) beside a one-layer SVG:
//   * the dimmed set: the siblings off the entered chain at every level, never a matte source and
//     nothing inside the entered group; a plan without isolation (every export) is unchanged;
//   * on the CPU reference at 512 x 288 over three frames with animated siblings (a keyed opacity
//     and a keyed position): outside layers are exactly 0.3 times their undimmed pixels, the
//     entered group's pixels are untouched, and the CPU operation cache never serves an undimmed
//     image to the dimmed plan or the reverse;
//   * the compiled-plan cache keys on the isolation;
//   * on the production GPU route (CpuGpuSceneBuilder -> GpuSceneExecutor): every dimmed and
//     undimmed frame matches its CPU reference (2e-6) cold and warm, interleaved on one executor so
//     its caches never cross-serve, and the dimmed frame at 1920 x 1080 and 3840 x 2160 prepares and
//     runs on the device with no refusal.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"
#include "gpu_coverage_ocio_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/drill_levels.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/compiled_plan_cache.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::uint32_t kWidth = 512;
constexpr std::uint32_t kHeight = 288;

// Top level: the SVG's root group and, above it, a one-layer badge. Inside the root: backdrop and
// outer; outer holds gradbox (a gradient fill: a small group) and middle; middle holds clipped
// (its clip a matte source) and inner; inner holds ring and tri.
constexpr auto kArtwork =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="288" viewBox="0 0 512 288">
<defs>
<linearGradient id="fade"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient>
<clipPath id="window"><rect x="300" y="200" width="40" height="40"/></clipPath>
</defs>
<rect id="backdrop" x="0" y="0" width="60" height="60" fill="#808080"/>
<g id="outer">
<rect id="gradbox" x="20" y="200" width="100" height="60" fill="url(#fade)"/>
<g id="middle">
<rect id="clipped" x="280" y="180" width="100" height="100" fill="#00ff00" clip-path="url(#window)"/>
<g id="inner">
<path id="ring" fill="#336699" fill-rule="evenodd" d="M100 40H220V160H100Z M130 70H180V120H130Z"/>
<path id="tri" fill="#cc3333" d="M180 100L260 100L260 180Z"/>
</g>
</g>
</g>
</svg>
)svg";

constexpr auto kBadge =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="288" viewBox="0 0 512 288">
<rect id="badge" x="430" y="200" width="50" height="50" fill="#33cc66"/>
</svg>
)svg";

[[nodiscard]] std::shared_ptr<const commands::PreparedSvgSource> prepared(const char* text,
                                                                         const char* file) {
    const auto size = std::strlen(text);
    auto bytes = std::make_shared<std::vector<std::byte>>(size);
    std::memcpy(bytes->data(), text, size);
    auto read = commands::prepareSvgSource(std::move(bytes), file);
    if (!read.source)
        throw std::runtime_error("the SVG could not be prepared: " + read.diagnostic);
    return read.source;
}

[[nodiscard]] core::RationalTime frameTime(const std::int64_t frame) {
    return *core::RationalTime::create(frame, 24);
}

struct Fixture final {
    document::NewProject initial;
    document::CompositionId main;
    document::Document document;
    commands::CommandStack commands;
    document::LayerId root, badge, backdrop, outer, gradbox, middle, clipped, clip, inner, ring;

    Fixture(const std::uint32_t width, const std::uint32_t height)
        : initial(document::makeNewProject(
              "Isolation", "Main", core::RationalTime::fromInteger(4),
              *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                   *document::FrameRate::create(24, 1)))),
          main(initial.initialCompositionId), document(std::move(initial.project)),
          commands(document) {
        root = import(kArtwork, commands::SvgImportMode::Shapes, "Art");
        badge = import(kBadge, commands::SvgImportMode::Layer, "Badge");
        backdrop = child(root, "backdrop");
        outer = child(root, "outer");
        gradbox = child(outer, "gradbox");
        middle = child(outer, "middle");
        clipped = child(middle, "clipped");
        clip = child(middle, "Clip window");
        inner = child(middle, "inner");
        ring = child(inner, "ring");
        // Animated siblings outside the group: the backdrop fades and the badge moves.
        // A first key holds the constant's own value (no change), so the last key goes in first.
        key(backdrop, document::kOpacityParameterRole, 2, 0.5);
        key(backdrop, document::kOpacityParameterRole, 0, 1.0);
        const auto at = position(badge);
        key(badge, document::kPositionParameterRole, 2, document::Vec2d{at.x - 12.0, at.y - 6.0});
        key(badge, document::kPositionParameterRole, 0, at);
    }

    [[nodiscard]] const document::Composition& current() const {
        return composition(document, main);
    }
    document::LayerId import(const char* text, const commands::SvgImportMode mode,
                             const std::string& name) {
        commands::Transaction transaction("Import " + name, document.snapshot().revision());
        transaction.emplace<commands::AddSvgArtwork>(main, prepared(text, "isolation.svg"), name,
                                                     mode, true);
        const auto result = commands.execute(std::move(transaction));
        const auto layer = result.outputId<document::LayerId>("layer");
        if (!result.succeeded() || !layer)
            throw std::runtime_error("the SVG import failed: " + name);
        return *layer;
    }
    [[nodiscard]] document::LayerId child(const document::LayerId parent,
                                          const std::string_view name) const {
        const auto merge = document::layerContentMerge(current(), parent);
        if (merge)
            for (const auto layer : document::mergeEntryLayers(current(), *merge))
                if (const auto* boundary = current().graph().findLayer(layer);
                    boundary != nullptr && boundary->name == name)
                    return layer;
        throw std::runtime_error("no layer named " + std::string(name));
    }
    [[nodiscard]] document::ParameterId role(const document::LayerId layer,
                                             const std::string_view name) const {
        const auto* boundary = current().graph().findLayer(layer);
        const auto* binding =
            boundary ? document::layerParameterBinding(current().graph(), *boundary, name) : nullptr;
        if (binding == nullptr)
            throw std::runtime_error("the layer has no " + std::string(name));
        return binding->parameterId;
    }
    [[nodiscard]] document::Vec2d position(const document::LayerId layer) const {
        const auto* record = current().parameters().find(role(layer, document::kPositionParameterRole));
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        const auto* value = constant ? std::get_if<document::Vec2d>(&constant->value) : nullptr;
        if (value == nullptr)
            throw std::runtime_error("the badge's position is not a constant");
        return *value;
    }
    template <typename Value>
    void key(const document::LayerId layer, const std::string_view name, const std::int64_t frame,
             const Value value) {
        commands::Transaction transaction("Key", document.snapshot().revision());
        transaction.emplace<commands::SetKeyframeAtTimeForParameter>(main, role(layer, name),
                                                                     frameTime(frame), value);
        if (!commands.execute(std::move(transaction)).succeeded())
            throw std::runtime_error("the key was refused");
    }
};

struct Frame final {
    Reference reference;
    std::string label;
};

// A frame of the fixture compiled with `isolation`, through `cache` when given, and its CPU
// reference (bypassing the operation cache: the oracle).
[[nodiscard]] std::optional<Reference>
render(Expectations& expectations, const Fixture& fixture,
       const std::vector<document::LayerId>& isolation, const std::int64_t frame,
       const std::string& label) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::SnapshotCompileRequest request{fixture.document.snapshot(), fixture.main};
    request.isolation = isolation;
    const auto compiled = compiler.compile(request, {});
    expectations.expect(compiled.status == runtime::SnapshotCompileStatus::Compiled &&
                            compiled.plan != nullptr,
                        label + ": compiles");
    if (!compiled.plan)
        return std::nullopt;
    auto evaluation = requestFor(*compiled.plan);
    evaluation.time = frameTime(frame);
    auto oracle = evaluation;
    oracle.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, oracle, {});
    expectations.expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
    if (result.frame() == nullptr)
        return std::nullopt;
    return Reference{compiled.plan, evaluation, result.frame()};
}

[[nodiscard]] render::Rgba32f pixelAt(const Reference& reference, const std::int64_t x,
                                      const std::int64_t y) {
    const auto& image = reference.frame->processImage();
    const auto window = image.descriptor()->dataWindow();
    if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
        y >= window.maxYExclusive())
        return render::Rgba32f::transparent();
    const auto width = static_cast<std::int64_t>(window.extent().width());
    return image
        .pixels()[static_cast<std::size_t>((y - window.originY()) * width + (x - window.originX()))];
}

[[nodiscard]] bool scaled(const render::Rgba32f dimmed, const render::Rgba32f full,
                          const float factor) {
    for (std::size_t c = 0; c < 4; ++c)
        if (std::abs(dimmed.components()[c] - factor * full.components()[c]) > 2e-6F)
            return false;
    return true;
}

[[nodiscard]] bool same(const render::Rgba32f a, const render::Rgba32f b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// The composition-space box a layer's evaluated output covers in a frame.
[[nodiscard]] std::optional<runtime::EvaluatedOperationBounds>
boundsOf(const Reference& reference, const document::LayerId layer) {
    for (const auto& bounds : reference.frame->evaluatedBounds())
        if (bounds.layerId == layer && !bounds.output.empty())
            return bounds;
    return std::nullopt;
}

void structure(Expectations& expectations, const Fixture& fixture) {
    const auto& c = fixture.current();
    const std::vector chain{fixture.root, fixture.outer};
    expectations.expect(document::drillIsolationDimmed(c, chain) ==
                            std::vector{fixture.badge, fixture.backdrop},
                        "inside outer, the badge (top level) and the backdrop (root) are dimmed");
    const std::vector deep{fixture.root, fixture.outer, fixture.middle};
    const auto dimmedDeep = document::drillIsolationDimmed(c, deep);
    expectations.expect(dimmedDeep ==
                            std::vector{fixture.badge, fixture.backdrop, fixture.gradbox},
                        "inside middle, the gradient fill joins them; clipped and its clip, "
                        "inside middle, stay undimmed");
    expectations.expect(std::ranges::find(dimmedDeep, fixture.clip) == dimmedDeep.end(),
                        "a matte source is never dimmed");
    expectations.expect(document::drillIsolationDimmed(c, {}).empty(),
                        "the top level dims nothing");
    const std::vector broken{fixture.outer};
    expectations.expect(document::drillIsolationDimmed(c, broken).empty(),
                        "a chain that does not resolve dims nothing");

    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::SnapshotCompileRequest request{fixture.document.snapshot(), fixture.main};
    request.isolation = deep;
    const auto dimmed = compiler.compile(request, {});
    const auto plain = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    if (!dimmed.plan || !plain.plan) {
        expectations.expect(false, "both plans compile");
        return;
    }
    bool stamped = true;
    bool clean = true;
    for (const auto& operation : dimmed.plan->operations())
        if (const auto* layer = std::get_if<runtime::CompiledLayerOutput>(&operation)) {
            const bool outside = std::ranges::find(dimmedDeep, layer->layerId) != dimmedDeep.end();
            stamped = stamped && layer->isolationOpacity ==
                                     (outside ? runtime::kViewerIsolationOpacity : 1.0);
        }
    for (const auto& operation : plain.plan->operations())
        if (const auto* layer = std::get_if<runtime::CompiledLayerOutput>(&operation))
            clean = clean && layer->isolationOpacity == 1.0;
    expectations.expect(stamped, "the plan dims exactly the layers outside the entered chain");
    expectations.expect(clean, "a plan without isolation (every export) dims nothing");

    // The compiled-plan cache keys on the isolation.
    runtime::CompiledPlanCache cache;
    const auto first = cache.compile(compiler, request, {});
    const auto outside = cache.compile(compiler, {fixture.document.snapshot(), fixture.main}, {});
    const auto again = cache.compile(compiler, request, {});
    expectations.expect(first.plan && outside.plan && first.plan != outside.plan &&
                            again.plan == first.plan && cache.statistics().compiles == 2,
                        "the plan cache keeps the dimmed and the exported plan apart and hits "
                        "each");
}

// The CPU reference: outside layers at 0.3 of their undimmed pixels, the entered group untouched,
// and the operation cache never crossing.
std::vector<Frame> cpuFrames(Expectations& expectations, const Fixture& fixture) {
    std::vector<Frame> frames;
    const std::vector chain{fixture.root, fixture.outer};
    runtime::CpuCompositionEvaluator cached;
    for (const std::int64_t frame : {0, 1, 2}) {
        const auto tag = "frame " + std::to_string(frame);
        const auto full = render(expectations, fixture, {}, frame, tag + " outside");
        const auto dim = render(expectations, fixture, chain, frame, tag + " inside outer");
        if (!full || !dim)
            continue;
        const auto backdrop = boundsOf(*full, fixture.backdrop);
        const auto badge = boundsOf(*full, fixture.badge);
        expectations.expect(backdrop && badge, tag + ": the backdrop and the badge are drawn");
        if (!backdrop || !badge)
            continue;
        const auto centre = [](const runtime::EvaluatedOperationBounds& bounds) {
            return std::pair{static_cast<std::int64_t>((bounds.output.left + bounds.output.right) / 2),
                             static_cast<std::int64_t>((bounds.output.top + bounds.output.bottom) / 2)};
        };
        const auto [bx, by] = centre(*backdrop);
        const auto [gx, gy] = centre(*badge);
        const auto alpha = pixelAt(*full, bx, by).alpha();
        const bool keyed = frame == 0   ? std::abs(alpha - 1.0F) < 1e-6F
                           : frame == 2 ? std::abs(alpha - 0.5F) < 1e-6F
                                        : alpha > 0.5F && alpha < 1.0F;
        expectations.expect(keyed, tag + ": the backdrop's keyed opacity draws undimmed outside "
                                         "(alpha " + std::to_string(alpha) + ")");
        expectations.expect(scaled(pixelAt(*dim, bx, by), pixelAt(*full, bx, by), 0.3F),
                            tag + ": the backdrop draws at 0.3 of its keyed opacity inside");
        expectations.expect(scaled(pixelAt(*dim, gx, gy), pixelAt(*full, gx, gy), 0.3F),
                            tag + ": the moving one-layer badge draws at 0.3 inside");
        // Every pixel off the two dimmed layers is the undimmed pixel, bit for bit: the ring, the
        // triangle, the gradient fill and the clipped square inside the entered group.
        const auto near = [](const runtime::EvaluatedOperationBounds& bounds, const std::int64_t px,
                             const std::int64_t py) {
            const auto x = static_cast<double>(px);
            const auto y = static_cast<double>(py);
            return x >= bounds.output.left - 2 && x <= bounds.output.right + 2 &&
                   y >= bounds.output.top - 2 && y <= bounds.output.bottom + 2;
        };
        bool untouched = true;
        std::size_t inside = 0;
        for (std::int64_t y = 0; y < kHeight; ++y)
            for (std::int64_t x = 0; x < kWidth; ++x) {
                if (near(*backdrop, x, y) || near(*badge, x, y))
                    continue;
                const auto pixel = pixelAt(*full, x, y);
                inside += pixel.alpha() > 0.0F ? 1 : 0;
                untouched = untouched && same(pixelAt(*dim, x, y), pixel);
            }
        expectations.expect(untouched && inside > 1000,
                            tag + ": the entered group's pixels are untouched (" +
                                std::to_string(inside) + " covered pixels compared)");
        // One evaluator, its operation cache shared: outside, inside, outside again.
        const auto viaCache = [&](const Reference& reference) {
            const auto result = cached.evaluate(reference.plan, reference.request, {});
            return result.frame() != nullptr &&
                   parity(std::vector<render::Rgba32f>(result.frame()->processImage().pixels().begin(),
                                                       result.frame()->processImage().pixels().end()),
                          reference.frame->processImage().pixels());
        };
        expectations.expect(viaCache(*full) && viaCache(*dim) && viaCache(*full) && viaCache(*dim),
                            tag + ": the CPU operation cache never serves one to the other");
        frames.push_back({*full, tag + " outside"});
        frames.push_back({*dim, tag + " inside outer"});
    }
    // Deeper: inside middle the gradient fill dims too, still exactly.
    if (const auto deep = render(expectations, fixture,
                                 {fixture.root, fixture.outer, fixture.middle}, 1, "inside middle")) {
        frames.push_back({*deep, "frame 1 inside middle"});
        const auto full = render(expectations, fixture, {}, 1, "frame 1 outside");
        if (full) {
            const auto gradbox = boundsOf(*full, fixture.gradbox);
            if (gradbox) {
                const auto x = static_cast<std::int64_t>((gradbox->output.left + gradbox->output.right) / 2);
                const auto y = static_cast<std::int64_t>((gradbox->output.top + gradbox->output.bottom) / 2);
                expectations.expect(scaled(pixelAt(*deep, x, y), pixelAt(*full, x, y), 0.3F),
                                    "inside middle: the gradient fill outside it draws at 0.3");
            }
        }
    }
    return frames;
}

[[nodiscard]] runtime::CpuGpuSceneBuilder
builderWithTools(runtime::CpuCompositionEvaluator& evaluator,
                 const runtime::GpuSceneOcioContext& ocio) {
    return runtime::CpuGpuSceneBuilder(
        nullptr, runtime::GpuSceneMediaContext::fromEvaluator(evaluator), ocio);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        Fixture fixture(kWidth, kHeight);
        structure(expectations, fixture);
        const auto frames = cpuFrames(expectations, fixture);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " isolation expectation(s) failed\n";
            return 1;
        }

        render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto created = render::GpuDevice::create(createOptions);
        if (!created) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << created.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: CPU half passed; no compatible Vulkan device available: "
                      << created.diagnostic.message << '\n';
            return 77;
        }
        const auto ocio = gpu_coverage_ocio::sharedContext();
        if (ocio == nullptr || ocio->preparer == nullptr) {
            std::cerr << "FAIL: the packaged shader tools are unavailable: "
                      << gpu_coverage_ocio::unavailableReason() << '\n';
            return 1;
        }
        auto& device = *created.device;
        auto cache = runtime::GpuSceneCache::create(device, {}).cache;
        auto executor = cache ? runtime::GpuSceneExecutor::create(device, *cache, {}).executor
                              : nullptr;
        expectations.expect(executor != nullptr, "the executor creates");
        if (!executor)
            return 1;
        runtime::CpuCompositionEvaluator mediaEvaluator;
        const auto builder = builderWithTools(mediaEvaluator, *ocio);
        // Interleaved on one executor and cache: outside, inside, ... over three frames.
        for (const auto& frame : frames)
            gpuParity(expectations, *executor, frame.reference, "GPU " + frame.label, builder);
        // Again in reverse, warm: a cached image of one is never served to the other.
        for (auto it = frames.rbegin(); it != frames.rend(); ++it)
            gpuParity(expectations, *executor, it->reference, "GPU again " + it->label, builder);

        // 1920 x 1080 and 3840 x 2160: the dimmed Viewer frame prepares and runs on the device with
        // no refusal (no full-size CPU compare: the test scope's large-size rule).
        for (const auto [width, height] : {std::pair{1920U, 1080U}, std::pair{3840U, 2160U}}) {
            const auto label = std::to_string(width) + "x" + std::to_string(height);
            Fixture large(width, height);
            const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
            runtime::SnapshotCompileRequest request{large.document.snapshot(), large.main};
            request.isolation = {large.root, large.outer};
            const auto compiled = compiler.compile(request, {});
            expectations.expect(compiled.plan != nullptr, label + ": the dimmed plan compiles");
            if (!compiled.plan)
                continue;
            auto evaluation = requestFor(*compiled.plan);
            evaluation.time = frameTime(1);
            evaluation.pixelStorageByteLimit = std::size_t{1} << 30U;
            const auto built = builder.build(compiled.plan, evaluation);
            expectations.expect(built.hasValue(),
                                label + ": the dimmed frame prepares on the GPU route: " +
                                    built.diagnostic.message);
            if (!built)
                continue;
            const auto before = executor->counters().dispatches;
            const auto begun = executor->begin(built.scene, std::uint64_t{1} << 30U);
            expectations.expect(begun.code == runtime::GpuSceneExecutorDiagnosticCode::None,
                                label + ": the device admits it: " + begun.message);
            if (begun.code != runtime::GpuSceneExecutorDiagnosticCode::None)
                continue;
            auto poll = executor->poll();
            while (poll == runtime::GpuSceneExecutorPollResult::Pending)
                poll = executor->poll();
            expectations.expect(poll == runtime::GpuSceneExecutorPollResult::Ready &&
                                    executor->image() != nullptr &&
                                    executor->counters().dispatches > before,
                                label + ": the dimmed frame ran on the device: " +
                                    executor->diagnostic().message);
            static_cast<void>(executor->takeImage());
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " isolation expectation(s) failed\n";
            return 1;
        }
        std::cout << "GPU: " << frames.size() << " frames compared cold and warm twice, "
                  << executor->counters().dispatches << " dispatches\n";
        std::cout << "PASS: Viewer isolation dims the outside at 0.3 on the CPU reference and the "
                     "GPU route alike, leaves the entered group and every export untouched, and "
                     "renders on the device at 1080p and 4K\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
