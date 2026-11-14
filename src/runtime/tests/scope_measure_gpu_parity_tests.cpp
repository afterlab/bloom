// #724 Scopes: the CPU reference (measureScopesCpu) and the GPU kernel (ScopeTablesV1) give equal
// tables, word for word, for the same float pictures: every scope, both signals' axis rules (the
// Display signal's maximum of 1 and a Working signal's raised maximum), transparent pixels counted
// and ignored, and an area that reaches outside the picture. 256 x 256, a real device. Without a
// device the test is skipped (exit 77); with --require-device that is a failure.

#include "gpu_composite_native_support.hpp"

#include <bloom/render/gpu_scope_tables.hpp>
#include <bloom/runtime/scope_measure.hpp>

#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::render::GpuScopeTables;
using bloom::render::GpuScopeTablesRequest;
using bloom::render::GpuScopeTablesState;
using bloom::runtime::measureScopesCpu;
using bloom::runtime::ScopeLuma;
using bloom::runtime::ScopeMeasureInput;

constexpr std::uint32_t kSide = 256;

// A fixed sequence of values, the same on every platform.
class Sequence final {
  public:
    [[nodiscard]] float next(const float low, const float high) noexcept {
        state_ = state_ * 1664525U + 1013904223U;
        const float unit = static_cast<float>(state_ >> 8U) * (1.0F / 16777216.0F);
        return low + unit * (high - low);
    }

  private:
    std::uint32_t state_ = 0x9E3779B9U;
};

struct Pictures final {
    std::vector<Rgba32f> signal;
    std::vector<Rgba32f> xyz;
    std::shared_ptr<const GpuImage> residentSignal;
    std::shared_ptr<const GpuImage> residentXyz;
};

[[nodiscard]] std::shared_ptr<const GpuImage> resident(GpuImageUpload& uploader,
                                                       const std::vector<Rgba32f>& pixels) {
    const auto imageWindow = window(0, 0, kSide, kSide);
    auto host = makeImage(imageWindow, imageWindow, PixelAspectRatio::square(), pixels);
    if (!host) {
        return nullptr;
    }
    auto image = upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*host)));
    return image ? std::make_shared<const GpuImage>(std::move(*image)) : nullptr;
}

// Values below, inside and above each axis, three kinds of alpha, and XYZ pixels with no
// chromaticity and outside the table. `scale` raises the picture for a Working signal.
[[nodiscard]] Pictures makePictures(GpuImageUpload& uploader, const float scale) {
    Pictures pictures;
    Sequence sequence;
    for (std::uint32_t index = 0; index < kSide * kSide; ++index) {
        const float selector = sequence.next(0.0F, 1.0F);
        const float alpha = selector < 0.1F ? 0.0F : (selector < 0.6F ? 1.0F : selector);
        pictures.signal.push_back(pixel(sequence.next(-0.25F, 1.5F) * scale,
                                        sequence.next(-0.25F, 1.5F) * scale,
                                        sequence.next(-0.25F, 1.5F) * scale, alpha));
        const float low = sequence.next(0.0F, 1.0F) < 0.15F ? -1.0F : 0.0F;
        pictures.xyz.push_back(pixel(sequence.next(low, 1.2F), sequence.next(low, 1.2F),
                                     sequence.next(low, 1.2F), 1.0F));
    }
    pictures.residentSignal = resident(uploader, pictures.signal);
    pictures.residentXyz = resident(uploader, pictures.xyz);
    return pictures;
}

// `value` moved by `steps` representable numbers (0 stays 0).
[[nodiscard]] float nudged(const float value, const std::int32_t steps) noexcept {
    if (value == 0.0F) {
        return value;
    }
    const auto bits = static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(value));
    return std::bit_cast<float>(static_cast<std::uint32_t>(bits + steps));
}

// Pixels that sit on, and a few representable numbers either side of, the edges between cells of
// the Vectorscope and Chromaticity tables: where a quotient that is one unit in the last place off
// changes the cell. A random picture reaches such a pixel about once in 65,536.
[[nodiscard]] Pictures makeEdgePictures(GpuImageUpload& uploader) {
    Pictures pictures;
    Sequence sequence;
    for (std::uint32_t index = 0; index < kSide * kSide; ++index) {
        const std::uint32_t edge = index % 512U;
        const auto steps = static_cast<std::int32_t>((index / 512U) % 7U) - 3;
        // With one channel alone, cb (or cr) is that channel times a constant; this value lands
        // near the edge for coefficients near Rec. 709's and exercises the neighbors otherwise.
        const float axis = nudged((static_cast<float>(edge) / 512.0F - 0.5F) * 2.0F, steps);
        const bool blue = (index / 3584U) % 2U == 0U;
        pictures.signal.push_back(pixel(blue ? 0.0F : axis, 0.0F, blue ? axis : 0.0F, 1.0F));
        const float sum = sequence.next(0.1F, 3.0F);
        const float x = nudged(static_cast<float>(edge) / 512.0F * 0.8F * sum, steps);
        const float y =
            nudged(static_cast<float>((edge * 7U + 3U) % 512U) / 512.0F * 0.9F * sum, -steps);
        pictures.xyz.push_back(pixel(x, y, sum - x - y, 1.0F));
    }
    pictures.residentSignal = resident(uploader, pictures.signal);
    pictures.residentXyz = resident(uploader, pictures.xyz);
    return pictures;
}

struct Case final {
    std::string label;
    std::uint32_t scopes = bloom::render::kScopeTableAll;
    std::uint32_t axisMaximum = 1;
    std::int32_t areaX = 0;
    std::int32_t areaY = 0;
    std::uint32_t areaWidth = kSide;
    std::uint32_t areaHeight = kSide;
    bool ignoreTransparent = false;
    std::uint32_t detail = 0;
};

void compare(Expectations& expectations, GpuScopeTables& measure, const Pictures& pictures,
             const ScopeLuma& luma, const Case& test) {
    ScopeMeasureInput input;
    input.signal = pictures.signal;
    input.xyz = pictures.xyz;
    input.width = kSide;
    input.height = kSide;
    input.areaX = test.areaX;
    input.areaY = test.areaY;
    input.areaWidth = test.areaWidth;
    input.areaHeight = test.areaHeight;
    input.scopes = test.scopes;
    input.axisMaximum = test.axisMaximum;
    input.luma = luma;
    input.ignoreTransparent = test.ignoreTransparent;
    input.detail = test.detail;
    const auto reference = measureScopesCpu(input);
    expectations.expect(reference.has_value(), test.label + ": the CPU reference measures");
    if (!reference) {
        return;
    }

    GpuScopeTablesRequest request;
    request.signal = pictures.residentSignal;
    request.xyz = pictures.residentXyz;
    request.scopes = test.scopes;
    request.areaX = test.areaX;
    request.areaY = test.areaY;
    request.areaWidth = test.areaWidth;
    request.areaHeight = test.areaHeight;
    request.inverseAxisMaximum = 1.0F / static_cast<float>(test.axisMaximum);
    request.lumaRed = luma.red;
    request.lumaGreen = luma.green;
    request.lumaBlue = luma.blue;
    request.cbDenominator = luma.cbDenominator;
    request.crDenominator = luma.crDenominator;
    request.ignoreTransparent = test.ignoreTransparent;
    request.detail = test.detail;
    expectations.expect(measure.begin(request, kBudget),
                        test.label +
                            ": the device measure begins: " + measure.diagnostic().message);
    GpuScopeTablesState state = measure.state();
    while (state == GpuScopeTablesState::Pending) {
        state = measure.poll();
    }
    expectations.expect(state == GpuScopeTablesState::Ready,
                        test.label +
                            ": the device measure is Ready: " + measure.diagnostic().message);
    if (state != GpuScopeTablesState::Ready) {
        return;
    }
    const auto device = measure.take();
    expectations.expect(device.layout == reference->layout, test.label + ": the layouts agree");
    std::size_t differences = 0;
    std::size_t first = 0;
    if (device.words.size() == reference->words.size()) {
        for (std::size_t index = 0; index < device.words.size(); ++index) {
            if (device.words[index] != reference->words[index] && differences++ == 0) {
                first = index;
            }
        }
    }
    expectations.expect(device.words == reference->words,
                        test.label + ": the CPU and GPU tables are equal (" +
                            std::to_string(differences) + " words differ; first at " +
                            std::to_string(first) + ": GPU " +
                            std::to_string(first < device.words.size() ? device.words[first] : 0U) +
                            ", CPU " + std::to_string(reference->words[first]) + ")");
    expectations.expect(reference->counted() != 0U, test.label + ": pixels were counted");
}

void testParity(Expectations& expectations, GpuDevice& device) {
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(uploader.hasValue(), "the uploader is created");
    if (!uploader) {
        return;
    }
    const Pictures display = makePictures(*uploader.upload, 1.0F);
    const Pictures working = makePictures(*uploader.upload, 6.0F);
    const Pictures edges = makeEdgePictures(*uploader.upload);
    expectations.expect(edges.residentSignal != nullptr && edges.residentXyz != nullptr,
                        "the edge pictures are resident");
    if (edges.residentSignal == nullptr || edges.residentXyz == nullptr) {
        return;
    }
    expectations.expect(display.residentSignal != nullptr && display.residentXyz != nullptr &&
                            working.residentSignal != nullptr && working.residentXyz != nullptr,
                        "the pictures are resident");
    if (display.residentSignal == nullptr || display.residentXyz == nullptr ||
        working.residentSignal == nullptr || working.residentXyz == nullptr) {
        return;
    }
    const ScopeLuma rec709 = bloom::runtime::scopeLuma(0.2126F, 0.7152F, 0.0722F);
    // The coefficients OCIO gives for a wide working space, so the arithmetic is not only
    // exercised with Rec. 709's.
    const ScopeLuma wide = bloom::runtime::scopeLuma(0.2677F, 0.6743F, 0.0580F);
    GpuScopeTables measure;

    compare(expectations, measure, display, rec709, {.label = "display, all scopes"});
    compare(expectations, measure, display, rec709,
            {.label = "display, transparent ignored", .ignoreTransparent = true});
    compare(expectations, measure, display, rec709,
            {.label = "display, area past the picture",
             .areaX = -9,
             .areaY = 31,
             .areaWidth = 300,
             .areaHeight = 240});
    for (const std::uint32_t scope :
         {bloom::render::kScopeTableHistogram, bloom::render::kScopeTableWaveform,
          bloom::render::kScopeTableVectorscope, bloom::render::kScopeTableChromaticity}) {
        compare(expectations, measure, display, rec709,
                {.label = "display, scope " + std::to_string(scope), .scopes = scope});
    }
    compare(expectations, measure, edges, rec709, {.label = "cell edges, Rec. 709"});
    compare(expectations, measure, edges, wide, {.label = "cell edges, wide space"});
    for (const std::uint32_t maximum : {1U, 2U, 4U, 8U, 16U}) {
        compare(expectations, measure, working, wide,
                {.label = "working, maximum " + std::to_string(maximum), .axisMaximum = maximum});
    }
    compare(expectations, measure, working, wide,
            {.label = "working, narrow region",
             .axisMaximum = 8,
             .areaX = 40,
             .areaY = 12,
             .areaWidth = 77,
             .areaHeight = 130,
             .ignoreTransparent = true});

    // The coarser details: the same pictures, the CPU reference and the device equal cell for cell.
    for (const std::uint32_t detail : {1U, 2U}) {
        const std::string suffix = ", detail " + std::to_string(detail);
        compare(expectations, measure, display, rec709,
                {.label = "display, all scopes" + suffix, .detail = detail});
        compare(expectations, measure, display, rec709,
                {.label = "display, area past the picture" + suffix,
                 .areaX = -9,
                 .areaY = 31,
                 .areaWidth = 300,
                 .areaHeight = 240,
                 .detail = detail});
        compare(expectations, measure, edges, rec709,
                {.label = "cell edges, Rec. 709" + suffix, .detail = detail});
        compare(expectations, measure, edges, wide,
                {.label = "cell edges, wide space" + suffix, .detail = detail});
        compare(expectations, measure, working, wide,
                {.label = "working, narrow region" + suffix,
                 .axisMaximum = 8,
                 .areaX = 40,
                 .areaY = 12,
                 .areaWidth = 77,
                 .areaHeight = 130,
                 .ignoreTransparent = true,
                 .detail = detail});
        for (const std::uint32_t scope :
             {bloom::render::kScopeTableWaveform, bloom::render::kScopeTableVectorscope,
              bloom::render::kScopeTableChromaticity}) {
            compare(expectations, measure, display, rec709,
                    {.label = "display, scope " + std::to_string(scope) + suffix,
                     .scopes = scope,
                     .detail = detail});
        }
    }
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
            return 77;
        }
        testParity(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " scope parity expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: scope measure CPU/GPU parity\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
