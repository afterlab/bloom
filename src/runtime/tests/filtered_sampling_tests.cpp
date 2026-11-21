// #742 and #743 on the CPU reference: the mip levels a filtered resample reads are retained per
// source identity (a warm frame and a turned layer read them again), a filtered layer's window
// reaches as far as its levels do (its edge is the unclipped reference's), a Corner Pin shrinking a
// fine texture 8-16x does not shimmer from frame to frame, and a snapping layer moving 0.3 px a
// frame keeps whole pixels.
#include "affine_mip_levels.hpp"
#include "field_raster.hpp"
#include "field_test_programs.hpp"
#include "image_effect.hpp"
#include "layer_parent_transform.hpp"
#include "transform_2d_geometry.hpp"

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace bloom;
using document::CompositionFormat;
using document::CompositionId;
using document::LayerId;
using document::LayerSlotId;
using document::NodeId;
using document::ParameterId;
using document::Vec2d;
using render::GpuAffineFilter;
using render::ImageWindow;
using render::Rgba32f;
using runtime::CompiledCompositionPlan;
using runtime::CompiledCompositionPlanDefinition;
using runtime::CompiledLayerOutput;
using runtime::CompiledScalarParameter;
using runtime::CompiledVec2Parameter;
using runtime::detail::AffineMipLevels;
using runtime::detail::LayerMatrix;
using runtime::detail::MipLevelCache;
using runtime::detail::ParentedLayerTransform;

int failures = 0;

void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::size_t kBudget = 1ULL << 30U;

render::Rgba32fImage image(const std::uint32_t width, const std::uint32_t height,
                           const std::function<std::array<float, 4>(std::int64_t, std::int64_t)>&
                               pixel,
                           const std::uint32_t displayWidth = 0,
                           const std::uint32_t displayHeight = 0) {
    const auto data = ImageWindow::create(0, 0, width, height);
    const auto display = ImageWindow::create(0, 0, displayWidth == 0 ? width : displayWidth,
                                             displayHeight == 0 ? height : displayHeight);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
    for (std::int64_t y = 0; y < height; ++y)
        for (std::int64_t x = 0; x < width; ++x) {
            const auto p = pixel(x, y);
            const auto value = Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
            if (!value || builder.value()->write(x, y, *value.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

// A one-pixel black and white checker: the finest texture there is, the one a shrinking read
// aliases most.
render::Rgba32fImage checker(const std::uint32_t width, const std::uint32_t height) {
    return image(width, height, [](const std::int64_t x, const std::int64_t y) {
        const float v = ((x + y) % 2 == 0) ? 1.0F : 0.0F;
        return std::array{v, v, v, 1.0F};
    });
}

// The output rows of `transform` over `lattice`, as the evaluator resamples a layer.
std::vector<Rgba32f> resample(const ParentedLayerTransform& transform,
                              const render::Rgba32fImage& source, const ImageWindow lattice,
                              const ImageWindow output, const AffineMipLevels* levels) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(output.extent().width()) *
                                    output.extent().height(),
                                Rgba32f::transparent());
    const auto sourceView = source.view();
    for (std::int64_t y = output.originY(); y < output.maxYExclusive(); ++y) {
        const std::span row(pixels.data() + static_cast<std::size_t>(y - output.originY()) *
                                                output.extent().width(),
                            output.extent().width());
        if (transform.row(*sourceView.value(), lattice, output, y, row, levels))
            throw std::logic_error("resample row");
    }
    return pixels;
}

bool same(const render::Rgba32fImage& a, const render::Rgba32fImage& b) {
    return *a.descriptor() == *b.descriptor() && std::ranges::equal(a.pixels(), b.pixels());
}

// #742 gap: the CPU reference keeps its mip levels per source identity. A warm build reads every
// level from the operation cache, a changed rotation over the same source reads the levels both
// plans share and builds only deeper ones, and another source identity reads none.
void testRetainedLevels() {
    runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    runtime::OperationCache cache(1U << 30U, ledger);
    const auto source = checker(96, 72);
    const auto window = source.descriptor()->dataWindow();
    const auto planOf = [&](const double rotation, const double scale) {
        return ParentedLayerTransform(
                   LayerMatrix::authored({200.0, 150.0}, {48.0, 36.0}, {scale, scale}, rotation),
                   window, 1.0, 1.0, 1.0F, GpuAffineFilter::Trilinear)
            .filterPlan();
    };
    const MipLevelCache retained{&cache, "checker-96x72", document::Revision::fromRaw(3), nullptr};
    const auto sourceView = source.view();
    const auto view = *sourceView.value();
    const auto cold = planOf(30.0, 0.15);
    AffineMipLevels first;
    expect(AffineMipLevels::build(view, cold, kBudget, {}, first, &retained) ==
                   AffineMipLevels::Outcome::Built &&
               first.count() == cold.upperLevel && first.count() >= 2 && first.retainedLevels() == 0,
           "retained levels: a cold build builds every level the plan reads");
    AffineMipLevels warm;
    expect(AffineMipLevels::build(view, cold, kBudget, {}, warm, &retained) ==
                   AffineMipLevels::Outcome::Built &&
               warm.count() == first.count() && warm.retainedLevels() == first.count(),
           "retained levels: a warm build reads every level from the cache");
    bool identical = true;
    for (std::uint32_t n = 1; n <= warm.count(); ++n)
        identical = identical && same(*warm.image(n), *first.image(n));
    expect(identical, "retained levels: the retained levels are the built ones");
    // A turned and shrunk-further layer over the same picture: the shared levels are read, the
    // deeper one is built.
    const auto turned = planOf(35.0, 0.07);
    AffineMipLevels moved;
    expect(AffineMipLevels::build(view, turned, kBudget, {}, moved, &retained) ==
                   AffineMipLevels::Outcome::Built &&
               turned.upperLevel > cold.upperLevel && moved.count() == turned.upperLevel &&
               moved.retainedLevels() == cold.upperLevel,
           "retained levels: a changed rotation reads the shared levels and builds the deeper one");
    // Built without the cache, the turned plan's levels are the same images.
    AffineMipLevels fresh;
    expect(AffineMipLevels::build(view, turned, kBudget, {}, fresh) ==
                   AffineMipLevels::Outcome::Built &&
               fresh.retainedLevels() == 0 && fresh.count() == moved.count(),
           "retained levels: a build without the cache builds every level");
    identical = true;
    for (std::uint32_t n = 1; n <= fresh.count(); ++n)
        identical = identical && same(*fresh.image(n), *moved.image(n));
    expect(identical, "retained levels: retained and freshly built levels are identical");
    const MipLevelCache other{&cache, "another-source", document::Revision::fromRaw(3), nullptr};
    AffineMipLevels unrelated;
    expect(AffineMipLevels::build(view, cold, kBudget, {}, unrelated, &other) ==
                   AffineMipLevels::Outcome::Built &&
               unrelated.retainedLevels() == 0,
           "retained levels: another source identity reads none");
    // A retained level is still charged to the frame: a budget below the levels refuses.
    AffineMipLevels refused;
    expect(AffineMipLevels::build(view, cold, first.bytes() - 1, {}, refused, &retained) ==
               AffineMipLevels::Outcome::OverBudget,
           "retained levels: retained levels are charged to the frame's budget");
}

// The layer plan the evaluator tests below share: a checker resampled by one layer.
std::shared_ptr<const CompiledCompositionPlan>
layerPlan(const std::uint64_t id, const std::uint32_t width, const std::uint32_t height,
          const std::uint32_t sourceWidth, const std::uint32_t sourceHeight, const Vec2d position,
          const Vec2d scale, const double rotation, const std::int64_t filter, const bool snap,
          const std::uint32_t cell = 1, const std::optional<Vec2d> parentPosition = {}) {
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledChecker{
        NodeId::fromRaw(id + 1),
        {ParameterId::fromRaw(id + 2), core::Color4d{1.0, 1.0, 1.0, 1.0}},
        {ParameterId::fromRaw(id + 3), core::Color4d{0.0, 0.25, 0.5, 1.0}},
        cell,
        cell,
        sourceWidth,
        sourceHeight});
    std::vector<runtime::CompiledMergeInput> entries;
    const auto addLayer = [&](const std::uint64_t at, const LayerId layerId, const Vec2d where,
                              const Vec2d size, const double turn,
                              const std::optional<runtime::OperationIndex> parent) {
        CompiledLayerOutput layer{NodeId::fromRaw(at + 6),
                                  layerId,
                                  runtime::OperationIndex::fromRaw(0),
                                  CompiledVec2Parameter{ParameterId::fromRaw(at + 7), where},
                                  CompiledVec2Parameter{ParameterId::fromRaw(at + 8), Vec2d{}},
                                  CompiledVec2Parameter{ParameterId::fromRaw(at + 9), size},
                                  CompiledScalarParameter{ParameterId::fromRaw(at + 10), turn},
                                  CompiledScalarParameter{ParameterId::fromRaw(at + 11), 1.0},
                                  ParameterId::fromRaw(at + 12),
                                  core::kDefaultBlendMode};
        layer.filter = filter;
        layer.snap = snap;
        layer.parent = parent;
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(std::move(layer));
        return index;
    };
    std::optional<runtime::OperationIndex> parent;
    if (parentPosition) {
        // A parent that moves the child: it is in the stack, entirely transparent (opacity 0).
        parent = addLayer(id + 40, LayerId::fromRaw(2), *parentPosition, Vec2d{1.0, 1.0}, 0.0,
                          std::nullopt);
        std::get<CompiledLayerOutput>(operations.back()).opacity.source = 0.0;
        entries.push_back({LayerSlotId::fromRaw(id + 15), LayerId::fromRaw(2), *parent});
    }
    const auto layerIndex = addLayer(id, LayerId::fromRaw(1), position, scale, rotation, parent);
    entries.push_back({LayerSlotId::fromRaw(id + 14), LayerId::fromRaw(1), layerIndex});
    operations.emplace_back(runtime::CompiledMerge{NodeId::fromRaw(id + 13), std::move(entries)});
    const auto merge = runtime::OperationIndex::fromRaw(operations.size() - 1);
    operations.emplace_back(runtime::CompiledCompositionOutput{NodeId::fromRaw(id + 16), merge});
    const auto output = runtime::OperationIndex::fromRaw(operations.size() - 1);
    const auto format = CompositionFormat::create(width, height);
    CompiledCompositionPlanDefinition definition{document::Revision::fromRaw(7),
                                                 document::ProjectId::fromRaw(1),
                                                 CompositionId::fromRaw(id + 100),
                                                 *format,
                                                 std::move(operations),
                                                 output};
    definition.duration = core::RationalTime::fromInteger(100);
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

runtime::EvaluationRequest requestFor(const CompiledCompositionPlan& plan) {
    return runtime::EvaluationRequest{
        .time = core::RationalTime{},
        .output = plan.output(),
        .resolution = runtime::CompositionFormatResolution{},
        .quality = runtime::EvaluationQuality::Reference,
        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = kBudget};
}

// The evaluator retains them too: after a cold frame, the same layer turned further reads the
// picture's levels from the operation cache (more hits than the same change with Bilinear, which
// has no levels), and its pixels are those of an evaluation that bypasses the cache.
void testEvaluatorRetainsLevels() {
    const auto hitsOfChange = [](const std::int64_t filter, const std::uint64_t id) {
        const runtime::CpuCompositionEvaluator evaluator;
        const auto cold = layerPlan(id, 256, 192, 96, 72, {128.0, 96.0}, {0.15, 0.15}, 30.0,
                                    filter, false);
        const auto first = evaluator.evaluate(cold, requestFor(*cold), {});
        const auto before = evaluator.operationCache()->statistics();
        const auto warm = evaluator.evaluate(cold, requestFor(*cold), {});
        const auto afterWarm = evaluator.operationCache()->statistics();
        const auto turned = layerPlan(id, 256, 192, 96, 72, {128.0, 96.0}, {0.15, 0.15}, 35.0,
                                      filter, false);
        const auto moved = evaluator.evaluate(turned, requestFor(*turned), {});
        const auto after = evaluator.operationCache()->statistics();
        auto uncachedRequest = requestFor(*turned);
        uncachedRequest.bypassOperationCache = true;
        const auto reference = evaluator.evaluate(turned, uncachedRequest, {});
        expect(first.frame() && warm.frame() && moved.frame() && reference.frame() &&
                   std::ranges::equal(moved.frame()->processImage().pixels(),
                                      reference.frame()->processImage().pixels()) &&
                   std::ranges::equal(warm.frame()->processImage().pixels(),
                                      first.frame()->processImage().pixels()),
               "evaluator levels: warm and turned frames match an uncached evaluation");
        expect(afterWarm.misses == before.misses,
               "evaluator levels: a warm frame misses nothing");
        return after.hits - afterWarm.hits;
    };
    const auto bilinear = hitsOfChange(runtime::kResampleFilterBilinear, 9000);
    const auto trilinear = hitsOfChange(runtime::kResampleFilterTrilinear, 9000);
    std::cout << "evaluator levels: a turned layer hits " << bilinear << " entries Bilinear, "
              << trilinear << " Trilinear\n";
    expect(trilinear > bilinear,
           "evaluator levels: a turned Trilinear layer reads its retained levels");
}

// #742 gap: a filtered layer's window grows by its levels' reach. Rows over a lattice far larger
// than the window (the unclipped evaluation) are transparent everywhere outside the window, and
// inside it they are the evaluator's rows over the window itself; the bilinear box alone would
// have clipped the filtered fringe.
void testFilteredEdgeIsUnclipped() {
    // An odd size, so the levels' extents round up past the picture's edge.
    const auto source = image(
        250, 190, [](std::int64_t, std::int64_t) { return std::array{0.75F, 0.5F, 0.25F, 1.0F}; });
    const auto data = source.descriptor()->dataWindow();
    const auto big = ImageWindow::create(-4096, -4096, 8192, 8192);
    for (const auto filter : {GpuAffineFilter::Trilinear, GpuAffineFilter::Anisotropic}) {
        const auto name = std::string(filter == GpuAffineFilter::Trilinear ? "Trilinear"
                                                                           : "Anisotropic");
        std::size_t outsideWindow = 0;
        std::size_t outsideBox = 0;
        std::size_t differ = 0;
        std::size_t placements = 0;
        // Shrinks just over and under a power of two, upright and turned, at eight sub-pixel
        // placements: where the picture's edge falls against the pixel grid decides whether the
        // deepest level's reach leaves the bilinear box.
        for (const double shrink : {17.0, 24.0, 31.0})
            for (const double rotation : {0.0, 30.0})
                for (int step = 0; step < 8; ++step) {
                    const auto matrix = LayerMatrix::authored(
                        {120.0 + 0.125 * step, 90.0 + 0.0625 * step}, {125.0, 95.0},
                        {1.0 / shrink,
                         filter == GpuAffineFilter::Trilinear ? 1.0 / shrink : 4.0 / shrink},
                        rotation);
                    const ParentedLayerTransform transform(matrix, data, 1.0, 1.0, 1.0F, filter);
                    const ParentedLayerTransform bilinear(matrix, data, 1.0, 1.0, 1.0F,
                                                          GpuAffineFilter::Bilinear);
                    AffineMipLevels levels;
                    const auto sourceView = source.view();
                    const bool built = transform.filterPlan().filtered() &&
                                       AffineMipLevels::build(*sourceView.value(),
                                                              transform.filterPlan(), kBudget, {},
                                                              levels) ==
                                           AffineMipLevels::Outcome::Built;
                    const auto window = transform.supportBounds(*big.value());
                    const auto box = bilinear.supportBounds(*big.value());
                    if (!built || !window || !box) {
                        expect(false, name + " edge: the filtered layer has levels and a window");
                        continue;
                    }
                    ++placements;
                    const auto lattice = ImageWindow::create(
                        window->originX() - 32, window->originY() - 32,
                        window->extent().width() + 64, window->extent().height() + 64);
                    const auto unclipped =
                        resample(transform, source, *lattice.value(), *lattice.value(), &levels);
                    const auto clipped = resample(transform, source, *window, *window, &levels);
                    const auto width = lattice.value()->extent().width();
                    for (std::size_t i = 0; i < unclipped.size(); ++i) {
                        const auto x =
                            lattice.value()->originX() + static_cast<std::int64_t>(i % width);
                        const auto y =
                            lattice.value()->originY() + static_cast<std::int64_t>(i / width);
                        const bool visible = unclipped[i] != Rgba32f::transparent();
                        const auto inside = [x, y](const ImageWindow& w) {
                            return x >= w.originX() && x < w.maxXExclusive() &&
                                   y >= w.originY() && y < w.maxYExclusive();
                        };
                        if (visible && !inside(*window))
                            ++outsideWindow;
                        if (visible && !inside(*box))
                            ++outsideBox;
                        if (inside(*window)) {
                            const auto at = static_cast<std::size_t>(y - window->originY()) *
                                                window->extent().width() +
                                            static_cast<std::size_t>(x - window->originX());
                            if (clipped[at] != unclipped[i])
                                ++differ;
                        }
                    }
                }
        std::cout << name << " edge, " << placements << " placements: " << outsideBox
                  << " visible pixels lie outside the bilinear box, " << outsideWindow
                  << " outside the filtered window\n";
        expect(outsideWindow == 0,
               name + " edge: nothing the unclipped evaluation draws lies outside the window");
        expect(outsideBox > 0, name + " edge: the bilinear box alone would clip the fringe");
        expect(differ == 0, name + " edge: inside the window the rows are the unclipped ones");
    }
    // Transform 2D sizes its window the same way.
    runtime::detail::PreparedTransform2D prepared{
        runtime::Transform2DExtent::Transformed, runtime::kResampleFilterTrilinear,
        runtime::detail::Transform2DValues{.translate = {3.0, -2.0},
                                           .scale = {1.0 / 31.0, 1.0 / 31.0},
                                           .rotation = 30.0,
                                           .centre = {125.0, 95.0}}};
    const auto geometry =
        runtime::detail::resolveTransform2DGeometry(prepared, *source.descriptor(), 1.0, 1.0);
    expect(geometry && geometry->window, "Transform 2D edge: the geometry resolves");
    if (geometry && geometry->window) {
        const ParentedLayerTransform transform(geometry->matrix, data, 1.0, 1.0, 1.0F,
                                               GpuAffineFilter::Trilinear);
        AffineMipLevels levels;
        const auto sourceView = source.view();
        static_cast<void>(AffineMipLevels::build(*sourceView.value(), transform.filterPlan(),
                                                 kBudget, {}, levels));
        const auto& window = *geometry->window;
        const auto lattice =
            ImageWindow::create(window.originX() - 32, window.originY() - 32,
                                window.extent().width() + 64, window.extent().height() + 64);
        const auto unclipped = resample(transform, source, *lattice.value(), *lattice.value(), &levels);
        std::size_t outside = 0;
        const auto width = lattice.value()->extent().width();
        for (std::size_t i = 0; i < unclipped.size(); ++i) {
            const auto x = lattice.value()->originX() + static_cast<std::int64_t>(i % width);
            const auto y = lattice.value()->originY() + static_cast<std::int64_t>(i / width);
            if (unclipped[i] != Rgba32f::transparent() &&
                (x < window.originX() || x >= window.maxXExclusive() || y < window.originY() ||
                 y >= window.maxYExclusive()))
                ++outside;
        }
        expect(outside == 0, "Transform 2D edge: its window holds the whole filtered fringe");
    }
}

// The operands of a field program as the evaluator samples them (constants, then the time).
std::vector<double> operands(const runtime::FieldProgram& program) {
    std::vector<double> values;
    runtime::forEachFieldOperand(program, [&](const auto& operand) {
        using Operand = std::decay_t<decltype(operand)>;
        if constexpr (std::is_same_v<Operand, runtime::CompiledVec2Parameter>) {
            const auto& v = std::get<Vec2d>(operand.source);
            values.push_back(v.x);
            values.push_back(v.y);
        } else if constexpr (std::is_same_v<Operand, runtime::CompiledScalarParameter>) {
            values.push_back(std::get<double>(operand.source));
        }
    });
    values.push_back(0.0);
    return values;
}

// #742 evidence: a Corner Pin shrinking a 512 px one-pixel checker 8x, 12x and 16x, moved 0.3 px a
// frame for four frames. Bilinear point-samples the checker, so a pixel flips between black and
// white as the quad moves (shimmer); Trilinear and Anisotropic read the levels, which average the
// checker, so a pixel barely changes. Measured as each interior pixel's range over the frames.
void testCornerPinShrinkDoesNotShimmer() {
    constexpr std::uint32_t kSize = 512;
    const auto source = checker(kSize, kSize);
    const auto canvas = image(kSize, kSize, [](std::int64_t, std::int64_t) {
        return std::array{0.0F, 0.0F, 0.0F, 0.0F};
    });
    const std::array<const render::Rgba32fImage*, 1> slots{&source};
    for (const double shrink : {8.0, 12.0, 16.0}) {
        std::array<double, 3> worst{};
        std::array<double, 3> mean{};
        const std::array<std::int64_t, 3> filters{0, 3, 4};
        for (std::size_t f = 0; f < filters.size(); ++f) {
            const double inset = (kSize - kSize / shrink) / 2.0;
            std::vector<std::vector<float>> frames;
            std::vector<std::pair<std::int64_t, std::int64_t>> interior;
            for (int frame = 0; frame < 4; ++frame) {
                // 0.3 px a frame along a diagonal, from a start off the texel lattice.
                const double shift = 0.3 * frame;
                const double dx = 0.37 + shift * 0.8;
                const double dy = 0.21 + shift * 0.6;
                const std::array<Vec2d, 4> corners{
                    Vec2d{inset + dx, inset + dy}, Vec2d{-inset + dx, inset + dy},
                    Vec2d{-inset + dx, -inset + dy}, Vec2d{inset + dx, -inset + dy}};
                const auto program =
                    field_test_programs::cornerPinProgram(20000, filters[f], corners);
                const auto grade = runtime::detail::prepareFieldRasterGrade(
                    runtime::FieldRasterKernel{program, 1}, operands(program));
                if (!grade) {
                    expect(false, "shimmer: the Corner Pin program prepares");
                    return;
                }
                const auto result = runtime::detail::applyFieldRaster(
                    *grade, canvas, slots, kSize, kSize, 1.0, 1.0, kBudget, {},
                    runtime::OperationIndex::fromRaw(1), {}, nullptr);
                if (!result.image) {
                    expect(false, "shimmer: the Corner Pin rasters");
                    return;
                }
                std::vector<float> red;
                for (const auto& pixel : result.image->pixels())
                    red.push_back(pixel.red());
                frames.push_back(std::move(red));
            }
            // Pixels well inside every frame's quad (two pixels from its edges).
            const double low = inset + 3.0;
            const double high = kSize - inset - 2.0;
            for (std::int64_t y = static_cast<std::int64_t>(std::ceil(low));
                 y < static_cast<std::int64_t>(std::floor(high)); ++y)
                for (std::int64_t x = static_cast<std::int64_t>(std::ceil(low));
                     x < static_cast<std::int64_t>(std::floor(high)); ++x)
                    interior.emplace_back(x, y);
            double sum = 0.0;
            for (const auto& [x, y] : interior) {
                const auto at = static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x);
                float lowest = frames[0][at];
                float highest = frames[0][at];
                for (const auto& frame : frames) {
                    lowest = std::min(lowest, frame[at]);
                    highest = std::max(highest, frame[at]);
                }
                const double range = static_cast<double>(highest - lowest);
                worst[f] = std::max(worst[f], range);
                sum += range;
            }
            mean[f] = interior.empty() ? 0.0 : sum / static_cast<double>(interior.size());
            expect(interior.size() >= 9, "shimmer: the quad has an interior");
        }
        std::cout << "corner pin shrink " << shrink << "x, a pixel's range over 4 frames "
                  << "(max / mean): Bilinear " << worst[0] << " / " << mean[0] << ", Trilinear "
                  << worst[1] << " / " << mean[1] << ", Anisotropic " << worst[2] << " / "
                  << mean[2] << '\n';
        expect(worst[0] > 0.15, "shimmer: Bilinear flickers on the shrunk checker (control)");
        expect(worst[1] < 0.02 && worst[2] < 0.02,
               "shimmer: Trilinear and Anisotropic hold every pixel within 0.02 over the frames");
    }
}

// #743 evidence: a 2 px sprite (a 2 x 2 one-pixel checker) moving 0.3 px a frame. With snap its
// position rounds to whole pixels: frames between whole-pixel steps are identical, each step moves
// the image exactly one pixel, and no pixel is a blend of the sprite and the background. Without
// snap (the control) the fractional frames blend. A child of a moving parent snaps after
// parenting: its final position is whole even when its parent's is not.
void testSnapKeepsWholePixels() {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto run = [&](const bool snap, const bool parented, const int frame) {
        const double x = 20.0 + 0.3 * frame;
        const auto plan = parented ? layerPlan(9500, 64, 32, 2, 2, {6.0, 0.0}, {1.0, 1.0}, 0.0,
                                               runtime::kResampleFilterBilinear, snap, 1,
                                               Vec2d{x - 6.0, 16.0})
                                   : layerPlan(9500, 64, 32, 2, 2, {x, 16.0}, {1.0, 1.0}, 0.0,
                                               runtime::kResampleFilterBilinear, snap);
        auto request = requestFor(*plan);
        request.bypassOperationCache = true;
        const auto result = evaluator.evaluate(plan, request, {});
        if (!result.frame())
            throw std::logic_error("snap frame evaluates");
        return std::vector<Rgba32f>(result.frame()->processImage().pixels().begin(),
                                    result.frame()->processImage().pixels().end());
    };
    const auto blended = [](const std::vector<Rgba32f>& pixels) {
        return std::ranges::any_of(pixels, [](const Rgba32f& p) {
            const bool white = p.red() == 1.0F && p.green() == 1.0F && p.blue() == 1.0F;
            const bool blue = p.red() == 0.0F && p.green() == 0.25F && p.blue() == 0.5F;
            return p != Rgba32f::transparent() && !(p.alpha() == 1.0F && (white || blue));
        });
    };
    // The image moved right by `n` whole pixels in a 64-wide frame.
    const auto shifted = [](const std::vector<Rgba32f>& pixels, const int n) {
        std::vector<Rgba32f> out(pixels.size(), Rgba32f::transparent());
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const auto x = static_cast<int>(i % 64) + n;
            if (x >= 0 && x < 64)
                out[i - (i % 64) + static_cast<std::size_t>(x)] = pixels[i];
        }
        return out;
    };
    for (const bool parented : {false, true}) {
        const std::string name = parented ? "snap (parented): " : "snap: ";
        std::vector<std::vector<Rgba32f>> frames;
        for (int frame = 0; frame < 10; ++frame)
            frames.push_back(run(true, parented, frame));
        std::size_t runs = 1;
        bool steps = true;
        bool whole = true;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            whole = whole && !blended(frames[i]);
            if (i > 0 && frames[i] != frames[i - 1]) {
                ++runs;
                steps = steps && frames[i] == shifted(frames[i - 1], 1);
            }
        }
        std::cout << name << "10 frames at 0.3 px a frame hold " << runs << " whole positions\n";
        expect(whole, name + "no pixel blends the sprite with the background");
        expect(steps, name + "each change is the image moved exactly one whole pixel");
        expect(runs == 3 || runs == 4,
               name + "frames between whole-pixel steps are identical (3 or 4 positions over "
                      "2.7 px)");
        bool control = false;
        for (int frame = 0; frame < 10; ++frame)
            control = control || blended(run(false, parented, frame));
        expect(control, name + "without snap the fractional frames blend (control)");
    }
}

} // namespace

int main() {
    try {
        testRetainedLevels();
        testEvaluatorRetainsLevels();
        testFilteredEdgeIsUnclipped();
        testCornerPinShrinkDoesNotShimmer();
        testSnapKeepsWholePixels();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
