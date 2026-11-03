// Tests for SampleConvertV1 (#422): decoded samples uploaded in their native layout and converted
// to the premultiplied RGBA32F working image on the device.
//
// The oracle is the CPU reference bloom::render::convertNativeSamples. Every layout (8-bit 4:2:0,
// 16-bit 4:4:4:4 YUV, 16-bit RGBA, binary16 RGBA) runs with its alpha associations, the stored-
// samples encoding, limited and full range and the BT.709, BT.601 and BT.2020 matrices, over odd
// extents, padded strides, a nonzero data-window origin and a multi-workgroup frame. The
// config-managed route (no inverse transfer) must equal the reference bit for bit; the legacy
// pow() transfer curves are held to the documented 2e-6 tolerance. The exact division the kernel
// uses is also checked against IEEE division on the host. Local mode pins the explicit loader and
// requires a device; a hardware-free image prints an explicit skip for the device part.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/native_samples.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace bloom::render;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

// The matrix coefficients exactly as color::yuvToSceneLinear evaluates them.
struct Matrix final {
    float redFromCr, greenFromCb, greenFromCr, blueFromCb;
};
[[nodiscard]] Matrix matrixFor(const float kr, const float kb) {
    const auto kg = 1.0F - kr - kb;
    return {2.0F * (1.0F - kr), 2.0F * kb * (1.0F - kb) / kg, 2.0F * kr * (1.0F - kr) / kg,
            2.0F * (1.0F - kb)};
}
const Matrix kRec709{1.5748F, 0.18732427F, 0.46812427F, 1.8556F};

// Owns the plane bytes of one fixture.
struct Fixture final {
    std::vector<std::vector<std::byte>> planes;
    std::shared_ptr<const NativeSampleImage> samples;
};

[[nodiscard]] std::uint32_t ceilHalf(const std::uint32_t value) { return value / 2U + (value & 1U); }

// A plane of pseudo-random bytes with every row padded by `pad` bytes; 16-bit samples can be
// constrained to a code range, and the first samples pin the range ends.
[[nodiscard]] std::vector<std::byte> planeBytes(std::mt19937& random, const std::uint32_t width,
                                                const std::uint32_t height,
                                                const std::uint32_t sampleBytes,
                                                const std::uint32_t stride) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(stride) * height);
    for (auto& byte : bytes)
        byte = static_cast<std::byte>(random() & 0xFFU);
    // Range ends at the start of the first row: all zeros, then all ones.
    const auto pinned = std::min<std::size_t>(static_cast<std::size_t>(width) * sampleBytes, 16U);
    for (std::size_t index = 0; index < pinned / 2U; ++index)
        bytes[index] = std::byte{0};
    for (std::size_t index = pinned / 2U; index < pinned; ++index)
        bytes[index] = std::byte{0xFF};
    return bytes;
}

// A binary16 plane of finite, NaN, Inf, subnormal, negative and out-of-range alpha values.
[[nodiscard]] std::vector<std::byte> halfPlane(std::mt19937& random, const std::uint32_t width,
                                               const std::uint32_t height,
                                               const std::uint32_t stride) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(stride) * height);
    const std::array<std::uint16_t, 12> specials{0x0000, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0x0001,
                                                 0x03FF, 0x3C00, 0xBC00, 0x4000, 0x7BFF, 0x3800};
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width * 4U; ++x) {
            const auto pick = random() % 16U;
            std::uint16_t value = 0;
            if (pick < specials.size())
                value = specials[pick];
            else
                value = static_cast<std::uint16_t>(random() & 0x7BFFU); // finite, positive
            const auto offset = static_cast<std::size_t>(y) * stride + x * 2U;
            bytes[offset] = static_cast<std::byte>(value & 0xFFU);
            bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
        }
    return bytes;
}

[[nodiscard]] std::optional<Fixture> makeFixture(const NativeSampleConversion& conversion,
                                                 const std::uint32_t width,
                                                 const std::uint32_t height,
                                                 const std::int64_t originX,
                                                 const std::int64_t originY,
                                                 const std::uint32_t seed) {
    std::mt19937 random(seed);
    Fixture fixture;
    std::vector<NativeSamplePlane> planes;
    const auto addPlane = [&](std::vector<std::byte> bytes, const std::uint32_t planeWidth,
                              const std::uint32_t planeHeight, const std::uint32_t stride) {
        fixture.planes.push_back(std::move(bytes));
        planes.push_back({std::span<const std::byte>(fixture.planes.back()), planeWidth,
                          planeHeight, stride});
    };
    fixture.planes.reserve(4);
    switch (conversion.layout) {
    case NativeSampleLayout::Yuv420p8: {
        const auto cw = ceilHalf(width), ch = ceilHalf(height);
        addPlane(planeBytes(random, width, height, 1, width + 3), width, height, width + 3);
        addPlane(planeBytes(random, cw, ch, 1, cw + 1), cw, ch, cw + 1);
        addPlane(planeBytes(random, cw, ch, 1, cw + 5), cw, ch, cw + 5);
        break;
    }
    case NativeSampleLayout::Yuva444p16:
        for (std::uint32_t index = 0; index < 4; ++index)
            addPlane(planeBytes(random, width, height, 2, width * 2U + index * 2U), width, height,
                     width * 2U + index * 2U);
        break;
    case NativeSampleLayout::Rgba16:
        addPlane(planeBytes(random, width, height, 8, width * 8U + 8U), width, height,
                 width * 8U + 8U);
        break;
    case NativeSampleLayout::RgbaHalf:
        addPlane(halfPlane(random, width, height, width * 8U), width, height, width * 8U);
        break;
    }
    const auto data = ImageWindow::create(originX, originY, width, height);
    const auto display = ImageWindow::create(0, 0, width + 4U, height + 2U);
    const auto aspect = bloom::core::PixelAspectRatio::create(4, 3);
    if (!data || !display || !aspect)
        return std::nullopt;
    const auto descriptor = Rgba32fImageDescriptor::create(*data.value(), *display.value(), *aspect);
    if (!descriptor)
        return std::nullopt;
    // The fixture's vectors outlive the samples, so the owner is a no-op keep-alive.
    auto created = NativeSampleImage::create(*descriptor.value(), conversion, planes,
                                             std::make_shared<int>(0));
    if (!created)
        return std::nullopt;
    fixture.samples = *created.value();
    return fixture;
}

[[nodiscard]] GpuImageUploadPollResult pollUntilTerminal(GpuImageUpload& upload) {
    for (int attempt = 0; attempt < 20000; ++attempt) {
        const auto result = upload.poll();
        if (result != GpuImageUploadPollResult::Pending)
            return result;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return GpuImageUploadPollResult::Pending;
}

[[nodiscard]] bool withinTolerance(const float expected, const float actual) {
    if (std::bit_cast<std::uint32_t>(expected) == std::bit_cast<std::uint32_t>(actual))
        return true;
    const auto difference = std::fabs(expected - actual);
    return difference <= 2e-6F || difference <= 2e-6F * std::fabs(expected);
}

// Uploads `fixture` through SampleConvertV1 and compares the resident image with the CPU
// reference. `exact` demands identical bits.
void checkCase(Expectations& expectations, GpuImageUpload& upload, const Fixture& fixture,
               const bool exact, const std::string& label) {
    const auto reference = convertNativeSamples(*fixture.samples, 1ULL << 32U);
    expectations.expect(reference.hasValue(), label + ": the CPU reference converts");
    if (!reference)
        return;
    GpuImageUploadParameters parameters;
    parameters.samples = fixture.samples;
    parameters.reuseStagingWithinScene = true;
    const auto before = upload.sampleConvertedUploads();
    const auto begun = upload.begin(parameters, 1ULL << 32U);
    expectations.expect(begun.code == GpuImageUploadDiagnosticCode::None,
                        label + ": the sample upload begins: " + begun.message);
    if (begun.code != GpuImageUploadDiagnosticCode::None)
        return;
    expectations.expect(upload.sampleConvertedUploads() == before + 1,
                        label + ": the upload crossed as native samples");
    expectations.expect(pollUntilTerminal(upload) == GpuImageUploadPollResult::Ready,
                        label + ": the sample upload reaches Ready");
    const auto* const resident = upload.image();
    expectations.expect(resident != nullptr &&
                            resident->dataWindow() == fixture.samples->descriptor().dataWindow() &&
                            resident->displayWindow() ==
                                fixture.samples->descriptor().displayWindow() &&
                            resident->pixelAspect() == fixture.samples->descriptor().pixelAspect(),
                        label + ": the resident image keeps the samples' geometry");
    auto readback = upload.readback();
    const auto& expected = reference.value()->pixels();
    expectations.expect(readback.hasValue() && readback.pixels.size() == expected.size(),
                        label + ": the resident image reads back");
    if (readback.hasValue() && readback.pixels.size() == expected.size()) {
        std::size_t mismatches = 0;
        std::size_t first = expected.size();
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto e = expected[index].components();
            const auto a = readback.pixels[index].components();
            bool same = true;
            for (std::size_t c = 0; c < 4; ++c)
                same = same && (exact ? std::bit_cast<std::uint32_t>(e[c]) ==
                                            std::bit_cast<std::uint32_t>(a[c])
                                      : (c == 3 ? e[c] == a[c] : withinTolerance(e[c], a[c])));
            if (!same) {
                if (mismatches == 0)
                    first = index;
                ++mismatches;
            }
        }
        if (mismatches != 0) {
            const auto e = expected[first].components();
            const auto a = readback.pixels[first].components();
            std::cerr << label << ": first mismatch at " << first << " expected (" << e[0] << ", "
                      << e[1] << ", " << e[2] << ", " << e[3] << ") got (" << a[0] << ", " << a[1]
                      << ", " << a[2] << ", " << a[3] << ")\n";
        }
        expectations.expect(mismatches == 0, label + (exact ? ": equals the CPU reference bit for bit"
                                                            : ": is within 2e-6 of the CPU reference"));
    }
    GpuImage taken = upload.takeImage();
    expectations.expect(taken.isValid(), label + ": the resident image is taken");
}

void testExactDivide(Expectations& expectations) {
    std::mt19937 random(422);
    std::size_t mismatches = 0;
    const std::array<float, 9> divisors{219.0F, 224.0F, 255.0F, 256.0F, 65535.0F,
                                        12.92F, 4.5F,   1.055F, 1.099F};
    for (int index = 0; index < 400000; ++index) {
        float a = 0.0F;
        switch (index % 4) {
        case 0:
            a = static_cast<float>(random() % 65536U) - 128.0F;
            break;
        case 1:
            a = static_cast<float>(random() % 1048576U) / 4096.0F - 128.0F;
            break;
        case 2:
            a = std::bit_cast<float>(static_cast<std::uint32_t>(random() % 0x3F800000U));
            break;
        default:
            a = -std::bit_cast<float>(static_cast<std::uint32_t>(random() % 0x47800000U));
        }
        const float b = index % 2 == 0
                            ? divisors[static_cast<std::size_t>(index) % divisors.size()]
                            : static_cast<float>(1U + random() % 65535U) / 65535.0F;
        const float expected = a / b;
        if (std::bit_cast<std::uint32_t>(nativeSampleExactDivide(a, b)) !=
            std::bit_cast<std::uint32_t>(expected))
            ++mismatches;
    }
    expectations.expect(mismatches == 0, "the kernel's long division equals IEEE division");
    expectations.expect(std::bit_cast<std::uint32_t>(nativeSampleExactDivide(-0.0F, 219.0F)) ==
                            std::bit_cast<std::uint32_t>(-0.0F),
                        "a signed zero dividend keeps its sign");
}

void testInvalidSamples(Expectations& expectations) {
    std::vector<std::byte> bytes(64);
    const auto window = ImageWindow::create(0, 0, 4, 2);
    if (!window)
        return;
    const auto descriptor =
        Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                       bloom::core::PixelAspectRatio::square());
    if (!descriptor)
        return;
    NativeSampleConversion conversion;
    conversion.layout = NativeSampleLayout::Rgba16;
    const NativeSamplePlane short_{std::span<const std::byte>(bytes).first(32), 4, 2, 32};
    expectations.expect(!NativeSampleImage::create(*descriptor.value(), conversion,
                                                   std::span(&short_, 1), std::make_shared<int>(0)),
                        "a plane shorter than its rows is refused");
    const NativeSamplePlane narrow{std::span<const std::byte>(bytes), 3, 2, 32};
    expectations.expect(!NativeSampleImage::create(*descriptor.value(), conversion,
                                                   std::span(&narrow, 1), std::make_shared<int>(0)),
                        "a plane narrower than the data window is refused");
    conversion.layout = NativeSampleLayout::Yuv420p8;
    const NativeSamplePlane whole{std::span<const std::byte>(bytes), 4, 2, 32};
    expectations.expect(!NativeSampleImage::create(*descriptor.value(), conversion,
                                                   std::span(&whole, 1), std::make_shared<int>(0)),
                        "a 4:2:0 image without its chroma planes is refused");
}

void testDevice(Expectations& expectations, GpuImageUpload& upload) {
    struct Case final {
        std::string label;
        NativeSampleConversion conversion;
        bool exact = true;
    };
    std::vector<Case> cases;
    const auto yuv = [](const NativeSampleLayout layout, const Matrix matrix, const bool limited,
                        const std::int32_t transfer, const bool stored) {
        NativeSampleConversion conversion;
        conversion.layout = layout;
        conversion.limitedRange = limited;
        conversion.transfer = transfer;
        conversion.storedSamples = stored;
        conversion.redFromCr = matrix.redFromCr;
        conversion.greenFromCb = matrix.greenFromCb;
        conversion.greenFromCr = matrix.greenFromCr;
        conversion.blueFromCb = matrix.blueFromCb;
        return conversion;
    };
    const auto rec601 = matrixFor(0.299F, 0.114F);
    const auto rec2020 = matrixFor(0.2627F, 0.0593F);
    for (const auto layout : {NativeSampleLayout::Yuv420p8, NativeSampleLayout::Yuva444p16}) {
        const std::string name = layout == NativeSampleLayout::Yuv420p8 ? "yuv420p8" : "yuva444p16";
        cases.push_back({name + " 709 limited", yuv(layout, kRec709, true, 8, false)});
        cases.push_back({name + " 709 full", yuv(layout, kRec709, false, 8, false)});
        cases.push_back({name + " 601 limited", yuv(layout, rec601, true, 8, false)});
        cases.push_back({name + " 2020 full", yuv(layout, rec2020, false, 8, false)});
        cases.push_back({name + " 709 stored", yuv(layout, kRec709, true, 8, true)});
        cases.push_back({name + " 709 sRGB curve", yuv(layout, kRec709, true, 13, false), false});
        cases.push_back({name + " 709 BT.709 curve", yuv(layout, kRec709, true, 1, false), false});
    }
    for (const auto alpha : {NativeSampleAlpha::Straight, NativeSampleAlpha::Unpremultiply}) {
        for (const bool stored : {false, true}) {
            NativeSampleConversion conversion;
            conversion.layout = NativeSampleLayout::Rgba16;
            conversion.alpha = alpha;
            conversion.storedSamples = stored;
            cases.push_back({std::string("rgba16 ") +
                                 (alpha == NativeSampleAlpha::Straight ? "straight" : "unpremultiply") +
                                 (stored ? " stored" : ""),
                             conversion});
        }
    }
    for (const auto alpha : {NativeSampleAlpha::Straight, NativeSampleAlpha::Premultiplied}) {
        for (const bool stored : {false, true})
            for (const bool luminance : {false, true})
                for (const bool hasAlpha : {true, false}) {
                    NativeSampleConversion conversion;
                    conversion.layout = NativeSampleLayout::RgbaHalf;
                    conversion.alpha = alpha;
                    conversion.storedSamples = stored;
                    conversion.luminance = luminance;
                    conversion.hasAlpha = hasAlpha;
                    cases.push_back(
                        {std::string("half ") +
                             (alpha == NativeSampleAlpha::Straight ? "straight" : "premultiplied") +
                             (stored ? " stored" : "") + (luminance ? " Y" : " RGB") +
                             (hasAlpha ? "A" : ""),
                         conversion});
                }
    }
    std::uint32_t seed = 1;
    for (const auto& item : cases) {
        // An odd extent with a nonzero origin, then a frame that spans many workgroups.
        for (const auto& [width, height, x, y] :
             {std::array<std::int64_t, 4>{37, 23, -3, 5}, std::array<std::int64_t, 4>{1031, 67, 0, 0}}) {
            const auto fixture = makeFixture(item.conversion, static_cast<std::uint32_t>(width),
                                             static_cast<std::uint32_t>(height), x, y, seed++);
            expectations.expect(fixture.has_value(), item.label + ": the fixture builds");
            if (fixture)
                checkCase(expectations, upload, *fixture, item.exact,
                          item.label + " " + std::to_string(width) + "x" + std::to_string(height));
        }
    }

    // A float upload and a sample upload alternate on one pipeline and both stay exact.
    const auto fixture = makeFixture(cases.front().conversion, 64, 32, 0, 0, 9000);
    expectations.expect(fixture.has_value(), "the alternation fixture builds");
    if (!fixture)
        return;
    auto reference = convertNativeSamples(*fixture->samples, 1ULL << 32U);
    expectations.expect(reference.hasValue(), "the alternation reference converts");
    if (!reference)
        return;
    const auto floatSource = std::make_shared<const Rgba32fImage>(std::move(*reference.value()));
    GpuImageUploadParameters floatParameters;
    floatParameters.source = floatSource;
    floatParameters.reuseStagingWithinScene = true;
    expectations.expect(upload.begin(floatParameters, 1ULL << 32U).code ==
                                GpuImageUploadDiagnosticCode::None &&
                            pollUntilTerminal(upload) == GpuImageUploadPollResult::Ready,
                        "a float upload runs between sample uploads");
    { GpuImage taken = upload.takeImage(); }
    checkCase(expectations, upload, *fixture, true, "sample after float");
    // Cancelling a sample job retires it without publishing.
    GpuImageUploadParameters parameters;
    parameters.samples = fixture->samples;
    if (upload.begin(parameters, 1ULL << 32U).code == GpuImageUploadDiagnosticCode::None) {
        upload.cancel();
        const auto result = pollUntilTerminal(upload);
        expectations.expect(result == GpuImageUploadPollResult::Failure &&
                                upload.diagnostic().code == GpuImageUploadDiagnosticCode::Cancelled,
                            "a cancelled sample upload ends Cancelled");
        expectations.expect(!upload.hasUnretiredSubmission(),
                            "the cancelled sample upload's submission is retired");
    } else {
        expectations.expect(false, "the cancellation fixture begins");
    }
    // A budget that holds the image but not the staging refuses before allocating.
    const auto imageBytes = 64ULL * 32ULL * sizeof(Rgba32f);
    expectations.expect(upload.begin(parameters, imageBytes).code ==
                            GpuImageUploadDiagnosticCode::OverBudget,
                        "a sample upload over the byte budget is refused");
    GpuImageUploadParameters both = parameters;
    both.source = floatSource;
    expectations.expect(upload.begin(both, 1ULL << 32U).code ==
                            GpuImageUploadDiagnosticCode::InvalidArgument,
                        "an upload carrying both a float image and samples is refused");
    upload.releaseSpareStaging();
}

} // namespace

int main(const int argc, char** argv) {
    std::filesystem::path loader;
    bool requireDevice = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc)
            loader = argv[++index];
        else if (argument == "--require-device")
            requireDevice = true;
        else {
            std::cerr << "unknown argument: " << argument << '\n';
            return 2;
        }
    }
    Expectations expectations;
    testExactDivide(expectations);
    testInvalidSamples(expectations);
    GpuDeviceCreationOptions options;
    options.loader_path = loader;
    auto device = GpuDevice::create(options);
    if (!device) {
        if (requireDevice) {
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return expectations.ok() ? 0 : 1;
    }
    auto created = GpuImageUpload::create(*device.device);
    expectations.expect(created.hasValue(), "the upload pipeline is created");
    if (!created)
        return 1;
    testDevice(expectations, *created.upload);
    if (!expectations.ok()) {
        std::cerr << "FAIL: SampleConvertV1 expectations failed\n";
        return 1;
    }
    std::cout << "PASS: SampleConvertV1\n";
    return 0;
}
