// #566 Auto Exposure. docs/architecture/image-filters.md, "Auto Exposure", is the specification.
//
// The pixel arithmetic is additions, multiplications, min, max, correctly rounded divisions, and the
// logarithm and the exponential of base two, in one fixed order. The table (binary32):
//   0 pass (AutoExposureStage)  1 grid (Reduce)
//   2 exposure amount  3 log2 target  4 minimum gain  5 maximum gain (stops)
//   6 white balance amount  7 white balance limit (stops)
//   8 attack  9 release  10 white balance  (the filter's one-pole coefficients per frame)
//  11 means in the window (Filter)
//  12 source width  13 source height  14 tile width  15 tile height  16 output width  17 output height
//  18 1 / source pixels  19 has gain pixel (Apply)  20 gain pixel x  21 gain pixel y
//  32 atlas position (x, y) of each mean, oldest first, (-1, -1) when absent (Filter)

#include "auto_exposure_image_effect.hpp"
#include "auto_exposure_math.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/document/auto_exposure_nodes.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace bloom::runtime::detail {
namespace {

constexpr std::size_t kCells = 32;
constexpr double kExactInteger = 16777216.0; // 2^24
// Rec. 709 luminance of scene-linear colour.
constexpr float kLumaRed = 0.2126F;
constexpr float kLumaGreen = 0.7152F;
constexpr float kLumaBlue = 0.0722F;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

[[nodiscard]] std::int64_t ceilDiv(const std::int64_t a, const std::int64_t b) noexcept {
    return (a + b - 1) / b;
}

// The one-pole coefficient of a lag of `seconds` sampled every `frame` seconds: the share of the
// distance to the target covered in one frame. No lag follows at once.
[[nodiscard]] float lagCoefficient(const double seconds, const double frame) noexcept {
    if (!(seconds > 0.0))
        return 1.0F;
    return static_cast<float>(1.0 - std::exp(-frame / seconds));
}

// What a Reduce multiplies its tile sums by: the first pass (a grid of more than one tile) divides by
// the frame's pixel count, so the last pass, which only sums, ends on the mean.
[[nodiscard]] float reductionScale(const std::int64_t grid, const std::int64_t width,
                                   const std::int64_t height) noexcept {
    if (grid <= 1)
        return 1.0F;
    return static_cast<float>(1.0 / (static_cast<double>(width) * static_cast<double>(height)));
}

// The gains one mean asks for: the exposure and the red and blue white balance, all in stops.
[[nodiscard]] AutoExposureGains desiredGains(const float* const k, const AutoExposureMean& m) {
    const float r = std::max(m[0], kAutoExposureFloor);
    const float g = std::max(m[1], kAutoExposureFloor);
    const float b = std::max(m[2], kAutoExposureFloor);
    const float limit = k[7];
    const float quotientRed = g / r;
    const float quotientBlue = g / b;
    const float logRed = std::min(std::max(detail::autoExposureLog2(quotientRed), -limit), limit);
    const float logBlue = std::min(std::max(detail::autoExposureLog2(quotientBlue), -limit), limit);
    const float wr = logRed * k[6];
    const float wb = logBlue * k[6];
    const float correctedRed = r * detail::autoExposureExp2(wr);
    const float correctedBlue = b * detail::autoExposureExp2(wb);
    const float luma = kLumaRed * correctedRed + kLumaGreen * g + kLumaBlue * correctedBlue;
    const float stops = k[3] - detail::autoExposureLog2(std::max(luma, kAutoExposureFloor));
    const float e = std::min(std::max(stops, k[4]), k[5]) * k[2];
    return {e, wr, wb};
}

// The source pixels of one image, as rows of its data window.
struct Rows final {
    std::int64_t originX = 0;
    std::int64_t originY = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::vector<std::span<const render::Rgba32f>> rows;
    [[nodiscard]] bool present() const noexcept { return width > 0 && height > 0; }
    [[nodiscard]] const render::Rgba32f& at(const std::int64_t x, const std::int64_t y) const {
        return rows[static_cast<std::size_t>(y - originY)][static_cast<std::size_t>(x - originX)];
    }
};

[[nodiscard]] bool rowsOf(const render::Rgba32fImage* const picture,
                          std::vector<render::Rgba32fImageView>& views, Rows& rows) {
    if (picture == nullptr)
        return true;
    const auto view = picture->view();
    if (!view)
        return false;
    views.push_back(*view.value());
    const auto window = picture->descriptor()->dataWindow();
    rows.originX = window.originX();
    rows.originY = window.originY();
    rows.width = static_cast<std::int64_t>(window.extent().width());
    rows.height = static_cast<std::int64_t>(window.extent().height());
    rows.rows.reserve(static_cast<std::size_t>(rows.height));
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        const auto row = views.back().row(y);
        if (!row)
            return false;
        rows.rows.push_back(*row.value());
    }
    return true;
}

// A descriptor like `source`'s but over `window` as both its data and display window.
[[nodiscard]] std::optional<render::Rgba32fImageDescriptor>
descriptorOver(const render::ImageWindow& window, const render::Rgba32fImageDescriptor& source) {
    const auto created =
        render::Rgba32fImageDescriptor::create(window, window, source.pixelAspect());
    if (!created)
        return std::nullopt;
    return *created.value();
}

} // namespace

AutoExposureGrid autoExposureGrid(const std::int64_t width, const std::int64_t height,
                                  const std::int64_t grid) noexcept {
    const auto axis = [grid](const std::int64_t length) {
        const auto tiles = std::max<std::int64_t>(std::min(grid, length), 1);
        const auto tile = ceilDiv(std::max<std::int64_t>(length, 1), tiles);
        return std::pair{tile, ceilDiv(std::max<std::int64_t>(length, 1), tile)};
    };
    const auto [tileWidth, columns] = axis(width);
    const auto [tileHeight, rows] = axis(height);
    return {tileWidth, tileHeight, columns, rows};
}

std::optional<PreparedColorGrade> prepareAutoExposureGrade(const AutoExposureKernel& kernel,
                                                           const std::span<const double> values) {
    if (kernel.grid < 1 || kernel.grid > 256)
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::AutoExposure,
                             std::vector<float>(kAutoExposurePreparedFloats, 0.0F), false};
    auto& d = grade.data;
    d[1] = static_cast<float>(kernel.grid);
    if (kernel.role != AutoExposureRole::Filter) {
        if (!values.empty())
            return std::nullopt;
        d[0] = static_cast<float>(kernel.role == AutoExposureRole::Reduce
                                      ? AutoExposureStage::Reduce
                                      : AutoExposureStage::Apply);
        // Only a Reduce reads its grid.
        if (kernel.role == AutoExposureRole::Apply)
            d[1] = 0.0F;
        return grade;
    }
    if (values.size() != document::kAutoExposureScalarCount + 1)
        return std::nullopt;
    for (const auto& spec : document::autoExposureParameters()) {
        if (spec.kind != document::AutoExposureParameterKind::Scalar)
            continue;
        const double value = values[document::autoExposureScalarIndex(spec.role)];
        if (!std::isfinite(value) || value < spec.minimum || value > spec.maximum)
            return std::nullopt;
    }
    const double frame = values[document::kAutoExposureScalarCount];
    if (!std::isfinite(frame) || !(frame > 0.0))
        return std::nullopt;
    const auto value = [&](const std::string_view role) {
        return values[document::autoExposureScalarIndex(role)];
    };
    d[0] = static_cast<float>(AutoExposureStage::Filter);
    d[1] = 0.0F;
    d[2] = static_cast<float>(value("exposure"));
    d[3] = static_cast<float>(std::log2(value("target")));
    d[4] = static_cast<float>(value("minGain"));
    d[5] = static_cast<float>(value("maxGain"));
    d[6] = static_cast<float>(value("whiteBalance"));
    d[7] = static_cast<float>(value("balanceLimit"));
    d[8] = lagCoefficient(value("attack"), frame);
    d[9] = lagCoefficient(value("release"), frame);
    d[10] = lagCoefficient(value("balanceLag"), frame);
    return grade;
}

AutoExposureStage autoExposureStage(const PreparedColorGrade& grade) noexcept {
    return (grade.data.size() == kAutoExposurePreparedFloats ||
            grade.data.size() == kAutoExposureProgramFloats) &&
                   grade.data[0] >= 0.0F && grade.data[0] <= 2.0F
               ? static_cast<AutoExposureStage>(static_cast<int>(grade.data[0]))
               : AutoExposureStage::Reduce;
}

std::array<std::int64_t, 2> autoExposureOutputSize(const PreparedColorGrade& grade,
                                                   const std::int64_t sourceWidth,
                                                   const std::int64_t sourceHeight) noexcept {
    switch (autoExposureStage(grade)) {
    case AutoExposureStage::Reduce: {
        const auto geometry = autoExposureGrid(sourceWidth, sourceHeight,
                                               static_cast<std::int64_t>(grade.data[1]));
        return {geometry.width, geometry.height};
    }
    case AutoExposureStage::Filter:
        return {1, 1};
    case AutoExposureStage::Apply:
        break;
    }
    return {sourceWidth, sourceHeight};
}

std::optional<std::vector<float>>
autoExposureProgramData(const PreparedColorGrade& grade, const AutoExposurePlacement& placement) {
    if (grade.kind != ColorGradeKind::AutoExposure || grade.data.size() != kAutoExposurePreparedFloats)
        return std::nullopt;
    const auto stage = autoExposureStage(grade);
    std::vector<float> d(kAutoExposureProgramFloats, 0.0F);
    std::copy(grade.data.begin(), grade.data.begin() + 11, d.begin());
    if (stage != AutoExposureStage::Filter) {
        if (placement.sourceWidth <= 0 || placement.sourceHeight <= 0)
            return std::nullopt;
        for (const auto value : {placement.sourceWidth, placement.sourceHeight, placement.stateX,
                                 placement.stateY})
            if (std::abs(static_cast<double>(value)) > kExactInteger)
                return std::nullopt;
        const auto size =
            autoExposureOutputSize(grade, placement.sourceWidth, placement.sourceHeight);
        d[12] = static_cast<float>(placement.sourceWidth);
        d[13] = static_cast<float>(placement.sourceHeight);
        d[16] = static_cast<float>(size[0]);
        d[17] = static_cast<float>(size[1]);
        if (stage == AutoExposureStage::Reduce) {
            const auto geometry = autoExposureGrid(placement.sourceWidth, placement.sourceHeight,
                                                   static_cast<std::int64_t>(grade.data[1]));
            d[14] = static_cast<float>(geometry.tileWidth);
            d[15] = static_cast<float>(geometry.tileHeight);
            d[18] = reductionScale(static_cast<std::int64_t>(grade.data[1]), placement.sourceWidth,
                                   placement.sourceHeight);
        } else {
            d[19] = placement.hasState ? 1.0F : 0.0F;
            d[20] = static_cast<float>(placement.stateX);
            d[21] = static_cast<float>(placement.stateY);
        }
    } else {
        const auto count = placement.cells.size();
        if (count < 1 || count > kAutoExposureMaxCells)
            return std::nullopt;
        d[11] = static_cast<float>(count);
        d[16] = 1.0F;
        d[17] = 1.0F;
        for (std::size_t i = 0; i < count; ++i) {
            for (const auto value : placement.cells[i])
                if (std::abs(static_cast<double>(value)) > kExactInteger)
                    return std::nullopt;
            d[kCells + 2 * i] = static_cast<float>(placement.cells[i][0]);
            d[kCells + 2 * i + 1] = static_cast<float>(placement.cells[i][1]);
        }
    }
    if (!std::ranges::all_of(d, [](const float v) { return std::isfinite(v); }))
        return std::nullopt;
    return d;
}

bool validAutoExposureProgramData(const std::vector<float>& d) noexcept {
    if (d.size() != kAutoExposureProgramFloats ||
        !std::ranges::all_of(d, [](const float v) { return std::isfinite(v); }))
        return false;
    const auto whole = [](const float v, const float low, const float high) {
        return v >= low && v <= high && v == std::floor(v);
    };
    if (!whole(d[0], 0.0F, 2.0F))
        return false;
    if (d[0] == 1.0F)
        return whole(d[11], 1.0F, static_cast<float>(kAutoExposureMaxCells)) && d[8] > 0.0F &&
               d[8] <= 1.0F && d[9] > 0.0F && d[9] <= 1.0F && d[10] > 0.0F && d[10] <= 1.0F &&
               d[4] <= 0.0F && d[5] >= 0.0F && d[7] >= 0.0F;
    return d[12] >= 1.0F && d[13] >= 1.0F && d[16] >= 1.0F && d[17] >= 1.0F &&
           (d[0] == 2.0F || (d[14] >= 1.0F && d[15] >= 1.0F && d[18] > 0.0F));
}

AutoExposureGains autoExposureFilter(const std::vector<std::optional<AutoExposureMean>>& means,
                                     const std::vector<float>& table) noexcept {
    // The recursive attack/release filter is seeded from the window's oldest sample, so its final
    // state still carries that sample with weight p, the product of (1 - c) over the steps. When a
    // change leaves the window that weight would vanish at once and the correction would pop, so the
    // seed's share is taken out at the end. A constant input returns that constant.
    const float* const k = table.data();
    AutoExposureGains state{};
    AutoExposureGains seed{};
    AutoExposureGains weight{1.0F, 1.0F, 1.0F};
    bool have = false;
    for (const auto& mean : means) {
        if (!mean)
            continue;
        const auto d = desiredGains(k, *mean);
        if (!have) {
            state = d;
            seed = d;
            have = true;
            continue;
        }
        const float exposureCoefficient = d[0] < state[0] ? k[8] : k[9];
        const std::array<float, 3> coefficient{exposureCoefficient, k[10], k[10]};
        for (std::size_t i = 0; i < 3; ++i) {
            state[i] = state[i] + coefficient[i] * (d[i] - state[i]);
            weight[i] = weight[i] * (1.0F - coefficient[i]);
        }
    }
    AutoExposureGains result = state;
    for (std::size_t i = 0; i < 3; ++i) {
        // A lag far longer than the window (or a zero-length step) leaves the seed nearly the whole
        // state: the share cannot be separated, so the state itself is the defined result.
        if (weight[i] < kAutoExposureSeedWeightLimit)
            result[i] = (state[i] - weight[i] * seed[i]) / (1.0F - weight[i]);
    }
    return result;
}

const std::string& autoExposureProgramText() {
    // The same binary32 operations as applyAutoExposure: precise additions and multiplications, the
    // wrapper's correctly rounded division and no fused multiply-add. The wrapper makes alpha 0
    // transparent black and refuses a non-finite result.
    static const std::string text = std::string(R"GLSL(
layout(set = 0, binding = 0, std430) readonly buffer BloomAutoExposureData { vec4 values[]; } bloom_auto_exposure_data;
precise float bloom_ocio_cpu_div(float a, float b);
float bae(int i) { return bloom_auto_exposure_data.values[i >> 2][i & 3]; }
vec3 bae_desired(vec3 m) {
 float r = max(m.r, 1.0e-6);
 float g = max(m.g, 1.0e-6);
 float b = max(m.b, 1.0e-6);
 float limit = bae(7);
 precise float qr = bloom_ocio_cpu_div(g, r);
 precise float qb = bloom_ocio_cpu_div(g, b);
 float lr = min(max(log2(qr), -limit), limit);
 float lb = min(max(log2(qb), -limit), limit);
 precise float wr = lr * bae(6);
 precise float wb = lb * bae(6);
 precise float cr = r * exp2(wr);
 precise float cb = b * exp2(wb);
 precise float lumaR = 0.2126 * cr;
 precise float lumaG = 0.7152 * g;
 precise float lumaB = 0.0722 * cb;
 precise float luma = lumaR + lumaG + lumaB;
 precise float stops = bae(3) - log2(max(luma, 1.0e-6));
 precise float e = min(max(stops, bae(4)), bae(5)) * bae(2);
 return vec3(e, wr, wb);
}
vec4 bloom_auto_exposure(ivec2 c) {
 int stage = int(bae(0));
 if (stage == 0) {
  if (c.x >= int(bae(16)) || c.y >= int(bae(17))) { return vec4(0.0, 0.0, 0.0, 1.0); }
  int tw = int(bae(14));
  int th = int(bae(15));
  int x0 = c.x * tw;
  int y0 = c.y * th;
  int x1 = min(x0 + tw, int(bae(12)));
  int y1 = min(y0 + th, int(bae(13)));
  precise float sr = 0.0;
  precise float sg = 0.0;
  precise float sb = 0.0;
  for (int y = y0; y < y1; ++y) {
   for (int x = x0; x < x1; ++x) {
    vec4 p = imageLoad(bloom_ocio_input, ivec2(x, y));
    sr = sr + p.r;
    sg = sg + p.g;
    sb = sb + p.b;
   }
  }
  precise float inv = bae(18);
  precise float mr = sr * inv;
  precise float mg = sg * inv;
  precise float mb = sb * inv;
  return vec4(mr, mg, mb, 1.0);
 }
 if (stage == 1) {
  if (c.x != 0 || c.y != 0) { return imageLoad(bloom_ocio_input, c); }
  int n = int(bae(11));
  bool have = false;
  vec3 s = vec3(0.0);
  vec3 seed = vec3(0.0);
  vec3 w = vec3(1.0);
  for (int i = 0; i < n; ++i) {
   int px = int(bae(32 + 2 * i));
   int py = int(bae(33 + 2 * i));
   if (px < 0) { continue; }
   vec3 d = bae_desired(imageLoad(bloom_ocio_input, ivec2(px, py)).rgb);
   if (!have) {
    s = d;
    seed = d;
    have = true;
    continue;
   }
   float ke = d.x < s.x ? bae(8) : bae(9);
   float kb = bae(10);
   precise float de = d.x - s.x;
   precise float dr = d.y - s.y;
   precise float db = d.z - s.z;
   precise float se = ke * de;
   precise float sr2 = kb * dr;
   precise float sb2 = kb * db;
   precise float ne = s.x + se;
   precise float nr = s.y + sr2;
   precise float nb = s.z + sb2;
   s = vec3(ne, nr, nb);
   precise float oe = 1.0 - ke;
   precise float ob2 = 1.0 - kb;
   precise float we = w.x * oe;
   precise float wr = w.y * ob2;
   precise float wb = w.z * ob2;
   w = vec3(we, wr, wb);
  }
  vec3 outv = s;
  if (w.x < 0.99609375) {
   precise float sx = w.x * seed.x;
   precise float nx = s.x - sx;
   precise float dx = 1.0 - w.x;
   outv.x = bloom_ocio_cpu_div(nx, dx);
  }
  if (w.y < 0.99609375) {
   precise float sy = w.y * seed.y;
   precise float ny = s.y - sy;
   precise float dy = 1.0 - w.y;
   outv.y = bloom_ocio_cpu_div(ny, dy);
  }
  if (w.z < 0.99609375) {
   precise float sz = w.z * seed.z;
   precise float nz = s.z - sz;
   precise float dz = 1.0 - w.z;
   outv.z = bloom_ocio_cpu_div(nz, dz);
  }
  return vec4(outv, 1.0);
 }
 vec4 own = imageLoad(bloom_ocio_input, c);
 if (c.x >= int(bae(12)) || c.y >= int(bae(13)) || bae(19) == 0.0) { return own; }
 vec3 s = imageLoad(bloom_ocio_input, ivec2(int(bae(20)), int(bae(21)))).rgb;
 precise float er = s.x + s.y;
 precise float eb = s.x + s.z;
 precise float gr = exp2(er);
 precise float gg = exp2(s.x);
 precise float gb = exp2(eb);
 precise float orr = own.r * gr;
 precise float og = own.g * gg;
 precise float ob = own.b * gb;
 return vec4(orr, og, ob, own.a);
}
)GLSL");
    return text;
}

ImageEffectResult applyAutoExposure(const PreparedColorGrade& grade,
                                    const render::Rgba32fImage* const input,
                                    const std::span<const render::Rgba32fImage* const> images,
                                    const std::size_t budget, const CancellationToken& cancellation,
                                    const OperationIndex operation,
                                    const EvaluationProgressCallback& progress,
                                    CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (input == nullptr) {
        ImageEffectResult none;
        none.empty = true;
        return none;
    }
    const auto stage = autoExposureStage(grade);
    std::vector<render::Rgba32fImageView> views;
    views.reserve(images.size() + 1);
    Rows source;
    if (!rowsOf(input, views, source) || !source.present())
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Auto Exposure image is invalid");
    const float* const k = grade.data.data();
    const auto sourceWindow = input->descriptor()->dataWindow();
    const auto invalid = [](const char* const text) {
        return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                    DiagnosticSeverity::Error,
                                    {},
                                    text,
                                    {}};
    };

    // The window the pass publishes, anchored at the source's origin.
    const auto size = autoExposureOutputSize(grade, source.width, source.height);
    const auto outputWindow = render::ImageWindow::create(
        sourceWindow.originX(), sourceWindow.originY(), static_cast<std::uint64_t>(size[0]),
        static_cast<std::uint64_t>(size[1]));
    if (!outputWindow)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Auto Exposure output window is invalid");
    const auto descriptor = descriptorOver(*outputWindow.value(), *input->descriptor());
    if (!descriptor)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Auto Exposure output descriptor is invalid");
    auto builder = render::Rgba32fImageBuilder::create(*descriptor, budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Auto Exposure output exceeds the pixel budget");
    auto& image = *builder.value();

    if (stage == AutoExposureStage::Filter) {
        std::vector<std::optional<AutoExposureMean>> means;
        means.reserve(images.size() + 1);
        for (const auto* const older : images) {
            if (older == nullptr) {
                means.emplace_back();
                continue;
            }
            Rows rows;
            if (!rowsOf(older, views, rows) || !rows.present())
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Auto Exposure image is invalid");
            const auto& p = rows.at(rows.originX, rows.originY);
            means.push_back(AutoExposureMean{p.red(), p.green(), p.blue()});
        }
        const auto& newest = source.at(source.originX, source.originY);
        means.push_back(AutoExposureMean{newest.red(), newest.green(), newest.blue()});
        const auto gains = autoExposureFilter(means, grade.data);
        auto row = image.row(sourceWindow.originY());
        const auto pixel = render::Rgba32f::fromPremultiplied(gains[0], gains[1], gains[2], 1.0F);
        if (!row || !pixel)
            return failed(EvaluationDiagnosticCode::InvalidPixel,
                          "Auto Exposure produced an invalid gain");
        (*row.value())[0] = *pixel.value();
    } else {
        Rows gain;
        if (stage == AutoExposureStage::Apply && !images.empty() &&
            (!rowsOf(images[0], views, gain)))
            return failed(EvaluationDiagnosticCode::InvalidPixel, "Auto Exposure image is invalid");
        float gainRed = 1.0F;
        float gainGreen = 1.0F;
        float gainBlue = 1.0F;
        if (gain.present()) {
            const auto& s = gain.at(gain.originX, gain.originY);
            gainRed = detail::autoExposureExp2(s.red() + s.green());
            gainGreen = detail::autoExposureExp2(s.red());
            gainBlue = detail::autoExposureExp2(s.red() + s.blue());
        }
        const auto geometry = autoExposureGrid(source.width, source.height,
                                               static_cast<std::int64_t>(k[1]));
        const float inverseArea =
            reductionScale(static_cast<std::int64_t>(k[1]), source.width, source.height);
        const auto total = static_cast<std::uint32_t>(size[1]);
        reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
        const auto outcome = runRowBandPass(
            rowBands, cancellation, total, sourceWindow.originY(),
            [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
                auto row = image.row(y);
                if (!row)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Auto Exposure row is unavailable",
                                                {}};
                const auto j = y - sourceWindow.originY();
                for (std::int64_t i = 0; i < size[0]; ++i) {
                    if ((i & 255) == 0 && cancellation.isCancellationRequested())
                        return std::nullopt;
                    std::array<float, 4> out{};
                    if (stage == AutoExposureStage::Reduce) {
                        const auto x0 = i * geometry.tileWidth;
                        const auto y0 = j * geometry.tileHeight;
                        const auto x1 = std::min(x0 + geometry.tileWidth, source.width);
                        const auto y1 = std::min(y0 + geometry.tileHeight, source.height);
                        float sr = 0.0F;
                        float sg = 0.0F;
                        float sb = 0.0F;
                        for (auto yy = y0; yy < y1; ++yy)
                            for (auto xx = x0; xx < x1; ++xx) {
                                const auto& p =
                                    source.at(source.originX + xx, source.originY + yy);
                                sr = sr + p.red();
                                sg = sg + p.green();
                                sb = sb + p.blue();
                            }
                        out = {sr * inverseArea, sg * inverseArea, sb * inverseArea, 1.0F};
                    } else {
                        const auto& p = source.at(source.originX + i, source.originY + j);
                        out = {p.red() * gainRed, p.green() * gainGreen, p.blue() * gainBlue,
                               p.alpha()};
                    }
                    const auto pixel =
                        render::Rgba32f::fromPremultiplied(out[0], out[1], out[2], out[3]);
                    if (!pixel)
                        return invalid("Auto Exposure produced an invalid pixel");
                    (*row.value())[static_cast<std::size_t>(i)] = *pixel.value();
                }
                return std::nullopt;
            });
        if (outcome.cancelled || cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        if (outcome.incomplete || outcome.failure) {
            ImageEffectResult result;
            result.diagnostic = outcome.failure ? *outcome.failure
                                                : EvaluationDiagnostic{
                                                      EvaluationDiagnosticCode::AllocationFailure,
                                                      DiagnosticSeverity::Error,
                                                      {},
                                                      "An Auto Exposure row band could not complete",
                                                      {}};
            return result;
        }
        reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    }
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Auto Exposure publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
