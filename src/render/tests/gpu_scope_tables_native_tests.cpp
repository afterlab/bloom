// Native proof of the ScopeTablesV1 measure (#724): the count tables the device returns are equal,
// word for word, to counts made here from the same float pictures with the formulas of
// docs/architecture/scopes.md. The pictures are small (256 x 256): a flat field, where every pixel
// adds to the same cell of every table at once (the hardest case for the atomic adds); a varied
// field with values under, inside and over every range; an area that reaches outside the picture
// with transparent pixels ignored; and a narrow area with a raised axis maximum.
//
// The counts here are computed independently of the CPU reference in src/runtime, which has its
// own parity test against this kernel.

#include "gpu_composite_native_support.hpp"

#include <bloom/render/gpu_scope_tables.hpp>
#include <bloom/render/scope_table_layout.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::render::GpuScopeTables;
using bloom::render::GpuScopeTablesCode;
using bloom::render::GpuScopeTablesRequest;
using bloom::render::GpuScopeTablesResult;
using bloom::render::GpuScopeTablesState;
using bloom::render::ScopeTableLayout;

constexpr std::uint32_t kSide = 256;

struct Picture final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Rgba32f> signal;
    std::vector<Rgba32f> xyz;
};

[[nodiscard]] bool finite(const float value) noexcept {
    return (std::bit_cast<std::uint32_t>(value) & 0x7F800000U) != 0x7F800000U;
}

[[nodiscard]] std::uint32_t code(const float value, const float inverseMaximum) noexcept {
    const float scaled = value * inverseMaximum;
    const float coded = scaled * 1023.0F;
    const float rounded = coded + 0.5F;
    const float whole = std::floor(rounded);
    if (!(whole >= 0.0F)) {
        return bloom::render::kScopeCodeUnder;
    }
    if (!(whole <= 1023.0F)) {
        return bloom::render::kScopeCodeOver;
    }
    return static_cast<std::uint32_t>(whole);
}

[[nodiscard]] std::uint32_t cell(const float value) noexcept {
    const float scaled = value * 512.0F;
    const float whole = std::floor(scaled);
    if (!(whole >= 0.0F) || !(whole <= 511.0F)) {
        return 512U;
    }
    return static_cast<std::uint32_t>(whole);
}

[[nodiscard]] std::uint32_t level(const std::uint32_t value) noexcept {
    return value < 1024U ? value >> 1U : value - 512U;
}

// The tables of `request` over `picture`, counted one pixel at a time.
[[nodiscard]] std::vector<std::uint32_t> countTables(const Picture& picture,
                                                     const GpuScopeTablesRequest& request,
                                                     const ScopeTableLayout& layout) {
    std::vector<std::uint32_t> words(layout.wordCount, 0U);
    for (std::uint32_t y = 0; y < request.areaHeight; ++y) {
        for (std::uint32_t x = 0; x < request.areaWidth; ++x) {
            const std::int64_t px = static_cast<std::int64_t>(request.areaX) + x;
            const std::int64_t py = static_cast<std::int64_t>(request.areaY) + y;
            const bool inside = px >= 0 && py >= 0 && px < picture.width && py < picture.height;
            const std::size_t index =
                inside ? static_cast<std::size_t>(py) * picture.width + static_cast<std::size_t>(px)
                       : 0U;
            const Rgba32f signal = inside ? picture.signal[index] : Rgba32f::transparent();
            const float r = signal.red();
            const float g = signal.green();
            const float b = signal.blue();
            const float a = signal.alpha();
            if (!finite(r) || !finite(g) || !finite(b) || !finite(a)) {
                ++words[bloom::render::kScopeCounterNotFinite];
                continue;
            }
            if (request.ignoreTransparent && a == 0.0F) {
                continue;
            }
            ++words[bloom::render::kScopeCounterCounted];
            const float lumaRed = request.lumaRed * r;
            const float lumaGreen = request.lumaGreen * g;
            const float lumaBlue = request.lumaBlue * b;
            const float lumaRedGreen = lumaRed + lumaGreen;
            const float luma = lumaRedGreen + lumaBlue;
            const std::uint32_t codes[4] = {
                code(r, request.inverseAxisMaximum), code(g, request.inverseAxisMaximum),
                code(b, request.inverseAxisMaximum), code(luma, request.inverseAxisMaximum)};
            if (layout.has(bloom::render::kScopeTableHistogram)) {
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    ++words[layout.histogramOffset +
                            channel * bloom::render::kScopeHistogramChannelWords + codes[channel]];
                }
            }
            if (layout.has(bloom::render::kScopeTableWaveform)) {
                const std::uint32_t column = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(x) * layout.columns / request.areaWidth);
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    ++words[layout.waveformOffset + channel * layout.waveformChannelWords() +
                            column * bloom::render::kScopeWaveformColumnWords +
                            level(codes[channel])];
                }
            }
            if (layout.has(bloom::render::kScopeTableVectorscope)) {
                const float cbDifference = b - luma;
                const float cb = cbDifference / request.cbDenominator;
                const float cbShifted = cb + 0.5F;
                const float crDifference = r - luma;
                const float cr = crDifference / request.crDenominator;
                const float crShifted = cr + 0.5F;
                const std::uint32_t column = cell(cbShifted);
                const std::uint32_t row = cell(crShifted);
                if (column == 512U || row == 512U) {
                    ++words[bloom::render::kScopeCounterVectorscopeOutside];
                } else {
                    ++words[layout.vectorscopeOffset + row * 512U + column];
                }
            }
            if (layout.has(bloom::render::kScopeTableChromaticity)) {
                const Rgba32f xyz = inside ? picture.xyz[index] : Rgba32f::transparent();
                const float xy = xyz.red() + xyz.green();
                const float sum = xy + xyz.blue();
                if (!finite(xyz.red()) || !finite(xyz.green()) || !finite(xyz.blue()) ||
                    !(sum > 0.0F)) {
                    ++words[bloom::render::kScopeCounterNoChromaticity];
                } else {
                    const float cx = xyz.red() / sum;
                    const float cxScaled = cx / std::bit_cast<float>(0x3F4CCCCDU);
                    const float cy = xyz.green() / sum;
                    const float cyScaled = cy / std::bit_cast<float>(0x3F666666U);
                    const std::uint32_t column = cell(cxScaled);
                    const std::uint32_t row = cell(cyScaled);
                    if (column == 512U || row == 512U) {
                        ++words[bloom::render::kScopeCounterChromaticityOutside];
                    } else {
                        ++words[layout.chromaticityOffset + row * 512U + column];
                    }
                }
            }
        }
    }
    return words;
}

// A fixed sequence of values, the same on every platform.
class Sequence final {
  public:
    [[nodiscard]] float next(const float low, const float high) noexcept {
        state_ = state_ * 1664525U + 1013904223U;
        const float unit = static_cast<float>(state_ >> 8U) * (1.0F / 16777216.0F);
        return low + unit * (high - low);
    }

  private:
    std::uint32_t state_ = 0x724U;
};

[[nodiscard]] Picture flatPicture() {
    Picture picture;
    picture.width = kSide;
    picture.height = kSide;
    picture.signal.assign(static_cast<std::size_t>(kSide) * kSide, pixel(0.75F, 0.25F, 0.5F, 1.0F));
    picture.xyz.assign(static_cast<std::size_t>(kSide) * kSide, pixel(0.4F, 0.3F, 0.2F, 1.0F));
    return picture;
}

[[nodiscard]] Picture variedPicture() {
    Picture picture;
    picture.width = kSide;
    picture.height = kSide;
    Sequence sequence;
    for (std::uint32_t index = 0; index < kSide * kSide; ++index) {
        const float selector = sequence.next(0.0F, 1.0F);
        const float alpha = selector < 0.1F ? 0.0F : (selector < 0.6F ? 1.0F : selector);
        picture.signal.push_back(pixel(sequence.next(-0.25F, 1.5F), sequence.next(-0.25F, 1.5F),
                                       sequence.next(-0.25F, 1.5F), alpha));
        // Some pixels have no chromaticity (a sum at or below zero) and some fall outside the
        // table.
        const float fold = sequence.next(0.0F, 1.0F);
        const float low = fold < 0.15F ? -1.0F : 0.0F;
        picture.xyz.push_back(pixel(sequence.next(low, 1.2F), sequence.next(low, 1.2F),
                                    sequence.next(low, 1.2F), 1.0F));
    }
    return picture;
}

[[nodiscard]] std::shared_ptr<const GpuImage> resident(GpuImageUpload& uploader,
                                                       const std::vector<Rgba32f>& pixels,
                                                       const std::uint32_t width,
                                                       const std::uint32_t height) {
    const auto imageWindow = window(0, 0, width, height);
    auto host = makeImage(imageWindow, imageWindow, PixelAspectRatio::square(), pixels);
    if (!host) {
        return nullptr;
    }
    auto image = upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*host)));
    return image ? std::make_shared<const GpuImage>(std::move(*image)) : nullptr;
}

[[nodiscard]] GpuScopeTablesRequest rec709Request() {
    GpuScopeTablesRequest request;
    request.scopes = bloom::render::kScopeTableAll;
    request.areaWidth = kSide;
    request.areaHeight = kSide;
    request.lumaRed = 0.2126F;
    request.lumaGreen = 0.7152F;
    request.lumaBlue = 0.0722F;
    request.cbDenominator = 2.0F * (1.0F - request.lumaBlue);
    request.crDenominator = 2.0F * (1.0F - request.lumaRed);
    return request;
}

void compare(Expectations& expectations, GpuScopeTables& measure, const Picture& picture,
             const GpuScopeTablesRequest& request, const std::string& label) {
    const std::uint64_t before = GpuScopeTables::nativeDispatchCount();
    expectations.expect(measure.begin(request, kBudget),
                        label + ": begin accepted: " + measure.diagnostic().message);
    GpuScopeTablesState state = measure.state();
    while (state == GpuScopeTablesState::Pending) {
        state = measure.poll();
    }
    expectations.expect(state == GpuScopeTablesState::Ready,
                        label + ": the measure is Ready: " + measure.diagnostic().message);
    if (state != GpuScopeTablesState::Ready) {
        return;
    }
    const GpuScopeTablesResult result = measure.take();
    const ScopeTableLayout layout =
        bloom::render::scopeTableLayout(request.scopes, request.areaWidth);
    expectations.expect(result.layout == layout, label + ": the layout is the request's");
    const std::uint64_t dispatches = layout.columns != 0 ? 2U : 1U;
    expectations.expect(GpuScopeTables::nativeDispatchCount() == before + dispatches,
                        label + ": the passes were dispatched on the device");
    const std::vector<std::uint32_t> expected = countTables(picture, request, layout);
    expectations.expect(result.words.size() == expected.size(), label + ": the word count agrees");
    if (result.words.size() != expected.size()) {
        return;
    }
    std::size_t differences = 0;
    std::size_t first = 0;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (result.words[index] != expected[index]) {
            if (differences == 0) {
                first = index;
            }
            ++differences;
        }
    }
    expectations.expect(differences == 0,
                        label + ": every count is equal (" + std::to_string(differences) +
                            " differ; first at word " + std::to_string(first) + ": device " +
                            std::to_string(result.words[first]) + ", expected " +
                            std::to_string(expected[first]) + ")");
    std::uint64_t pixels = 0;
    for (std::uint32_t y = 0; y < request.areaHeight; ++y) {
        pixels += request.areaWidth;
    }
    expectations.expect(result.words[bloom::render::kScopeCounterCounted] +
                                result.words[bloom::render::kScopeCounterNotFinite] <=
                            pixels,
                        label + ": no more pixels are counted than the area holds");
}

void testTables(Expectations& expectations, GpuDevice& device) {
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(uploader.hasValue(), "the uploader is created");
    if (!uploader) {
        return;
    }
    GpuScopeTables measure;

    {
        const Picture picture = flatPicture();
        GpuScopeTablesRequest request = rec709Request();
        request.signal = resident(*uploader.upload, picture.signal, kSide, kSide);
        request.xyz = resident(*uploader.upload, picture.xyz, kSide, kSide);
        expectations.expect(request.signal != nullptr && request.xyz != nullptr,
                            "flat: the pictures are resident");
        if (request.signal != nullptr && request.xyz != nullptr) {
            compare(expectations, measure, picture, request, "flat field");
        }
    }

    const Picture picture = variedPicture();
    GpuScopeTablesRequest request = rec709Request();
    request.signal = resident(*uploader.upload, picture.signal, kSide, kSide);
    request.xyz = resident(*uploader.upload, picture.xyz, kSide, kSide);
    expectations.expect(request.signal != nullptr && request.xyz != nullptr,
                        "varied: the pictures are resident");
    if (request.signal == nullptr || request.xyz == nullptr) {
        return;
    }
    compare(expectations, measure, picture, request, "varied field");

    GpuScopeTablesRequest outside = request;
    outside.areaX = -8;
    outside.areaY = -8;
    outside.areaWidth = 200;
    outside.areaHeight = 300;
    compare(expectations, measure, picture, outside, "area past the picture");
    outside.ignoreTransparent = true;
    compare(expectations, measure, picture, outside, "area past the picture, transparent ignored");

    GpuScopeTablesRequest narrow = request;
    narrow.areaX = 17;
    narrow.areaY = 5;
    narrow.areaWidth = 100;
    narrow.areaHeight = 60;
    narrow.inverseAxisMaximum = 0.25F;
    compare(expectations, measure, picture, narrow, "narrow area, axis maximum 4");

    for (const std::uint32_t scope :
         {bloom::render::kScopeTableHistogram, bloom::render::kScopeTableWaveform,
          bloom::render::kScopeTableVectorscope, bloom::render::kScopeTableChromaticity}) {
        GpuScopeTablesRequest single = request;
        single.scopes = scope;
        if (scope != bloom::render::kScopeTableChromaticity) {
            single.xyz.reset();
        }
        compare(expectations, measure, picture, single, "single table " + std::to_string(scope));
    }

    GpuScopeTablesRequest missing = request;
    missing.xyz.reset();
    expectations.expect(!measure.begin(missing, kBudget) &&
                            measure.diagnostic().code == GpuScopeTablesCode::InvalidArgument,
                        "the Chromaticity table without an XYZ picture is refused");
    expectations.expect(!measure.begin(request, 1024) &&
                            measure.diagnostic().code == GpuScopeTablesCode::OverBudget,
                        "a budget below the tables is refused");
    compare(expectations, measure, picture, request, "varied field after two refusals");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 0;
        }
        testTables(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " scope table expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU scope tables\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
