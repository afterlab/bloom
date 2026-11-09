// #484 Motion blur: the pass planner. Turns a motion kernel, its prepared operands and the input
// windows into the backend-neutral pass plan both the CPU interpreter and the GPU emission run
// (motion_blur_image_effect.hpp). Every table is laid out exactly as motion_kernel.glsl documents
// its pass. docs/architecture/motion-blur.md is the specification.

#include "motion_blur_image_effect.hpp"

#include "transform_2d_geometry.hpp"

#include <bloom/document/motion_blur_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bloom::runtime::detail {
namespace {

constexpr int kPassAccumulate = 1;
constexpr int kPassPath = 2;
constexpr int kPassTile = 3;
constexpr int kPassGather = 4;
constexpr int kPassSplat = 5;
constexpr int kPassJfaInit = 6;
constexpr int kPassJfaStep = 7;
constexpr int kPassFill = 8;
constexpr int kPassConsistency = 9;
constexpr int kPassGrey = 10;
constexpr int kPassDecimate = 11;
constexpr int kPassTvInit = 12;
constexpr int kPassTvWarp = 13;
constexpr int kPassTvIterate = 14;
constexpr int kPassFlowOut = 15;
constexpr int kPassDisSearch = 16;
constexpr int kPassDisDensify = 17;
constexpr int kPassDisData = 18;
constexpr int kPassDisIterate = 19;
constexpr int kPassCutResidual = 20;
constexpr int kPassCutReduce = 21;
constexpr int kPassCutBroadcast = 22;
constexpr int kPassTemporalFill = 23;
constexpr int kPassDisInit = 24;
constexpr int kPassDisUpdate = 25;
constexpr int kPassNeighbour = 26;
constexpr std::int64_t kVectorTileSize = 32;
constexpr std::size_t kParams = 16;
constexpr std::size_t kWeights = kParams + 32;
// Coordinates the binary32 kernel addresses exactly.
constexpr std::int64_t kMaxCoordinate = 1 << 24;

[[nodiscard]] MotionRect unite(const MotionRect a, const MotionRect b) noexcept {
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    const auto x0 = std::min(a.x, b.x);
    const auto y0 = std::min(a.y, b.y);
    const auto x1 = std::max(a.x + a.width, b.x + b.width);
    const auto y1 = std::max(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

[[nodiscard]] MotionRect intersect(const MotionRect a, const MotionRect b) noexcept {
    const auto x0 = std::max(a.x, b.x);
    const auto y0 = std::max(a.y, b.y);
    const auto x1 = std::min(a.x + a.width, b.x + b.width);
    const auto y1 = std::min(a.y + a.height, b.y + b.height);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

[[nodiscard]] MotionRect grow(const MotionRect a, const std::int64_t by) noexcept {
    return {a.x - by, a.y - by, a.width + 2 * by, a.height + 2 * by};
}

[[nodiscard]] bool representable(const MotionRect r) noexcept {
    return r.width > 0 && r.height > 0 && std::abs(r.x) < kMaxCoordinate &&
           std::abs(r.y) < kMaxCoordinate && r.width < kMaxCoordinate &&
           r.height < kMaxCoordinate && std::abs(r.x + r.width) < kMaxCoordinate &&
           std::abs(r.y + r.height) < kMaxCoordinate;
}

// One pass table under construction: the header, regions and parameters as vec4s.
class Table final {
  public:
    explicit Table(const int pass) : data_(4 * (kParams + 4), 0.0F) {
        data_[0] = static_cast<float>(pass);
    }
    void active(const MotionRect r) {
        data_[1] = static_cast<float>(r.x);
        data_[2] = static_cast<float>(r.y);
        data_[3] = static_cast<float>(r.width);
        data_[4] = static_cast<float>(r.height);
    }
    void region(const std::size_t index, const MotionRect r) {
        set(2 + index, static_cast<float>(r.x), static_cast<float>(r.y),
            static_cast<float>(r.width), static_cast<float>(r.height));
    }
    void param(const std::size_t index, const double a, const double b = 0.0, const double c = 0.0,
               const double d = 0.0) {
        set(kParams + index, static_cast<float>(a), static_cast<float>(b), static_cast<float>(c),
            static_cast<float>(d));
    }
    void vec(const std::size_t index, const float a, const float b, const float c, const float d) {
        set(index, a, b, c, d);
    }
    void weights(const std::vector<double>& w) {
        for (std::size_t i = 0; i < w.size(); ++i) {
            const auto at = 4 * kWeights + i;
            if (data_.size() < at + 1)
                data_.resize((at / 4 + 1) * 4, 0.0F);
            data_[at] = static_cast<float>(w[i]);
        }
    }
    [[nodiscard]] std::vector<float> take() {
        while (data_.size() < 4 * 20)
            data_.push_back(0.0F);
        return std::move(data_);
    }

  private:
    void set(const std::size_t vec4, const float a, const float b, const float c, const float d) {
        if (data_.size() < 4 * (vec4 + 1))
            data_.resize(4 * (vec4 + 1), 0.0F);
        data_[4 * vec4] = a;
        data_[4 * vec4 + 1] = b;
        data_[4 * vec4 + 2] = c;
        data_[4 * vec4 + 3] = d;
    }
    std::vector<float> data_;
};

// The plan under construction.
class Builder final {
  public:
    explicit Builder(const std::span<const MotionRect> inputs) {
        plan_.inputs = static_cast<std::uint32_t>(inputs.size());
        plan_.buffers = plan_.inputs;
        plan_.windows.assign(inputs.begin(), inputs.end());
    }
    [[nodiscard]] const MotionRect& window(const std::uint32_t buffer) const {
        return plan_.windows[buffer];
    }
    // A zero image over `window` with each (buffer, shift) moved into it.
    std::uint32_t place(const MotionRect window, std::vector<MotionPlacement> sources) {
        std::erase_if(sources, [&](const MotionPlacement& p) {
            return intersect(window, {plan_.windows[p.buffer].x + p.shiftX,
                                      plan_.windows[p.buffer].y + p.shiftY,
                                      plan_.windows[p.buffer].width,
                                      plan_.windows[p.buffer].height})
                .empty();
        });
        if (!representable(window) || sources.empty())
            valid_ = false;
        const auto target = add(window);
        plan_.steps.push_back({MotionStepKind::Place, target, window, std::move(sources), 0, {}});
        return target;
    }
    std::uint32_t run(const std::uint32_t source, std::vector<float> table) {
        if (table.size() > kMotionTableFloats || !validMotionProgramData(table))
            valid_ = false;
        const auto target = add(plan_.windows[source]);
        plan_.steps.push_back(
            {MotionStepKind::Run, target, plan_.windows[source], {}, source, std::move(table)});
        return target;
    }
    // `source`'s pixels in `rect` (source-window coordinates) moved to `to`.
    std::uint32_t cut(const std::uint32_t source, const MotionRect rect, const std::int64_t toX,
                      const std::int64_t toY) {
        return place({toX, toY, rect.width, rect.height},
                     {{source, toX - rect.x, toY - rect.y}});
    }
    [[nodiscard]] std::optional<MotionPassPlan> finish(const std::optional<std::uint32_t> output) {
        if (!valid_)
            return std::nullopt;
        plan_.output = output;
        return std::move(plan_);
    }
    void invalidate() noexcept { valid_ = false; }

  private:
    std::uint32_t add(const MotionRect window) {
        plan_.windows.push_back(window);
        return plan_.buffers++;
    }
    MotionPassPlan plan_;
    bool valid_ = true;
};

// A shelf layout of regions: each size placed left to right in rows no wider than about the
// square root of the total area (or the widest region). Positions are atlas-local; the atlas
// window starts at (0, 0).
struct Atlas final {
    MotionRect window{};
    std::vector<MotionRect> regions;
};
[[nodiscard]] Atlas layout(const std::vector<std::array<std::int64_t, 2>>& sizes) {
    Atlas atlas;
    double area = 0.0;
    std::int64_t widest = 1;
    for (const auto& s : sizes) {
        area += static_cast<double>(std::max<std::int64_t>(s[0], 0)) *
                static_cast<double>(std::max<std::int64_t>(s[1], 0));
        widest = std::max(widest, s[0]);
    }
    const auto limit = std::max<std::int64_t>(widest, static_cast<std::int64_t>(std::sqrt(area)));
    std::int64_t x = 0, y = 0, rowHeight = 0, width = 0;
    for (const auto& s : sizes) {
        if (s[0] <= 0 || s[1] <= 0) {
            atlas.regions.push_back({});
            continue;
        }
        if (x > 0 && x + s[0] > limit) {
            y += rowHeight;
            x = 0;
            rowHeight = 0;
        }
        atlas.regions.push_back({x, y, s[0], s[1]});
        x += s[0];
        rowHeight = std::max(rowHeight, s[1]);
        width = std::max(width, x);
    }
    atlas.window = {0, 0, std::max<std::int64_t>(width, 1), std::max<std::int64_t>(y + rowHeight, 1)};
    return atlas;
}

// An atlas holding `images` (buffer, nullopt: an empty region of the given size) as regions in
// order: placed, its regions returned.
struct Placed final {
    std::uint32_t buffer = 0;
    Atlas atlas;
};
struct Slot final {
    std::optional<std::uint32_t> buffer;
    std::array<std::int64_t, 2> size{};
};
[[nodiscard]] Placed placeAtlas(Builder& builder, const std::vector<Slot>& slots) {
    std::vector<std::array<std::int64_t, 2>> sizes;
    for (const auto& slot : slots)
        sizes.push_back(slot.buffer ? std::array{builder.window(*slot.buffer).width,
                                                 builder.window(*slot.buffer).height}
                                    : slot.size);
    Placed placed{0, layout(sizes)};
    std::vector<MotionPlacement> sources;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].buffer)
            continue;
        const auto& w = builder.window(*slots[i].buffer);
        const auto& r = placed.atlas.regions[i];
        sources.push_back({*slots[i].buffer, r.x - w.x, r.y - w.y});
    }
    if (sources.empty()) {
        builder.invalidate();
        return placed;
    }
    placed.buffer = builder.place(placed.atlas.window, std::move(sources));
    return placed;
}

[[nodiscard]] std::array<std::int64_t, 2> sizeOf(const MotionRect r) noexcept {
    return {r.width, r.height};
}

// --- Accumulate -----------------------------------------------------------------------------------

std::optional<MotionPassPlan> planAccumulate(const TemporalAccumulateKernel& kernel,
                                             const std::span<const MotionRect> inputs) {
    Builder builder(inputs);
    const auto u = unite(inputs[0], inputs[1]);
    if (u.empty())
        return builder.finish(std::nullopt);
    // Both images over the union, side by side, the accumulator's region active.
    // The new sample is the atlas's base and the accumulator blends onto it: the GPU planner visits
    // a blend's source first, so each step resolves the whole chain before rendering its sample,
    // and a frame holds an accumulator and one sample instead of every sample of the shutter.
    std::vector<MotionPlacement> sources;
    if (!inputs[1].empty())
        sources.push_back({1, u.width - u.x, -u.y});
    if (!inputs[0].empty())
        sources.push_back({0, -u.x, -u.y});
    const auto atlas = builder.place({0, 0, 2 * u.width, u.height}, std::move(sources));
    Table table(kPassAccumulate);
    table.active({0, 0, u.width, u.height});
    table.region(0, {0, 0, u.width, u.height});
    table.region(1, {u.width, 0, u.width, u.height});
    table.param(0, static_cast<double>(kernel.op), kernel.inputWeight, kernel.sampleWeight);
    const auto run = builder.run(atlas, table.take());
    return builder.finish(builder.cut(run, {0, 0, u.width, u.height}, u.x, u.y));
}

// --- The path sampler -----------------------------------------------------------------------------

struct Affine final {
    // input-local = [a b c; d e f] (output-local index x, y, 1).
    std::array<double, 6> m{};
};

struct PathSample final {
    Affine start;
    Affine end;
    double weight = 0.0;
};

[[nodiscard]] double rowValue(const PreparedColorGrade& grade, const std::size_t index) {
    const auto offset = static_cast<std::size_t>(grade.data[2]);
    return offset + index < grade.data.size() ? static_cast<double>(grade.data[offset + index])
                                              : 0.0;
}

// The prepared table's transform operands: the frame's own ten values follow the five path
// operands, then one row of ten per shutter row (translate, pivot, scale, rotation, skew, skew axis,
// opacity).
constexpr std::size_t kTransformStride = 10;
constexpr std::size_t kTransformBase = 15;

std::optional<MotionPassPlan> planPath(const MotionPathBlurKernel& kernel,
                                       const PreparedColorGrade& grade,
                                       const std::span<const MotionRect> inputs,
                                       const MotionGeometry& geometry) {
    Builder builder(inputs);
    const auto in = inputs[0];
    if (in.empty())
        return builder.finish(std::nullopt);
    const auto& shutter = kernel.shutter;
    const double hs = geometry.horizontalScale;
    const double vs = geometry.verticalScale;
    const double angle = rowValue(grade, 0) * std::numbers::pi / 180.0;
    const double length = rowValue(grade, 1);
    const double amount = rowValue(grade, 4);
    // The centre in evaluation pixels (pixel index i covers [i, i + 1)).
    const double cx = static_cast<double>(geometry.display.x) +
                      static_cast<double>(geometry.display.width) * 0.5 + rowValue(grade, 2) * hs;
    const double cy = static_cast<double>(geometry.display.y) +
                      static_cast<double>(geometry.display.height) * 0.5 + rowValue(grade, 3) * vs;
    const double reach = std::ceil(shutter.maxBlur * std::max(hs, vs));
    std::vector<PathSample> samples;
    MotionRect out{};
    const auto rows = static_cast<std::size_t>(grade.data[3]);
    if (kernel.mode == MotionPathMode::Transform) {
        // The transform at a row (or the frame's own operands without rows).
        const auto transformAt =
            [&](const std::optional<std::size_t> row) -> std::optional<Transform2DGeometry> {
            const auto v = [&](const std::size_t i) {
                return row ? rowValue(grade, kTransformBase + kTransformStride * *row + i)
                           : rowValue(grade, 5 + i);
            };
            PreparedTransform2D prepared{kernel.transform->extent, kernel.transform->filter,
                                         Transform2DValues{{v(0), v(1)},
                                                           {v(2), v(3)},
                                                           {v(4), v(5)},
                                                           v(6),
                                                           {geometry.centreX, geometry.centreY},
                                                           v(7),
                                                           v(8),
                                                           v(9)}};
            const auto window = render::ImageWindow::create(
                in.x, in.y, static_cast<std::uint64_t>(in.width),
                static_cast<std::uint64_t>(in.height));
            const auto display = render::ImageWindow::create(
                geometry.display.x, geometry.display.y,
                static_cast<std::uint64_t>(geometry.display.width),
                static_cast<std::uint64_t>(geometry.display.height));
            if (!window || !display)
                return std::nullopt;
            const auto descriptor = render::Rgba32fImageDescriptor::create(
                *window.value(), *display.value(), core::PixelAspectRatio::square());
            if (!descriptor)
                return std::nullopt;
            return resolveTransform2DGeometry(prepared, *descriptor.value(), hs, vs);
        };
        const auto transformOpacityAt = [&](const std::optional<std::size_t> row) {
            return row ? rowValue(grade, kTransformBase + kTransformStride * *row + 9)
                       : rowValue(grade, 5 + 9);
        };
        struct Row final {
            std::optional<render::LayerTransform> transform;
            MotionRect window{};
            // Opacity at the row: each sample's weight carries it, since the path sampler sums its
            // samples and a Transform 2D opacity multiplies the whole premultiplied pixel.
            double opacity = 1.0;
        };
        std::vector<Row> resolved;
        const auto count = rows == 0 ? std::size_t{1} : rows;
        for (std::size_t r = 0; r < count; ++r) {
            const auto g = transformAt(rows == 0 ? std::nullopt : std::optional{r});
            if (!g)
                return std::nullopt;
            Row row;
            row.transform = g->transform;
            row.opacity = transformOpacityAt(rows == 0 ? std::nullopt : std::optional{r});
            if (g->window)
                row.window = {g->window->originX(), g->window->originY(),
                              static_cast<std::int64_t>(g->window->extent().width()),
                              static_cast<std::int64_t>(g->window->extent().height())};
            out = unite(out, row.window);
            resolved.push_back(std::move(row));
        }
        if (out.empty())
            return builder.finish(std::nullopt);
        const auto affineOf = [&](const Row& row) -> Affine {
            Affine a;
            if (!row.transform)
                return a;
            const auto base = row.transform->inverseMap(static_cast<double>(out.x),
                                                        static_cast<double>(out.y));
            const auto col = row.transform->inverseMap(static_cast<double>(out.x + 1),
                                                       static_cast<double>(out.y));
            const auto down = row.transform->inverseMap(static_cast<double>(out.x),
                                                        static_cast<double>(out.y + 1));
            a.m = {col.x - base.x, down.x - base.x, base.x,
                   col.y - base.y, down.y - base.y, base.y};
            return a;
        };
        if (rows == 0) {
            samples.push_back({affineOf(resolved[0]), affineOf(resolved[0]), resolved[0].opacity});
        } else {
            // The widest displacement of the input's corners between consecutive rows.
            double widest = 0.0;
            for (std::size_t r = 1; r < resolved.size(); ++r) {
                if (!resolved[r].transform || !resolved[r - 1].transform)
                    continue;
                for (const auto corner : {std::array{0.0, 0.0},
                                          std::array{static_cast<double>(in.width), 0.0},
                                          std::array{0.0, static_cast<double>(in.height)},
                                          std::array{static_cast<double>(in.width),
                                                     static_cast<double>(in.height)}}) {
                    const auto a = resolved[r].transform->forwardMap(corner[0], corner[1]);
                    const auto b = resolved[r - 1].transform->forwardMap(corner[0], corner[1]);
                    widest = std::max(widest, std::hypot(a.x - b.x, a.y - b.y) *
                                                  static_cast<double>(rows - 1));
                }
            }
            const auto strata = static_cast<std::int64_t>((rows - 1) / 2);
            const auto n = std::min(shutterSampleCount(shutter, widest), strata);
            const auto s = shutterSamples(shutter, n);
            const auto step = static_cast<std::size_t>(strata / n);
            for (std::int64_t k = 0; k < n; ++k) {
                const auto r0 = static_cast<std::size_t>(2 * k) * step;
                const auto rc = r0 + step;
                const auto r1 = r0 + 2 * step;
                const auto weight = s.weight[static_cast<std::size_t>(k)];
                if (shutter.jitter)
                    samples.push_back({affineOf(resolved[r0]), affineOf(resolved[r1]),
                                       weight * 0.5 *
                                           (resolved[r0].opacity + resolved[r1].opacity)});
                else
                    samples.push_back({affineOf(resolved[rc]), affineOf(resolved[rc]),
                                       weight * resolved[rc].opacity});
            }
        }
    } else {
        // The path parameter p over [open, close] / shutter: Centered [-1/2, 1/2], Start [0, 1].
        const auto [open, close] = shutterInterval(shutter);
        const double span = shutter.shutter > 0.0 ? shutter.shutter : 1.0;
        const double p0 = open / span;
        const double p1 = close / span;
        // Square space: x times the pixel aspect over the horizontal scale, y over the vertical.
        const double sx = geometry.pixelAspect / hs;
        const double sy = 1.0 / vs;
        // The map at path parameter p: output position P to input position Q (both global).
        const auto mapAt = [&](const double p) -> std::array<double, 6> {
            if (kernel.mode == MotionPathMode::Directional) {
                const double dx = std::cos(angle) * length * p * hs;
                const double dy = std::sin(angle) * length * p * vs;
                return {1.0, 0.0, -dx, 0.0, 1.0, -dy};
            }
            if (kernel.mode == MotionPathMode::Spin) {
                const double phi = -amount * p * std::numbers::pi / 180.0;
                const double c = std::cos(phi);
                const double s = std::sin(phi);
                // Q = c + S^-1 R S (P - c).
                const double a = c;
                const double b = -s * sy / sx;
                const double d = s * sx / sy;
                const double e = c;
                return {a, b, cx - a * cx - b * cy, d, e, cy - d * cx - e * cy};
            }
            const double z = 1.0 / std::max(1.0 + amount * p, 1.0e-3);
            return {z, 0.0, cx - z * cx, 0.0, z, cy - z * cy};
        };
        // Forward image of the input's corners at the path's ends and middle bound the output.
        const auto forward = [&](const std::array<double, 6>& m, const double x, const double y) {
            const double det = m[0] * m[4] - m[1] * m[3];
            const double tx = x - m[2];
            const double ty = y - m[5];
            return std::array{(m[4] * tx - m[1] * ty) / det, (m[0] * ty - m[3] * tx) / det};
        };
        double x0 = static_cast<double>(in.x), y0 = static_cast<double>(in.y);
        double x1 = static_cast<double>(in.x + in.width), y1 = static_cast<double>(in.y + in.height);
        double widest = 0.0;
        for (int i = 0; i <= 8; ++i) {
            const double p = p0 + (p1 - p0) * static_cast<double>(i) / 8.0;
            const auto m = mapAt(p);
            for (const auto corner : {std::array{static_cast<double>(in.x), static_cast<double>(in.y)},
                                      std::array{static_cast<double>(in.x + in.width),
                                                 static_cast<double>(in.y)},
                                      std::array{static_cast<double>(in.x),
                                                 static_cast<double>(in.y + in.height)},
                                      std::array{static_cast<double>(in.x + in.width),
                                                 static_cast<double>(in.y + in.height)}}) {
                const auto f = forward(m, corner[0], corner[1]);
                x0 = std::min(x0, f[0]);
                y0 = std::min(y0, f[1]);
                x1 = std::max(x1, f[0]);
                y1 = std::max(y1, f[1]);
                const auto g = forward(mapAt(p0), corner[0], corner[1]);
                widest = std::max(widest, std::hypot(f[0] - g[0], f[1] - g[1]));
            }
        }
        const MotionRect bound{static_cast<std::int64_t>(std::floor(x0)) - 1,
                               static_cast<std::int64_t>(std::floor(y0)) - 1,
                               static_cast<std::int64_t>(std::ceil(x1) - std::floor(x0)) + 2,
                               static_cast<std::int64_t>(std::ceil(y1) - std::floor(y0)) + 2};
        out = intersect(bound, grow(in, static_cast<std::int64_t>(reach) + 1));
        if (out.empty())
            return builder.finish(std::nullopt);
        const auto n = shutterSampleCount(shutter, widest);
        const auto s = shutterSamples(shutter, n);
        // Output-local index (x, y) is global position (out.x + x + 1/2, ...); input-local index of
        // global position q is q - 1/2 - in.x.
        const auto local = [&](const std::array<double, 6>& m) -> Affine {
            Affine a;
            const double px = static_cast<double>(out.x) + 0.5;
            const double py = static_cast<double>(out.y) + 0.5;
            a.m = {m[0], m[1], m[0] * px + m[1] * py + m[2] - 0.5 - static_cast<double>(in.x),
                   m[3], m[4], m[3] * px + m[4] * py + m[5] - 0.5 - static_cast<double>(in.y)};
            return a;
        };
        const auto pOf = [&](const double t) { return t / span; };
        for (std::size_t k = 0; k < s.weight.size(); ++k) {
            if (shutter.jitter)
                samples.push_back({local(mapAt(pOf(s.start[k]))), local(mapAt(pOf(s.end[k]))),
                                   s.weight[k]});
            else
                samples.push_back({local(mapAt(pOf(s.centre[k]))),
                                   local(mapAt(pOf(s.centre[k]))), s.weight[k]});
        }
    }
    if (!representable(out) || samples.empty() ||
        samples.size() > static_cast<std::size_t>(kMotionMaxSamples))
        return std::nullopt;
    const auto placed = placeAtlas(builder, {{std::nullopt, sizeOf(out)}, {0, {}}});
    const auto& region = placed.atlas.regions;
    Table table(kPassPath);
    table.active(region[0]);
    table.region(0, region[0]);
    table.region(1, region[1]);
    const bool nearest = kernel.transform && kernel.transform->filter == kResampleFilterNearest;
    table.param(0, static_cast<double>(samples.size()), shutter.jitter ? 1.0 : 0.0,
                nearest ? 1.0 : 0.0);
    table.param(1, static_cast<double>(kernel.seed & 0xFFFFFFU), static_cast<double>(grade.data[1]),
                static_cast<double>(out.x), static_cast<double>(out.y));
    for (std::size_t k = 0; k < samples.size(); ++k) {
        const auto& a = samples[k].start.m;
        const auto& b = samples[k].end.m;
        const auto base = kParams + 4 + 4 * k;
        table.vec(base, static_cast<float>(a[0]), static_cast<float>(a[1]),
                  static_cast<float>(a[2]), static_cast<float>(samples[k].weight));
        table.vec(base + 1, static_cast<float>(a[3]), static_cast<float>(a[4]),
                  static_cast<float>(a[5]), 0.0F);
        table.vec(base + 2, static_cast<float>(b[0]), static_cast<float>(b[1]),
                  static_cast<float>(b[2]), 0.0F);
        table.vec(base + 3, static_cast<float>(b[3]), static_cast<float>(b[4]),
                  static_cast<float>(b[5]), 0.0F);
    }
    const auto run = builder.run(placed.buffer, table.take());
    return builder.finish(builder.cut(run, region[0], out.x, out.y));
}

// --- Motion vectors -------------------------------------------------------------------------------

// The decode block (three vec4s at `at`) for the vectors of `kernel` at the evaluation scales.
void decodeBlock(Table& table, const std::size_t at, const VectorBlurKernel& kernel,
                 const MotionGeometry& geometry, const double widest) {
    const auto& d = kernel.decode;
    // Units: 0 full-resolution pixels, 1 normalised (-1 to 1 across the display), 2 evaluation
    // pixels (Bloom's own optical flow).
    const double ux = d.units == 1   ? static_cast<double>(geometry.display.width) * 0.5
                      : d.units == 2 ? 1.0
                                     : geometry.horizontalScale;
    const double uy = d.units == 1   ? static_cast<double>(geometry.display.height) * 0.5
                      : d.units == 2 ? 1.0
                                     : geometry.verticalScale;
    const auto [open, close] = shutterInterval(kernel.shutter);
    table.param(at, d.scale, d.offset, d.flipY ? 1.0 : 0.0, static_cast<double>(d.layout));
    table.param(at + 1, ux, uy, d.alphaValid ? 1.0 : 0.0, widest);
    table.param(at + 2, open, close, kernel.curved ? 1.0 : 0.0);
}

// The plan input index of a vector slot (inputs are [input, slot 0 .. slot 13]).
[[nodiscard]] std::optional<std::uint32_t> slotInput(const std::span<const MotionRect> inputs,
                                                     const std::size_t slot) {
    const auto index = 1 + slot;
    if (index >= inputs.size() || inputs[index].empty())
        return std::nullopt;
    return static_cast<std::uint32_t>(index);
}

// McGuire et al.'s tiles over `out`: [x, y, extent, 1] of the dominant pixel per K x K tile.
[[nodiscard]] std::uint32_t planTiles(Builder& builder, const VectorBlurKernel& kernel,
                                      const MotionGeometry& geometry, const MotionRect out,
                                      const std::int64_t k, const double widest,
                                      const std::uint32_t forward,
                                      const std::optional<std::uint32_t> backward) {
    const std::array<std::int64_t, 2> tiles{(out.width + k - 1) / k, (out.height + k - 1) / k};
    const auto placed = placeAtlas(builder, {{std::nullopt, tiles},
                                             {forward, {}},
                                             {backward, {1, 1}}});
    const auto& r = placed.atlas.regions;
    Table table(kPassTile);
    table.active(r[0]);
    table.region(0, r[0]);
    table.region(1, r[1]);
    if (backward)
        table.region(2, r[2]);
    table.param(0, static_cast<double>(k));
    decodeBlock(table, 1, kernel, geometry, widest);
    const auto& fw = builder.window(forward);
    table.param(4, static_cast<double>(fw.x), static_cast<double>(fw.y));
    if (backward)
        table.param(5, static_cast<double>(builder.window(*backward).x),
                    static_cast<double>(builder.window(*backward).y));
    table.param(6, static_cast<double>(out.x), static_cast<double>(out.y));
    const auto run = builder.run(placed.buffer, table.take());
    const auto maxima = builder.cut(run, r[0], 0, 0);
    // NeighborMax: every tile the widest blur can reach from, one pass at tile resolution.
    const auto neighbours = placeAtlas(builder, {{std::nullopt, tiles}, {maxima, {}}});
    const auto& q = neighbours.atlas.regions;
    Table neighbour(kPassNeighbour);
    neighbour.active(q[0]);
    neighbour.region(0, q[0]);
    neighbour.region(1, q[1]);
    neighbour.param(0, static_cast<double>(k),
                    std::ceil(widest / static_cast<double>(k)), static_cast<double>(tiles[0]),
                    static_cast<double>(tiles[1]));
    neighbour.param(1, static_cast<double>(out.x), static_cast<double>(out.y));
    return builder.cut(builder.run(neighbours.buffer, neighbour.take()), q[0], 0, 0);
}

[[nodiscard]] std::int64_t vectorSamples(const VectorBlurKernel& kernel, const double widest) {
    return shutterSampleCount(kernel.shutter, 2.0 * widest);
}

std::optional<MotionPassPlan> planVector(const VectorBlurKernel& kernel,
                                         const PreparedColorGrade& grade,
                                         const std::span<const MotionRect> inputs,
                                         const MotionGeometry& geometry) {
    Builder builder(inputs);
    const auto in = inputs[0];
    if (in.empty())
        return builder.finish(std::nullopt);
    const double scale = std::max(geometry.horizontalScale, geometry.verticalScale);
    const double widest = std::max(kernel.shutter.maxBlur * scale, 0.0);
    const auto reach = static_cast<std::int64_t>(std::ceil(widest));
    const auto border = kernel.border;
    switch (kernel.stage) {
    case VectorBlurStage::Gather:
    case VectorBlurStage::Splat: {
        const auto forward = slotInput(inputs, kVectorForwardSlot);
        if (!forward)
            return builder.finish(builder.cut(0, {0, 0, in.width, in.height}, in.x, in.y));
        const auto backward = slotInput(inputs, kVectorBackwardSlot);
        // The colour grown by the widest blur, within the colour and display's hull.
        const auto out = intersect(grow(in, reach), unite(in, geometry.display));
        if (out.empty())
            return builder.finish(std::nullopt);
        // A fixed small tile keeps the dominant vector local whatever Maximum blur allows; the
        // neighbourhood grows instead (planTiles). Cost follows the frame, not the limit.
        const auto k = std::clamp<std::int64_t>(reach, 1, kVectorTileSize);
        const auto tiles = planTiles(builder, kernel, geometry, out, k, widest, *forward, backward);
        const auto n = vectorSamples(kernel, widest);
        const auto s = shutterSamples(kernel.shutter, n);
        const auto depth = slotInput(inputs, kVectorDepthSlot);
        if (kernel.stage == VectorBlurStage::Gather) {
            const auto placed = placeAtlas(builder, {{std::nullopt, sizeOf(out)},
                                                     {0, {}},
                                                     {*forward, {}},
                                                     {backward, {1, 1}},
                                                     {tiles, {}},
                                                     {depth, {1, 1}}});
            const auto& r = placed.atlas.regions;
            Table table(kPassGather);
            table.active(r[0]);
            for (std::size_t i = 0; i < 6; ++i)
                if (i != 3 || backward)
                    if (i != 5 || depth)
                        table.region(i, r[i]);
            table.param(0, static_cast<double>(n), kernel.shutter.jitter ? 1.0 : 0.0,
                        static_cast<double>(k), kernel.softZ);
            decodeBlock(table, 1, kernel, geometry, widest);
            table.param(4, static_cast<double>(out.x), static_cast<double>(out.y),
                        static_cast<double>(in.x), static_cast<double>(in.y));
            table.param(5, static_cast<double>(builder.window(*forward).x),
                        static_cast<double>(builder.window(*forward).y));
            if (backward)
                table.param(6, static_cast<double>(builder.window(*backward).x),
                            static_cast<double>(builder.window(*backward).y));
            if (depth)
                table.param(7, static_cast<double>(builder.window(*depth).x),
                            static_cast<double>(builder.window(*depth).y),
                            static_cast<double>(kernel.depthChannel));
            table.param(8, static_cast<double>(out.x), static_cast<double>(out.y),
                        static_cast<double>(kernel.seed & 0xFFFFFFU),
                        static_cast<double>(grade.data[1]));
            table.weights(s.weight);
            const auto run = builder.run(placed.buffer, table.take());
            return builder.finish(builder.cut(run, r[0], out.x, out.y));
        }
        // The splat: up to fourteen regions in the kernel's order.
        std::vector<Slot> slots{{std::nullopt, sizeOf(out)},
                                {0, {}},
                                {*forward, {}},
                                {backward, {1, 1}},
                                {tiles, {}},
                                {depth, {1, 1}},
                                {slotInput(inputs, kVectorMatteSlot), {1, 1}},
                                {slotInput(inputs, kVectorNextSlot), {1, 1}},
                                {slotInput(inputs, kVectorNextFlowSlot), {1, 1}},
                                {slotInput(inputs, kVectorPreviousSlot), {1, 1}},
                                {slotInput(inputs, kVectorPreviousFlowSlot), {1, 1}},
                                {slotInput(inputs, kVectorConsistencySlot), {1, 1}},
                                {slotInput(inputs, kVectorNextConsistencySlot), {1, 1}},
                                {slotInput(inputs, kVectorPreviousConsistencySlot), {1, 1}}};
        const auto placed = placeAtlas(builder, slots);
        const auto& r = placed.atlas.regions;
        Table table(kPassSplat);
        table.active(r[0]);
        const auto origin = [&](const std::size_t slot) -> std::array<double, 2> {
            if (!slots[slot].buffer)
                return {0.0, 0.0};
            const auto& w = builder.window(*slots[slot].buffer);
            return {static_cast<double>(w.x), static_cast<double>(w.y)};
        };
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (i == 0 || slots[i].buffer)
                table.region(i, r[i]);
        table.param(0, static_cast<double>(n), kernel.shutter.jitter ? 1.0 : 0.0,
                    static_cast<double>(k), kernel.softZ);
        decodeBlock(table, 1, kernel, geometry, widest);
        table.param(4, static_cast<double>(out.x), static_cast<double>(out.y));
        const std::array<std::pair<std::size_t, std::size_t>, 10> origins{
            {{5, 2}, {6, 3}, {7, 1}, {8, 5}, {9, 6}, {10, 7}, {11, 9}, {12, 11}, {13, 8}, {14, 10}}};
        for (const auto& [param, slot] : origins) {
            const auto o = origin(slot);
            table.param(param, o[0], o[1]);
        }
        const auto nc = origin(12);
        const auto pc = origin(13);
        table.param(15, nc[0], nc[1], pc[0], pc[1]);
        table.param(16, static_cast<double>(kernel.layer), static_cast<double>(kernel.zMode),
                    kernel.brightness, static_cast<double>(border));
        table.param(17, static_cast<double>(kernel.seed & 0xFFFFFFU),
                    static_cast<double>(grade.data[1]), static_cast<double>(kernel.depthChannel),
                    4.0);
        table.param(18, static_cast<double>(out.x), static_cast<double>(out.y));
        table.weights(s.weight);
        const auto run = builder.run(placed.buffer, table.take());
        return builder.finish(builder.cut(run, r[0], out.x, out.y));
    }
    case VectorBlurStage::Fill: {
        // Jump flooding to the nearest known pixel, then Baker et al.'s outside-in average.
        const auto matte = slotInput(inputs, kVectorMatteSlot);
        const auto init = placeAtlas(builder, {{std::nullopt, sizeOf(in)},
                                               {0, {}},
                                               {matte, {1, 1}}});
        const auto& r = init.atlas.regions;
        Table table(kPassJfaInit);
        table.active(r[0]);
        table.region(0, r[0]);
        table.region(1, r[1]);
        if (matte)
            table.region(2, r[2]);
        table.param(0, static_cast<double>(in.x), static_cast<double>(in.y),
                    matte ? static_cast<double>(builder.window(*matte).x) : 0.0,
                    matte ? static_cast<double>(builder.window(*matte).y) : 0.0);
        table.param(1, kernel.threshold);
        auto seeds = builder.cut(builder.run(init.buffer, table.take()), r[0], 0, 0);
        std::int64_t jump = 1;
        while (jump * 2 < std::max(in.width, in.height))
            jump *= 2;
        for (; jump >= 1; jump /= 2) {
            Table step(kPassJfaStep);
            step.active({0, 0, in.width, in.height});
            step.region(0, {0, 0, in.width, in.height});
            step.param(0, static_cast<double>(jump));
            seeds = builder.run(seeds, step.take());
        }
        const auto fill = placeAtlas(builder, {{std::nullopt, sizeOf(in)}, {0, {}}, {seeds, {}}});
        const auto& f = fill.atlas.regions;
        Table pass(kPassFill);
        pass.active(f[0]);
        pass.region(0, f[0]);
        pass.region(1, f[1]);
        pass.region(2, f[2]);
        const auto run = builder.run(fill.buffer, pass.take());
        return builder.finish(builder.cut(run, f[0], in.x, in.y));
    }
    case VectorBlurStage::TemporalFill:
    case VectorBlurStage::TemporalFillMask: {
        const std::vector<Slot> slots{{std::nullopt, sizeOf(in)},
                                      {0, {}},
                                      {slotInput(inputs, kVectorMatteSlot), {1, 1}},
                                      {slotInput(inputs, kVectorForwardSlot), {1, 1}},
                                      {slotInput(inputs, kVectorBackwardSlot), {1, 1}},
                                      {slotInput(inputs, kVectorNextSlot), {1, 1}},
                                      {slotInput(inputs, kVectorPreviousSlot), {1, 1}},
                                      {slotInput(inputs, kVectorNextMatteSlot), {1, 1}},
                                      {slotInput(inputs, kVectorPreviousMatteSlot), {1, 1}}};
        const auto placed = placeAtlas(builder, slots);
        const auto& r = placed.atlas.regions;
        Table table(kPassTemporalFill);
        table.active(r[0]);
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (i <= 1 || slots[i].buffer)
                table.region(i, r[i]);
        for (std::size_t i = 1; i < slots.size(); ++i) {
            if (!slots[i].buffer)
                continue;
            const auto& w = builder.window(*slots[i].buffer);
            table.param(i - 1, static_cast<double>(w.x), static_cast<double>(w.y));
        }
        table.param(8, kernel.threshold,
                    kernel.stage == VectorBlurStage::TemporalFillMask ? 1.0 : 0.0,
                    static_cast<double>(border));
        const auto run = builder.run(placed.buffer, table.take());
        return builder.finish(builder.cut(run, r[0], in.x, in.y));
    }
    case VectorBlurStage::Consistency: {
        // The input flow against the other frame's flow back (the forward slot); with the
        // backward slot and the previous-flow slot (its flow back), the pair as data.
        const auto back = slotInput(inputs, kVectorForwardSlot);
        const auto second = slotInput(inputs, kVectorBackwardSlot);
        const auto secondBack = slotInput(inputs, kVectorPreviousFlowSlot);
        const bool pair = second && secondBack;
        const auto placed = placeAtlas(builder, {{std::nullopt, sizeOf(in)},
                                                 {0, {}},
                                                 {back, {1, 1}},
                                                 {pair ? second : std::nullopt, {1, 1}},
                                                 {pair ? secondBack : std::nullopt, {1, 1}}});
        const auto& r = placed.atlas.regions;
        Table table(kPassConsistency);
        table.active(r[0]);
        table.region(0, r[0]);
        table.region(1, r[1]);
        if (back)
            table.region(2, r[2]);
        const auto origin = [&](const std::optional<std::uint32_t> b) -> std::array<double, 2> {
            if (!b)
                return {0.0, 0.0};
            return {static_cast<double>(builder.window(*b).x),
                    static_cast<double>(builder.window(*b).y)};
        };
        table.param(0, static_cast<double>(in.x), static_cast<double>(in.y), origin(back)[0],
                    origin(back)[1]);
        table.param(1, static_cast<double>(border), kernel.layer == 1 ? 1.0 : 0.0);
        if (pair) {
            table.region(3, r[3]);
            table.region(4, r[4]);
            table.param(2, origin(second)[0], origin(second)[1], origin(secondBack)[0],
                        origin(secondBack)[1]);
        }
        const auto run = builder.run(placed.buffer, table.take());
        return builder.finish(builder.cut(run, r[0], in.x, in.y));
    }
    }
    return std::nullopt;
}

// --- Optical flow ---------------------------------------------------------------------------------

struct FlowSchedule final {
    int finest = 0;
    int levels = 5;
    int warps = 5;
    int iterations = 30;
};

[[nodiscard]] FlowSchedule flowSchedule(const OpticalFlowKernel& kernel) noexcept {
    if (kernel.method == 0) {
        // DIS: OpenCV's FAST preset stops two levels above full resolution.
        switch (kernel.quality) {
        case document::kMotionQualityDraft:
            return {2, 5, 0, 0};
        case document::kMotionQualityStandard:
            return {1, 5, 0, 0};
        default:
            return {0, 5, 0, 0};
        }
    }
    switch (kernel.quality) {
    case document::kMotionQualityDraft:
        return {2, 5, 2, 10};
    case document::kMotionQualityStandard:
        return {1, 5, 3, 15};
    case document::kMotionQualityHigh:
        return {1, 5, 5, 25};
    default:
        return {0, 5, 5, 30};
    }
}

[[nodiscard]] std::array<std::int64_t, 2> half(const std::array<std::int64_t, 2> s) noexcept {
    return {(s[0] + 1) / 2, (s[1] + 1) / 2};
}

std::optional<MotionPassPlan> planFlow(const OpticalFlowKernel& kernel,
                                       const std::span<const MotionRect> inputs) {
    Builder builder(inputs);
    const auto i0 = inputs[0];
    const auto i1 = inputs.size() > 1 ? inputs[1] : MotionRect{};
    const auto domain = unite(i0, i1);
    if (domain.empty())
        return builder.finish(std::nullopt);
    const auto border = kernel.border;
    if (kernel.stage == OpticalFlowStage::CutDetect) {
        const auto flow = inputs.size() > 3 && !inputs[3].empty() ? std::optional<std::uint32_t>{3}
                                                                   : std::nullopt;
        if (!flow || i0.empty() || i1.empty())
            return builder.finish(std::nullopt);
        const auto placed = placeAtlas(builder, {{std::nullopt, sizeOf(domain)},
                                                 {0, {}},
                                                 {1, {}},
                                                 {*flow, {}}});
        const auto& r = placed.atlas.regions;
        Table table(kPassCutResidual);
        table.active(r[0]);
        for (std::size_t i = 0; i < 4; ++i)
            table.region(i, r[i]);
        table.param(0, static_cast<double>(i0.x), static_cast<double>(i0.y),
                    static_cast<double>(i1.x), static_cast<double>(i1.y));
        table.param(1, static_cast<double>(inputs[3].x), static_cast<double>(inputs[3].y),
                    static_cast<double>(border));
        table.param(2, static_cast<double>(domain.x), static_cast<double>(domain.y));
        auto sums = builder.cut(builder.run(placed.buffer, table.take()), r[0], 0, 0);
        auto size = sizeOf(domain);
        while (size[0] > 1 || size[1] > 1) {
            const std::array<std::int64_t, 2> next{(size[0] + 15) / 16, (size[1] + 15) / 16};
            const auto reduce = placeAtlas(builder, {{std::nullopt, next}, {sums, {}}});
            const auto& q = reduce.atlas.regions;
            Table pass(kPassCutReduce);
            pass.active(q[0]);
            pass.region(0, q[0]);
            pass.region(1, q[1]);
            sums = builder.cut(builder.run(reduce.buffer, pass.take()), q[0], 0, 0);
            size = next;
        }
        const auto broadcast = placeAtlas(builder, {{std::nullopt, sizeOf(domain)}, {sums, {}}});
        const auto& b = broadcast.atlas.regions;
        Table pass(kPassCutBroadcast);
        pass.active(b[0]);
        pass.region(0, b[0]);
        pass.region(1, b[1]);
        pass.param(0, kernel.cutThreshold);
        const auto run = builder.run(broadcast.buffer, pass.take());
        return builder.finish(builder.cut(run, b[0], domain.x, domain.y));
    }
    if (i0.empty() || i1.empty())
        return builder.finish(std::nullopt);
    const auto mask = inputs.size() > 2 && !inputs[2].empty() && kernel.layer != 0
                          ? std::optional<std::uint32_t>{2}
                          : std::nullopt;
    const auto schedule = flowSchedule(kernel);
    // The grey pyramid of both frames over the domain.
    std::vector<std::array<std::int64_t, 2>> sizes{sizeOf(domain)};
    while (static_cast<int>(sizes.size()) < schedule.levels &&
           std::min(half(sizes.back())[0], half(sizes.back())[1]) >= 16)
        sizes.push_back(half(sizes.back()));
    const int coarsest = static_cast<int>(sizes.size()) - 1;
    const int finest = std::min(schedule.finest, coarsest);
    std::array<std::vector<std::uint32_t>, 2> grey;
    {
        const auto placed = placeAtlas(builder, {{std::nullopt, sizes[0]},
                                                 {std::nullopt, sizes[0]},
                                                 {std::nullopt, {0, 0}},
                                                 {std::nullopt, {0, 0}},
                                                 {0, {}},
                                                 {1, {}}});
        const auto& r = placed.atlas.regions;
        Table table(kPassGrey);
        table.active(placed.atlas.window);
        table.region(0, r[0]);
        table.region(1, r[1]);
        table.region(4, r[4]);
        table.region(5, r[5]);
        table.param(1, static_cast<double>(border));
        table.param(2, static_cast<double>(domain.x), static_cast<double>(domain.y));
        table.param(3, static_cast<double>(i0.x), static_cast<double>(i0.y));
        table.param(4, static_cast<double>(i1.x), static_cast<double>(i1.y));
        const auto run = builder.run(placed.buffer, table.take());
        grey[0].push_back(builder.cut(run, r[0], 0, 0));
        grey[1].push_back(builder.cut(run, r[1], 0, 0));
    }
    for (int l = 1; l <= coarsest; ++l)
        for (int f = 0; f < 2; ++f) {
            const auto placed = placeAtlas(
                builder, {{std::nullopt, sizes[static_cast<std::size_t>(l)]},
                          {grey[static_cast<std::size_t>(f)].back(), {}}});
            const auto& r = placed.atlas.regions;
            Table table(kPassDecimate);
            table.active(r[0]);
            table.region(0, r[0]);
            table.region(1, r[1]);
            grey[static_cast<std::size_t>(f)].push_back(
                builder.cut(builder.run(placed.buffer, table.take()), r[0], 0, 0));
        }
    const auto maskParams = [&](Table& table, const std::size_t at, const int level) {
        if (!mask)
            return;
        const auto& m = builder.window(*mask);
        table.param(at, static_cast<double>(domain.x - m.x), static_cast<double>(domain.y - m.y),
                    std::ldexp(1.0, level), kernel.layer == 2 ? 1.0 : 0.0);
    };
    if (kernel.method == 0) {
        // DIS: per level the patch search, densification and variational refinement.
        constexpr int patch = 8;
        constexpr int stride = 4;
        constexpr int iterations = 16;
        constexpr int refinement = 5;
        std::optional<std::uint32_t> coarser;
        for (int l = coarsest; l >= 0; --l) {
            const auto s = sizes[static_cast<std::size_t>(l)];
            const bool solve = l >= finest;
            const std::array<std::int64_t, 2> grid{
                std::max<std::int64_t>((s[0] - patch) / stride + 1, 1),
                std::max<std::int64_t>((s[1] - patch) / stride + 1, 1)};
            std::vector<Slot> slots{{std::nullopt, s},    {std::nullopt, s}, {std::nullopt, grid},
                                    {std::nullopt, s},    {std::nullopt, s}, {std::nullopt, s},
                                    {std::nullopt, s},    {coarser, {1, 1}},
                                    {grey[0][static_cast<std::size_t>(l)], {}},
                                    {grey[1][static_cast<std::size_t>(l)], {}},
                                    {mask, {1, 1}}};
            const auto placed = placeAtlas(builder, slots);
            const auto& r = placed.atlas.regions;
            const auto common = [&](Table& table) {
                for (std::size_t i = 0; i < slots.size(); ++i)
                    if (i < 7 || slots[i].buffer)
                        table.region(i, r[i]);
                table.param(0, patch, stride, iterations, static_cast<double>(border));
                table.param(1, 5.0, 10.0, 20.0, refinement);
                maskParams(table, 2, l);
            };
            Table init(kPassDisInit);
            init.active(placed.atlas.window);
            common(init);
            auto atlas = builder.run(placed.buffer, init.take());
            if (solve) {
                Table search(kPassDisSearch);
                search.active(r[2]);
                common(search);
                atlas = builder.run(atlas, search.take());
                Table densify(kPassDisDensify);
                densify.active(r[3]);
                common(densify);
                atlas = builder.run(atlas, densify.take());
                Table data(kPassDisData);
                data.active(placed.atlas.window);
                common(data);
                atlas = builder.run(atlas, data.take());
                for (int it = 0; it < refinement; ++it) {
                    Table jacobi(kPassDisIterate);
                    jacobi.active(r[6]);
                    common(jacobi);
                    atlas = builder.run(atlas, jacobi.take());
                }
                Table update(kPassDisUpdate);
                update.active(r[3]);
                common(update);
                atlas = builder.run(atlas, update.take());
            }
            if (l == 0) {
                return builder.finish(builder.cut(atlas, r[3], domain.x, domain.y));
            }
            coarser = builder.cut(atlas, r[3], 0, 0);
            // At the finest solved level above full resolution the flow is carried straight up.
            if (l == finest) {
                // Upsample straight to full resolution.
                const auto s0 = sizes[0];
                std::vector<Slot> up{{std::nullopt, s0},  {std::nullopt, s0}, {std::nullopt, {1, 1}},
                                     {std::nullopt, s0},  {std::nullopt, {0, 0}},
                                     {std::nullopt, {0, 0}}, {std::nullopt, {0, 0}},
                                     {coarser, {1, 1}},
                                     {grey[0][0], {}},
                                     {grey[1][0], {}}};
                const auto upPlaced = placeAtlas(builder, up);
                const auto& u = upPlaced.atlas.regions;
                Table table(kPassDisInit);
                table.active(upPlaced.atlas.window);
                for (std::size_t i = 0; i < up.size(); ++i)
                    if (i < 4 || up[i].buffer)
                        table.region(i, u[i]);
                table.param(0, patch, stride, iterations, static_cast<double>(border));
                table.param(1, 5.0, 10.0, 20.0, refinement);
                const auto run = builder.run(upPlaced.buffer, table.take());
                return builder.finish(builder.cut(run, u[3], domain.x, domain.y));
            }
        }
        return std::nullopt;
    }
    // TV-L1: per level the start, then warps of iterations.
    std::optional<std::uint32_t> s1, s2;
    for (int l = coarsest; l >= 0; --l) {
        const auto s = sizes[static_cast<std::size_t>(l)];
        const bool solve = l >= finest;
        if (!solve && l != 0)
            continue;
        std::vector<Slot> slots{{std::nullopt, s},
                                {std::nullopt, s},
                                {std::nullopt, s},
                                {std::nullopt, s},
                                {std::nullopt, s},
                                {s1, {1, 1}},
                                {s2, {1, 1}},
                                {grey[0][static_cast<std::size_t>(l)], {}},
                                {grey[1][static_cast<std::size_t>(l)], {}},
                                {mask, {1, 1}}};
        // The level's five regions first (their own atlas), the start's regions below them.
        const auto level = layout({s, s, s, s, s});
        auto initSizes = std::vector<std::array<std::int64_t, 2>>{};
        for (std::size_t i = 5; i < slots.size(); ++i)
            initSizes.push_back(slots[i].buffer ? sizeOf(builder.window(*slots[i].buffer))
                                                : std::array<std::int64_t, 2>{1, 1});
        auto below = layout(initSizes);
        Atlas atlas;
        atlas.regions = level.regions;
        for (auto r : below.regions) {
            r.y += level.window.height;
            atlas.regions.push_back(r);
        }
        atlas.window = {0, 0, std::max(level.window.width, below.window.width),
                        level.window.height + below.window.height};
        std::vector<MotionPlacement> sources;
        for (std::size_t i = 5; i < slots.size(); ++i)
            if (slots[i].buffer) {
                const auto& w = builder.window(*slots[i].buffer);
                sources.push_back({*slots[i].buffer, atlas.regions[i].x - w.x,
                                   atlas.regions[i].y - w.y});
            }
        const auto start = builder.place(atlas.window, std::move(sources));
        const auto& r = atlas.regions;
        const auto regions = [&](Table& table, const bool init) {
            for (std::size_t i = 0; i < (init ? slots.size() : 5); ++i)
                if (i < 5 || slots[i].buffer)
                    table.region(i, r[i]);
            table.param(0, 0.15, 0.3, 0.25, static_cast<double>(border));
            maskParams(table, 1, l);
        };
        Table init(kPassTvInit);
        init.active(atlas.window);
        regions(init, true);
        auto current = builder.cut(builder.run(start, init.take()), level.window, 0, 0);
        if (solve)
            for (int w = 0; w < schedule.warps; ++w) {
                Table warp(kPassTvWarp);
                warp.active(level.window);
                regions(warp, false);
                current = builder.run(current, warp.take());
                for (int it = 0; it < schedule.iterations; ++it) {
                    Table iterate(kPassTvIterate);
                    iterate.active(level.window);
                    regions(iterate, false);
                    current = builder.run(current, iterate.take());
                }
            }
        if (l == 0) {
            const auto placed = placeAtlas(builder, {{current, {}}, {std::nullopt, s}});
            const auto& q = placed.atlas.regions;
            // The level atlas's S1 and S2 regions at the placed atlas's first region.
            Table out(kPassFlowOut);
            const MotionRect outRegion = q[1];
            out.active(outRegion);
            out.region(0, {q[0].x + r[0].x, q[0].y + r[0].y, s[0], s[1]});
            out.region(1, {q[0].x + r[1].x, q[0].y + r[1].y, s[0], s[1]});
            out.region(2, outRegion);
            const auto run = builder.run(placed.buffer, out.take());
            return builder.finish(builder.cut(run, outRegion, domain.x, domain.y));
        }
        s1 = builder.cut(current, r[0], 0, 0);
        s2 = builder.cut(current, r[1], 0, 0);
    }
    return std::nullopt;
}

} // namespace

std::size_t motionInputCount(const ImageEffectKernel& kernel) noexcept {
    if (std::holds_alternative<TemporalAccumulateKernel>(kernel))
        return 2;
    if (std::holds_alternative<MotionPathBlurKernel>(kernel))
        return 1;
    if (std::holds_alternative<VectorBlurKernel>(kernel))
        return 1 + kVectorSlots;
    if (std::holds_alternative<OpticalFlowKernel>(kernel))
        return 4;
    return 0;
}

std::optional<MotionPassPlan> planMotion(const ImageEffectKernel& kernel,
                                         const PreparedColorGrade& grade,
                                         const std::span<const MotionRect> inputs,
                                         const MotionGeometry& geometry) {
    if (grade.kind != ColorGradeKind::Motion || grade.data.size() < 4 ||
        inputs.size() != motionInputCount(kernel) || !std::isfinite(geometry.horizontalScale) ||
        !std::isfinite(geometry.verticalScale) || !(geometry.horizontalScale > 0.0) ||
        !(geometry.verticalScale > 0.0) || !(geometry.pixelAspect > 0.0))
        return std::nullopt;
    for (const auto& r : inputs)
        if (!r.empty() && !representable(r))
            return std::nullopt;
    if (const auto* accumulate = std::get_if<TemporalAccumulateKernel>(&kernel))
        return planAccumulate(*accumulate, inputs);
    if (const auto* path = std::get_if<MotionPathBlurKernel>(&kernel))
        return planPath(*path, grade, inputs, geometry);
    if (const auto* vector = std::get_if<VectorBlurKernel>(&kernel))
        return planVector(*vector, grade, inputs, geometry);
    if (const auto* flow = std::get_if<OpticalFlowKernel>(&kernel))
        return planFlow(*flow, inputs);
    return std::nullopt;
}

} // namespace bloom::runtime::detail
