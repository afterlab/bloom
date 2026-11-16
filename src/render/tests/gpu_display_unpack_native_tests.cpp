// Tests for DisplayUnpackV1 (#733): a stored display frame uploaded as its 4-byte cells and
// unpacked on the device into the float signal picture a scope measure counts.
//
// The oracle is bloom::render::displaySignalPixel, pixel for pixel and bit for bit: every 8-bit
// color against every 8-bit alpha of a straight 8:8:8:8 frame, and every 10-bit code of a
// 10:10:10:2 frame with opaque, transparent and not-opaque alpha fields, over an extent that is
// not a multiple of the workgroup and a data window away from the origin. Local mode pins the
// explicit loader and requires a device; an image without one prints an explicit skip.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/image.hpp>

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
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

[[nodiscard]] GpuImageUploadPollResult pollUntilTerminal(GpuImageUpload& upload) {
    for (int attempt = 0; attempt < 20000; ++attempt) {
        const auto result = upload.poll();
        if (result != GpuImageUploadPollResult::Pending)
            return result;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return GpuImageUploadPollResult::Pending;
}

struct Frame final {
    std::shared_ptr<const std::vector<Rgba8>> cells;
    DisplayPixelLayout layout = DisplayPixelLayout::Straight8888;
    std::optional<Rgba32fImageDescriptor> descriptor;
};

[[nodiscard]] Frame makeFrame(const DisplayPixelLayout layout, const std::int64_t originX,
                              const std::int64_t originY, const std::uint32_t width,
                              const std::uint32_t height) {
    Frame frame;
    frame.layout = layout;
    std::vector<Rgba8> cells;
    cells.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t i = y * width + x;
            if (layout == DisplayPixelLayout::Rgb10A2) {
                // Every code in every channel; the 2-bit alpha takes all four values, of which
                // only 3 is a visible pixel.
                cells.push_back(rgb10A2Cell({i % 1024U, (i * 7U + 5U) % 1024U,
                                             1023U - i % 1024U, (i / 1024U + i % 5U) % 4U}));
            } else {
                // Red meets every alpha: x is the color and y the alpha.
                cells.push_back(Rgba8{static_cast<std::uint8_t>(x % 256U),
                                      static_cast<std::uint8_t>(255U - x % 256U),
                                      static_cast<std::uint8_t>((x * 7U + y * 3U) % 256U),
                                      static_cast<std::uint8_t>(y % 256U)});
            }
        }
    }
    frame.cells = std::make_shared<const std::vector<Rgba8>>(std::move(cells));
    const auto window = ImageWindow::create(originX, originY, width, height);
    if (window) {
        const auto descriptor = Rgba32fImageDescriptor::create(
            *window.value(), *window.value(), bloom::core::PixelAspectRatio::square());
        if (descriptor)
            frame.descriptor = *descriptor.value();
    }
    return frame;
}

[[nodiscard]] GpuImageUploadParameters parametersFor(const Frame& frame) {
    GpuImageUploadParameters parameters;
    parameters.displayCells =
        GpuUploadDisplayCells{frame.cells, std::span<const Rgba8>(*frame.cells), frame.layout};
    parameters.descriptorOverride = frame.descriptor;
    return parameters;
}

void checkFrame(Expectations& expectations, GpuImageUpload& upload, const Frame& frame,
                const std::string& label) {
    expectations.expect(frame.descriptor.has_value(), label + ": the fixture has a descriptor");
    if (!frame.descriptor)
        return;
    const auto before = upload.displayUnpackedUploads();
    const auto begun = upload.begin(parametersFor(frame), 1ULL << 32U);
    expectations.expect(begun.code == GpuImageUploadDiagnosticCode::None,
                        label + ": the upload begins: " + begun.message);
    if (begun.code != GpuImageUploadDiagnosticCode::None)
        return;
    expectations.expect(upload.displayUnpackedUploads() == before + 1,
                        label + ": the frame crossed as its cells and was unpacked on the device");
    expectations.expect(pollUntilTerminal(upload) == GpuImageUploadPollResult::Ready,
                        label + ": the upload reaches Ready");
    const auto* const resident = upload.image();
    expectations.expect(resident != nullptr &&
                            resident->dataWindow() == frame.descriptor->dataWindow() &&
                            resident->displayWindow() == frame.descriptor->displayWindow() &&
                            resident->pixelAspect() == frame.descriptor->pixelAspect(),
                        label + ": the resident picture keeps the frame's geometry");
    auto readback = upload.readback();
    expectations.expect(readback.hasValue() && readback.pixels.size() == frame.cells->size(),
                        label + ": the resident picture reads back");
    if (readback.hasValue() && readback.pixels.size() == frame.cells->size()) {
        std::size_t mismatches = 0;
        std::size_t first = 0;
        for (std::size_t index = 0; index < frame.cells->size(); ++index) {
            const auto expected = displaySignalPixel(frame.layout, (*frame.cells)[index]);
            const auto actual = readback.pixels[index].components();
            bool same = true;
            for (std::size_t c = 0; c < 4; ++c)
                same = same && std::bit_cast<std::uint32_t>(expected[c]) ==
                                   std::bit_cast<std::uint32_t>(actual[c]);
            if (!same && mismatches++ == 0)
                first = index;
        }
        if (mismatches != 0) {
            const auto expected = displaySignalPixel(frame.layout, (*frame.cells)[first]);
            const auto actual = readback.pixels[first].components();
            std::cerr << label << ": first mismatch at " << first << " expected (" << expected[0]
                      << ", " << expected[1] << ", " << expected[2] << ", " << expected[3]
                      << ") got (" << actual[0] << ", " << actual[1] << ", " << actual[2] << ", "
                      << actual[3] << ")\n";
        }
        expectations.expect(mismatches == 0,
                            label + ": every pixel equals the CPU rule bit for bit (" +
                                std::to_string(mismatches) + " differ)");
    }
    GpuImage taken = upload.takeImage();
    expectations.expect(taken.isValid(), label + ": the resident picture is taken");
}

void testDevice(Expectations& expectations, GpuImageUpload& upload) {
    // 256 x 256: every 8-bit color (x) against every 8-bit alpha (y).
    checkFrame(expectations, upload, makeFrame(DisplayPixelLayout::Straight8888, 0, 0, 256, 256),
               "straight 8:8:8:8, every color against every alpha");
    // 1031 x 9: every 10-bit code more than once, the alpha field taking all four values, a
    // width no workgroup divides and a data window away from the origin.
    checkFrame(expectations, upload, makeFrame(DisplayPixelLayout::Rgb10A2, -7, 13, 1031, 9),
               "10:10:10:2, every code");
    checkFrame(expectations, upload, makeFrame(DisplayPixelLayout::Straight8888, 5, -3, 37, 3),
               "straight 8:8:8:8, a small frame away from the origin");

    // Refusals: a frame without its picture's descriptor, and cells that are not the descriptor's.
    const Frame frame = makeFrame(DisplayPixelLayout::Rgb10A2, 0, 0, 8, 4);
    if (!frame.descriptor)
        return;
    auto noDescriptor = parametersFor(frame);
    noDescriptor.descriptorOverride.reset();
    expectations.expect(upload.begin(noDescriptor, 1ULL << 32U).code ==
                            GpuImageUploadDiagnosticCode::InvalidArgument,
                        "a display frame without its descriptor is refused");
    auto shortCells = parametersFor(frame);
    shortCells.displayCells->cells = shortCells.displayCells->cells.first(31);
    expectations.expect(upload.begin(shortCells, 1ULL << 32U).code ==
                            GpuImageUploadDiagnosticCode::InvalidArgument,
                        "cells that do not fill the descriptor's data window are refused");
    auto two = parametersFor(frame);
    auto builder = Rgba32fImageBuilder::create(*frame.descriptor,
                                               frame.descriptor->layout().pixelStorageBytes);
    expectations.expect(static_cast<bool>(builder),
                        "a float picture for the two-source refusal is built");
    if (builder) {
        auto owned = std::move(*builder.value());
        auto image = std::move(owned).freeze();
        if (image) {
            two.source = std::make_shared<const Rgba32fImage>(std::move(*image.value()));
            expectations.expect(upload.begin(two, 1ULL << 32U).code ==
                                    GpuImageUploadDiagnosticCode::InvalidArgument,
                                "an upload with two sources is refused");
        }
    }
    expectations.expect(upload.begin(parametersFor(frame), 16).code ==
                            GpuImageUploadDiagnosticCode::OverBudget,
                        "a frame beyond the byte budget is refused");
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
        return 0;
    }
    auto created = GpuImageUpload::create(*device.device);
    expectations.expect(created.hasValue(), "the upload pipeline is created");
    if (!created)
        return 1;
    testDevice(expectations, *created.upload);
    if (!expectations.ok()) {
        std::cerr << "FAIL: DisplayUnpackV1 expectations failed\n";
        return 1;
    }
    std::cout << "PASS: DisplayUnpackV1\n";
    return 0;
}
