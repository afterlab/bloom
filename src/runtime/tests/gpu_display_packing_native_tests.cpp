// #733: native acceptance of the device display packs (render/gpu_display_pack.hpp).
//
// Every device packer of a preview display frame is run in both four-byte layouts and compared
// with the CPU packers of render/display_pixel_packing.hpp and color/ocio_cpu_display_frame.hpp:
//   * the Bloom Neutral v1 kernel through GpuNeutralDisplay (host words) and GpuResidentDisplay
//     (resident image, transferred once);
//   * the general OCIO display program through runtime::GpuOcioDisplayArm (preview packs).
// What is checked: every 10-bit code is packed and stored exactly; the codes of an ordinary frame
// are within one code of the CPU's (the long-standing allowance for the display transform's float
// difference, now one code of 1023) with alpha exact; the layout flag follows the layout rule,
// and a 10:10:10:2 pack of a frame that does not allow it is refused, never published; the frame
// counters equal the CPU's; the 8:8:8:8 pack gives the words it always gave.
//
// The frames keep their signal away from the ends of the axis, so a float difference between the
// device and the CPU cannot move a pixel across a counter's edge.
//
// A missing device is an explicit SKIP (exit 77) unless --require-device is passed.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/executable_suffix.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_display_pack.hpp>
#include <bloom/render/gpu_display_transfer.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/gpu_ocio_display_arm.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_ocio_wrapper.hpp>
#include <bloom/runtime/view_adjust.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::color::PreparedCpuDisplayProcessorHandle;
using bloom::render::DisplayFrameCounters;
using bloom::render::DisplayLumaCoefficients;
using bloom::render::DisplayPixelLayout;
using bloom::render::GpuDevice;
using bloom::render::GpuDisplayPackReport;
using bloom::render::GpuDisplayPackRequest;
using bloom::render::GpuImage;
using bloom::render::GpuNeutralDisplay;
using bloom::render::GpuNeutralDisplayDiagnosticCode;
using bloom::render::GpuNeutralDisplayPollResult;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuResidentDisplayDiagnosticCode;
using bloom::render::GpuResidentDisplayPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::Rgba8;

constexpr int kSkipExit = 77;
constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;
// Rec.709 luma, the coefficients of the Bloom Neutral display.
constexpr DisplayLumaCoefficients kLuma{0.2126F, 0.7152F, 0.0722F};

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

[[nodiscard]] bool parseRequireDevice(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            return true;
        }
    }
    return false;
}

// ---- Frames -------------------------------------------------------------------------------------

struct Frame final {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Rgba32f> pixels;
};

// A pixel from straight scene-linear color and alpha.
[[nodiscard]] Rgba32f straightPixel(const float red, const float green, const float blue,
                                    const float alpha) {
    const auto value =
        Rgba32f::fromPremultiplied(red * alpha, green * alpha, blue * alpha, alpha);
    return value ? *value.value() : Rgba32f::transparent();
}

// The scene-linear value whose Bloom Neutral display value is `display` (the inverse of the
// display's piecewise sRGB curve).
[[nodiscard]] float sceneOfDisplay(const double display) {
    constexpr double slope = 12.9232101;
    constexpr double breakPoint = 0.00303993467 * slope;
    if (display <= breakPoint) {
        return static_cast<float>(display / slope);
    }
    return static_cast<float>(std::pow((display + 0.055) / 1.055, 2.4));
}

// Every 10-bit code, each as an opaque gray pixel whose display value is the middle of the code's
// bin: the pack must store exactly that code.
[[nodiscard]] Frame codesFrame() {
    Frame frame{"every code", 64, 16, {}};
    frame.pixels.reserve(1024);
    for (std::uint32_t code = 0; code < 1024; ++code) {
        const float scene = sceneOfDisplay(static_cast<double>(code) / 1023.0);
        frame.pixels.push_back(straightPixel(scene, scene, scene, 1.0F));
    }
    return frame;
}

// Opaque and exactly transparent pixels: grays below, inside and above the axis, and a few
// colors. `soft` adds one half-transparent pixel, which no 10:10:10:2 frame can hold.
[[nodiscard]] Frame mixedFrame(const bool soft) {
    constexpr std::array<float, 8> levels{-0.2F, -0.05F, 0.02F, 0.18F, 0.5F, 0.9F, 1.3F, 2.5F};
    Frame frame{soft ? "soft edge" : "opaque and transparent", 43, 3, {}};
    const std::size_t count = std::size_t{frame.width} * frame.height;
    frame.pixels.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (index % 5U == 3U) {
            frame.pixels.push_back(Rgba32f::transparent());
            continue;
        }
        const float level = levels[index % levels.size()];
        frame.pixels.push_back(straightPixel(level, level, level, 1.0F));
    }
    frame.pixels[1] = straightPixel(2.5F, 0.0F, 0.0F, 1.0F);
    frame.pixels[2] = straightPixel(0.0F, 0.6F, 0.0F, 1.0F);
    frame.pixels[4] = straightPixel(0.05F, 0.05F, 3.0F, 1.0F);
    frame.pixels[5] = straightPixel(0.9F, 0.4F, 0.1F, 1.0F);
    if (soft) {
        frame.pixels[7] = straightPixel(0.5F, 0.25F, 0.125F, 0.5F);
    }
    return frame;
}

[[nodiscard]] std::optional<Rgba32fImage> makeImage(const Frame& frame) {
    const auto window = ImageWindow::create(0, 0, frame.width, frame.height);
    if (!window) {
        return std::nullopt;
    }
    const auto descriptor = Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                           bloom::core::PixelAspectRatio::square());
    if (!descriptor || descriptor.value()->layout().pixelCount != frame.pixels.size()) {
        return std::nullopt;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
    if (!builder) {
        return std::nullopt;
    }
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const auto row = builder.value()->row(y);
        if (!row) {
            return std::nullopt;
        }
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            (*row.value())[x] = frame.pixels[std::size_t{y} * frame.width + x];
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    return std::move(*frozen.value());
}

// ---- The CPU reference ---------------------------------------------------------------------------

struct Packed final {
    DisplayPixelLayout layout = DisplayPixelLayout::Straight8888;
    std::vector<Rgba8> cells;
    DisplayFrameCounters counters;
};

// The CPU packer of a preview frame: the layout rule decides the layout.
[[nodiscard]] std::optional<Packed> cpuPack(const PreparedCpuDisplayProcessorHandle& handle,
                                            const Frame& frame, const bool allowRgb10A2) {
    const auto image = makeImage(frame);
    if (!image.has_value()) {
        return std::nullopt;
    }
    const auto view = image->view();
    if (!view) {
        return std::nullopt;
    }
    const auto produced = bloom::color::produceBloomNeutralDisplayFrame(
        handle, *view.value(), 65536, std::numeric_limits<std::size_t>::max(), {},
        bloom::color::DisplayFramePacking{.allowRgb10A2 = allowRgb10A2, .luma = kLuma});
    if (!produced) {
        return std::nullopt;
    }
    Packed packed;
    packed.layout = produced.value()->pixelLayout();
    const auto cells = produced.value()->pixels();
    packed.cells.assign(cells.begin(), cells.end());
    packed.counters = produced.value()->counters();
    return packed;
}

// Cells of one layout within `tolerance` codes per color channel, alpha exact.
[[nodiscard]] bool cellsMatch(const DisplayPixelLayout layout, const std::span<const Rgba8> device,
                              const std::span<const Rgba8> reference,
                              const std::uint32_t tolerance) {
    if (device.size() != reference.size()) {
        return false;
    }
    const auto near = [tolerance](const std::uint32_t left, const std::uint32_t right) {
        return (left > right ? left - right : right - left) <= tolerance;
    };
    for (std::size_t index = 0; index < device.size(); ++index) {
        if (layout == DisplayPixelLayout::Rgb10A2) {
            const auto got = bloom::render::rgb10A2Codes(device[index]);
            const auto want = bloom::render::rgb10A2Codes(reference[index]);
            if (got.alpha != want.alpha || !near(got.red, want.red) ||
                !near(got.green, want.green) || !near(got.blue, want.blue)) {
                return false;
            }
            continue;
        }
        if (device[index].alpha != reference[index].alpha ||
            !near(device[index].red, reference[index].red) ||
            !near(device[index].green, reference[index].green) ||
            !near(device[index].blue, reference[index].blue)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool exactCodes(const std::span<const Rgba8> cells) {
    if (cells.size() != 1024) {
        return false;
    }
    for (std::uint32_t code = 0; code < 1024; ++code) {
        const auto codes = bloom::render::rgb10A2Codes(cells[code]);
        if (codes.red != code || codes.green != code || codes.blue != code ||
            codes.alpha != bloom::render::kDisplayAlpha2Opaque) {
            return false;
        }
    }
    return true;
}

// ---- The Bloom Neutral kernel: host words --------------------------------------------------------

struct NeutralRun final {
    GpuNeutralDisplayPollResult poll = GpuNeutralDisplayPollResult::Failure;
    GpuNeutralDisplayDiagnosticCode code = GpuNeutralDisplayDiagnosticCode::None;
    std::optional<GpuDisplayPackReport> report;
    DisplayPixelLayout layout = DisplayPixelLayout::Straight8888;
    std::vector<Rgba8> cells;
};

[[nodiscard]] NeutralRun runNeutral(GpuNeutralDisplay& display, const std::span<const Rgba32f> pixels,
                                    const std::optional<GpuDisplayPackRequest>& pack) {
    NeutralRun run;
    const auto begun = pack.has_value() ? display.begin(pixels, kBudget, *pack)
                                        : display.begin(pixels, kBudget);
    run.code = begun.code;
    if (begun.code != GpuNeutralDisplayDiagnosticCode::None) {
        return run;
    }
    run.poll = GpuNeutralDisplayPollResult::Pending;
    while (run.poll == GpuNeutralDisplayPollResult::Pending) {
        run.poll = display.poll();
    }
    run.report = display.packReport();
    run.code = display.diagnostic().code;
    if (run.poll == GpuNeutralDisplayPollResult::Ready) {
        auto readback = display.readback();
        run.layout = readback.pixelLayout;
        if (readback.report.has_value()) {
            run.report = readback.report;
        }
        run.cells = std::move(readback.pixels);
    }
    return run;
}

void testNeutral(Expectations& checks, GpuNeutralDisplay& display,
                 const PreparedCpuDisplayProcessorHandle& handle) {
    const GpuDisplayPackRequest wide{DisplayPixelLayout::Rgb10A2, kLuma};
    const GpuDisplayPackRequest straight{DisplayPixelLayout::Straight8888, kLuma};

    // Every 10-bit code is packed exactly, by the CPU and by the device.
    {
        const auto frame = codesFrame();
        const auto cpu = cpuPack(handle, frame, true);
        checks.expect(cpu.has_value() && cpu->layout == DisplayPixelLayout::Rgb10A2 &&
                          exactCodes(cpu->cells),
                      "every code: the CPU packs each of the 1024 codes exactly");
        const auto run = runNeutral(display, frame.pixels, wide);
        checks.expect(run.poll == GpuNeutralDisplayPollResult::Ready &&
                          run.layout == DisplayPixelLayout::Rgb10A2,
                      "every code: the device packs a 10:10:10:2 frame");
        checks.expect(exactCodes(run.cells),
                      "every code: the device packs each of the 1024 codes exactly");
        checks.expect(run.report.has_value() && run.report->allowsRgb10A2 &&
                          run.report->wordsValid() && cpu.has_value() &&
                          run.report->counters == cpu->counters &&
                          run.report->counters == [] {
                              DisplayFrameCounters none;
                              return none;
                          }(),
                      "every code: the frame allows the layout and nothing is under or over");
    }

    // An ordinary frame: opaque and exactly transparent pixels, in both layouts.
    const auto mixed = mixedFrame(false);
    const auto cpuWide = cpuPack(handle, mixed, true);
    const auto cpuStraight = cpuPack(handle, mixed, false);
    checks.expect(cpuWide.has_value() && cpuWide->layout == DisplayPixelLayout::Rgb10A2 &&
                      cpuStraight.has_value() &&
                      cpuStraight->layout == DisplayPixelLayout::Straight8888,
                  "the CPU packs the opaque and transparent frame in both layouts");
    if (!cpuWide.has_value() || !cpuStraight.has_value()) {
        return;
    }
    checks.expect(cpuWide->counters.lumaCounted() && cpuWide->counters.under[0] > 0 &&
                      cpuWide->counters.over[0] > 0 && cpuWide->counters.over[3] > 0 &&
                      cpuWide->counters.notFinite == 0 &&
                      cpuWide->counters == cpuStraight->counters,
                  "the frame has pixels under and over the axis, counted alike in both layouts");
    const auto wideRun = runNeutral(display, mixed.pixels, wide);
    checks.expect(wideRun.poll == GpuNeutralDisplayPollResult::Ready &&
                      wideRun.layout == DisplayPixelLayout::Rgb10A2 &&
                      cellsMatch(DisplayPixelLayout::Rgb10A2, wideRun.cells, cpuWide->cells, 1),
                  "10:10:10:2: the device codes are within one code of the CPU's, alpha exact");
    checks.expect(wideRun.report.has_value() && wideRun.report->allowsRgb10A2 &&
                      wideRun.report->layout == DisplayPixelLayout::Rgb10A2 &&
                      wideRun.report->frameLayout() == DisplayPixelLayout::Rgb10A2 &&
                      wideRun.report->counters == cpuWide->counters,
                  "10:10:10:2: the layout flag and the frame counters equal the CPU's");
    for (std::size_t index = 0; index < mixed.pixels.size(); ++index) {
        if (mixed.pixels[index].alpha() == 0.0F) {
            checks.expect(index < wideRun.cells.size() &&
                              bloom::render::displayWord(wideRun.cells[index]) == 0U,
                          "10:10:10:2: a transparent pixel is the word 0");
            break;
        }
    }
    const auto straightRun = runNeutral(display, mixed.pixels, straight);
    const auto legacyRun = runNeutral(display, mixed.pixels, std::nullopt);
    checks.expect(straightRun.poll == GpuNeutralDisplayPollResult::Ready &&
                      straightRun.layout == DisplayPixelLayout::Straight8888 &&
                      cellsMatch(DisplayPixelLayout::Straight8888, straightRun.cells,
                                 cpuStraight->cells, 1),
                  "8:8:8:8: the device codes are within one code of the CPU's, alpha exact");
    checks.expect(legacyRun.poll == GpuNeutralDisplayPollResult::Ready &&
                      legacyRun.layout == DisplayPixelLayout::Straight8888 &&
                      legacyRun.cells == straightRun.cells,
                  "8:8:8:8: a pack that asks for nothing gives the same words");
    checks.expect(straightRun.report.has_value() && straightRun.report->allowsRgb10A2 &&
                      straightRun.report->wordsValid() &&
                      straightRun.report->frameLayout() == DisplayPixelLayout::Rgb10A2 &&
                      straightRun.report->counters == cpuWide->counters,
                  "8:8:8:8: the report says the frame would fit 10:10:10:2, counters as before");
    checks.expect(legacyRun.report.has_value() && !legacyRun.report->counters.lumaCounted() &&
                      legacyRun.report->counters.under[0] == cpuWide->counters.under[0] &&
                      legacyRun.report->counters.over[2] == cpuWide->counters.over[2],
                  "8:8:8:8: without coefficients the luma counters are absent");

    // A frame with a soft pixel is refused as 10:10:10:2 and packs as 8:8:8:8.
    const auto soft = mixedFrame(true);
    const auto cpuSoft = cpuPack(handle, soft, true);
    checks.expect(cpuSoft.has_value() && cpuSoft->layout == DisplayPixelLayout::Straight8888,
                  "soft edge: the CPU keeps the frame 8:8:8:8");
    const auto refused = runNeutral(display, soft.pixels, wide);
    checks.expect(refused.poll == GpuNeutralDisplayPollResult::Failure &&
                      refused.code == GpuNeutralDisplayDiagnosticCode::LayoutRefused &&
                      refused.cells.empty(),
                  "soft edge: a 10:10:10:2 pack is refused and publishes nothing");
    checks.expect(refused.report.has_value() && !refused.report->allowsRgb10A2 &&
                      !refused.report->wordsValid() &&
                      refused.report->frameLayout() == DisplayPixelLayout::Straight8888,
                  "soft edge: the report names the layout the frame takes");
    checks.expect(!display.readback().hasValue(), "soft edge: the refused frame cannot be read");
    const auto softRun = runNeutral(display, soft.pixels, straight);
    checks.expect(softRun.poll == GpuNeutralDisplayPollResult::Ready && cpuSoft.has_value() &&
                      cellsMatch(DisplayPixelLayout::Straight8888, softRun.cells, cpuSoft->cells,
                                 1) &&
                      softRun.report.has_value() && !softRun.report->allowsRgb10A2 &&
                      softRun.report->counters == cpuSoft->counters,
                  "soft edge: the 8:8:8:8 pack succeeds with the CPU's pixels and counters");

    // The layout rule's edge: the smallest alpha that packs to the 8-bit alpha 255, and the
    // float below it.
    float edge = 254.0F / 255.0F;
    while (bloom::render::displayCode8(edge) != 255U) {
        edge = std::nextafter(edge, 2.0F);
    }
    const float below = std::nextafter(edge, 0.0F);
    checks.expect(bloom::render::displayAlphaAllowsRgb10A2(edge) &&
                      !bloom::render::displayAlphaAllowsRgb10A2(below),
                  "the layout rule's edge is one float wide");
    for (const float alpha : {edge, below, 1.0F, 0.0F}) {
        const std::vector<Rgba32f> one{straightPixel(0.25F, 0.5F, 0.75F, alpha)};
        const auto run = runNeutral(display, one, straight);
        checks.expect(run.poll == GpuNeutralDisplayPollResult::Ready && run.report.has_value() &&
                          run.report->allowsRgb10A2 ==
                              bloom::render::displayAlphaAllowsRgb10A2(alpha),
                      "the device's layout flag follows the layout rule at its edge");
    }
}

// ---- The Bloom Neutral kernel: resident image ----------------------------------------------------

[[nodiscard]] std::shared_ptr<const GpuImage> solidInput(bloom::render::GpuSolid& solid,
                                                         const Rgba32f pixel,
                                                         const ImageWindow window) {
    const auto begun = solid.begin(
        bloom::render::GpuSolidParameters{pixel, window, window,
                                          bloom::core::PixelAspectRatio::square()},
        kBudget);
    if (begun.code != bloom::render::GpuSolidDiagnosticCode::None) {
        return nullptr;
    }
    auto poll = bloom::render::GpuSolidPollResult::Pending;
    while (poll == bloom::render::GpuSolidPollResult::Pending) {
        poll = solid.poll();
    }
    if (poll != bloom::render::GpuSolidPollResult::Ready) {
        return nullptr;
    }
    GpuImage taken = solid.takeImage();
    return taken.isValid() ? std::make_shared<const GpuImage>(std::move(taken)) : nullptr;
}

void testResident(Expectations& checks, GpuDevice& device, GpuNeutralDisplay& neutral) {
    auto solid = bloom::render::GpuSolid::create(device);
    auto display = GpuResidentDisplay::create(device);
    checks.expect(solid.hasValue() && display.hasValue(),
                  "the solid and resident display pipelines are created");
    const auto window = ImageWindow::create(-3, 5, 7, 3);
    if (!solid || !display || !window) {
        return;
    }
    const std::size_t count = 21;
    const GpuDisplayPackRequest wide{DisplayPixelLayout::Rgb10A2, kLuma};
    const GpuDisplayPackRequest straight{DisplayPixelLayout::Straight8888, kLuma};
    const auto poll = [&display]() {
        auto result = GpuResidentDisplayPollResult::Pending;
        while (result == GpuResidentDisplayPollResult::Pending) {
            result = display.display->poll();
        }
        return result;
    };

    // An opaque frame: a resident 10:10:10:2 image with the words of the host route.
    const Rgba32f opaque = straightPixel(0.9F, 0.18F, 1.7F, 1.0F);
    const auto input = solidInput(*solid.solid, opaque, *window.value());
    checks.expect(input != nullptr, "the opaque resident input is made");
    if (input == nullptr) {
        return;
    }
    const auto host = runNeutral(neutral, std::vector<Rgba32f>(count, opaque), wide);
    checks.expect(display.display->begin(input, kBudget, wide).code ==
                          GpuResidentDisplayDiagnosticCode::None &&
                      poll() == GpuResidentDisplayPollResult::Ready,
                  "resident: the 10:10:10:2 pack of an opaque frame completes");
    const auto* image = display.display->image();
    checks.expect(image != nullptr && image->pixelLayout() == DisplayPixelLayout::Rgb10A2 &&
                      image->packReport().has_value() &&
                      image->packReport() == display.display->packReport() &&
                      image->packReport() == host.report,
                  "resident: the image names its layout and carries the pack's report");
    const auto readback = display.display->readback();
    checks.expect(readback.hasValue() && host.poll == GpuNeutralDisplayPollResult::Ready &&
                      readback.pixels == host.cells,
                  "resident: the image holds the words of the host route");
    auto taken = std::make_shared<const bloom::render::GpuDisplayImage>(
        display.display->takeImage());
    checks.expect(taken->isValid() && taken->pixelLayout() == DisplayPixelLayout::Rgb10A2 &&
                      taken->packReport() == host.report,
                  "resident: the taken image keeps the layout and the report");
    bloom::render::GpuDisplayTransfer transfer;
    checks.expect(transfer.begin(device, taken, kBudget), "resident: the transfer is admitted");
    auto state = bloom::render::GpuDisplayTransferState::Pending;
    while (state == bloom::render::GpuDisplayTransferState::Pending) {
        state = transfer.poll();
    }
    auto moved = transfer.take();
    checks.expect(moved.has_value() && moved->pixelLayout() == DisplayPixelLayout::Rgb10A2 &&
                      moved->packReport() == host.report &&
                      std::vector<Rgba8>(moved->pixels().begin(), moved->pixels().end()) ==
                          host.cells,
                  "resident: one transfer hands on the words, the layout and the counters");
    taken.reset();

    // A half-transparent frame: refused as 10:10:10:2, packed as 8:8:8:8.
    const Rgba32f half = straightPixel(0.9F, 0.18F, 1.7F, 0.5F);
    const auto softInput = solidInput(*solid.solid, half, *window.value());
    checks.expect(softInput != nullptr, "the half-transparent resident input is made");
    if (softInput == nullptr) {
        return;
    }
    checks.expect(display.display->begin(softInput, kBudget, wide).code ==
                          GpuResidentDisplayDiagnosticCode::None &&
                      poll() == GpuResidentDisplayPollResult::Failure &&
                      display.display->diagnostic().code ==
                          GpuResidentDisplayDiagnosticCode::LayoutRefused &&
                      display.display->image() == nullptr &&
                      !display.display->takeImage().isValid(),
                  "resident: a 10:10:10:2 pack of a soft frame is refused, nothing published");
    checks.expect(display.display->packReport().has_value() &&
                      !display.display->packReport()->allowsRgb10A2,
                  "resident: the refusal's report names the layout the frame takes");
    const auto softHost = runNeutral(neutral, std::vector<Rgba32f>(count, half), straight);
    checks.expect(display.display->begin(softInput, kBudget, straight).code ==
                          GpuResidentDisplayDiagnosticCode::None &&
                      poll() == GpuResidentDisplayPollResult::Ready,
                  "resident: the same input packs as 8:8:8:8 after the refusal");
    const auto* softImage = display.display->image();
    const auto softReadback = display.display->readback();
    checks.expect(softImage != nullptr &&
                      softImage->pixelLayout() == DisplayPixelLayout::Straight8888 &&
                      softImage->packReport().has_value() &&
                      !softImage->packReport()->allowsRgb10A2 && softReadback.hasValue() &&
                      softReadback.pixels == softHost.cells,
                  "resident: the 8:8:8:8 image holds the words of the host route");
    static_cast<void>(display.display->takeImage());
}

// ---- The general OCIO display program -------------------------------------------------------------

#ifdef BLOOM_GPUSHADER_TOOLS_DIR

using bloom::render::GpuImageUpload;
using bloom::runtime::GpuOcioCommandGeometry;
using bloom::runtime::GpuOcioDisplayArm;
using bloom::runtime::GpuOcioDisplayArmDiagnosticCode;
using bloom::runtime::GpuOcioDisplayPacking;
using bloom::runtime::GpuOcioDisplayPackingMode;
using bloom::runtime::GpuOcioDisplayRequest;
using bloom::runtime::GpuOcioExecutorDiagnosticCode;
using bloom::runtime::GpuOcioExecutorPollResult;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;
using bloom::runtime::PreparedGpuOcioCommand;
using bloom::runtime::ViewAdjust;

[[nodiscard]] bloom::runtime::GpuOcioCompileOptions compileOptions() {
    bloom::runtime::GpuOcioCompileOptions options;
    options.glslangValidatorPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator" BLOOM_EXECUTABLE_SUFFIX;
    options.spirvValPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val" BLOOM_EXECUTABLE_SUFFIX;
    return options;
}

[[nodiscard]] std::shared_ptr<const GpuImage> uploadFrame(GpuImageUpload& uploader,
                                                          const GpuDevice& device,
                                                          const Frame& frame) {
    auto image = makeImage(frame);
    if (!image.has_value()) {
        return nullptr;
    }
    auto source = std::make_shared<const Rgba32fImage>(std::move(*image));
    if (uploader.begin({source, device.retainedProcessingPhase()}, kBudget).code !=
        bloom::render::GpuImageUploadDiagnosticCode::None) {
        return nullptr;
    }
    auto poll = bloom::render::GpuImageUploadPollResult::Pending;
    while (poll == bloom::render::GpuImageUploadPollResult::Pending) {
        poll = uploader.poll();
    }
    if (poll != bloom::render::GpuImageUploadPollResult::Ready) {
        return nullptr;
    }
    return std::make_shared<GpuImage>(uploader.takeImage());
}

struct ArmRun final {
    GpuOcioExecutorPollResult poll = GpuOcioExecutorPollResult::Failure;
    GpuOcioDisplayArmDiagnosticCode begun = GpuOcioDisplayArmDiagnosticCode::None;
    GpuOcioExecutorDiagnosticCode code = GpuOcioExecutorDiagnosticCode::None;
    std::optional<GpuDisplayPackReport> report;
    std::optional<GpuDisplayPackReport> imageReport;
    DisplayPixelLayout layout = DisplayPixelLayout::Straight8888;
    std::vector<Rgba8> cells;
};

[[nodiscard]] ArmRun runArm(GpuOcioDisplayArm& arm,
                            const std::shared_ptr<const PreparedGpuOcioCommand>& command,
                            const std::shared_ptr<const GpuImage>& input, const Frame& frame,
                            const DisplayPixelLayout layout, const ViewAdjust adjust = {}) {
    ArmRun run;
    GpuOcioDisplayRequest request;
    request.command = command;
    request.layout = layout;
    request.input = input;
    request.width = frame.width;
    request.height = frame.height;
    request.viewAdjust = adjust;
    request.byteBudget = kBudget;
    run.begun = arm.begin(request).code;
    if (run.begun != GpuOcioDisplayArmDiagnosticCode::None) {
        return run;
    }
    run.poll = GpuOcioExecutorPollResult::Pending;
    while (run.poll == GpuOcioExecutorPollResult::Pending) {
        run.poll = arm.poll();
    }
    run.report = arm.packReport();
    run.code = arm.executor().diagnostic().code;
    if (run.poll != GpuOcioExecutorPollResult::Ready) {
        return run;
    }
    auto output = arm.takeDisplayImage();
    if (!output.has_value() || !output->isValid()) {
        return run;
    }
    run.layout = output->pixelLayout();
    run.imageReport = output->packReport();
    auto readback = bloom::render::readbackResidentDisplayImage(*output, kBudget);
    if (readback.hasValue()) {
        run.cells = std::move(readback.pixels);
    }
    return run;
}

void testGeneralDisplay(Expectations& checks, GpuDevice& device,
                        const bloom::color::ResolvedBloomNeutralConfig& config) {
    const std::string displayName(config.displayName());
    const std::string viewName(config.viewName());
    const auto cpuHandle =
        bloom::color::buildCpuDisplayProcessorForView(config, displayName, viewName);
    checks.expect(cpuHandle.handle() != nullptr, "general: the CPU display processor prepares");
    if (cpuHandle.handle() == nullptr) {
        return;
    }
    const auto mixed = mixedFrame(false);
    const auto soft = mixedFrame(true);
    const GpuOcioCommandGeometry geometry{mixed.width, mixed.height};
    GpuOcioProgramPreparer preparer;
    const auto prepare = [&](const GpuOcioDisplayPacking& packing, const ViewAdjust adjust) {
        GpuOcioTransformSpec spec;
        spec.kind = GpuOcioTransformKind::Display;
        spec.display = displayName;
        spec.view = viewName;
        spec.viewAdjust = adjust;
        spec.displayPacking = packing;
        return preparer.prepare(config, spec, geometry, compileOptions()).command;
    };
    const GpuOcioDisplayPacking preview{GpuOcioDisplayPackingMode::Preview, kLuma};
    const auto legacy = prepare({}, {});
    const auto previewCommand = prepare(preview, {});
    checks.expect(legacy != nullptr && previewCommand != nullptr,
                  "general: the two display commands prepare");
    if (legacy == nullptr || previewCommand == nullptr) {
        return;
    }
    checks.expect(legacy->identity() != previewCommand->identity() &&
                      !legacy->displayPacking().preview() &&
                      previewCommand->displayPacking() == preview,
                  "general: the preview pack is a command of its own, one for both layouts");
    checks.expect(prepare(preview, {}) == previewCommand && prepare({}, {}) == legacy,
                  "general: a prepared packing is served again from the preparer's cache");
    checks.expect(
        bloom::runtime::buildGpuOcioWrapperGlsl(legacy->program(), {}, {}).sourceDigest ==
                legacy->wrapperSourceDigest() &&
            bloom::runtime::buildGpuOcioWrapperGlsl(
                    legacy->program(), {},
                    GpuOcioDisplayPacking{GpuOcioDisplayPackingMode::Rgba8, kLuma})
                    .error == bloom::runtime::GpuOcioWrapperError::InvalidDisplayPacking,
        "general: the long-standing pack is the default text and takes no coefficients");

    auto arm = GpuOcioDisplayArm::create(device);
    auto uploader = GpuImageUpload::create(device);
    checks.expect(arm.hasValue() && uploader.hasValue(), "general: the arm and uploader host");
    if (!arm || !uploader) {
        return;
    }
    const auto mixedInput = uploadFrame(*uploader.upload, device, mixed);
    const auto softInput = uploadFrame(*uploader.upload, device, soft);
    checks.expect(mixedInput != nullptr && softInput != nullptr, "general: the frames upload");
    if (mixedInput == nullptr || softInput == nullptr) {
        return;
    }
    const auto cpuWide = cpuPack(*cpuHandle.handle(), mixed, true);
    const auto cpuSoft = cpuPack(*cpuHandle.handle(), soft, true);
    checks.expect(cpuWide.has_value() && cpuSoft.has_value(), "general: the CPU packs the frames");
    if (!cpuWide.has_value() || !cpuSoft.has_value()) {
        return;
    }

    // The program qualification holds a preview command to the CPU packer in both layouts.
    for (const auto& command : {previewCommand, legacy}) {
        const auto report = bloom::runtime::qualifyGpuOcioDisplay(
            command, *cpuHandle.handle(), device,
            bloom::runtime::GpuOcioDisplayQualificationBudgets{kBudget, kBudget});
        checks.expect(report.outcome() == bloom::runtime::GpuOcioDisplayOutcome::PreviewOnly &&
                          report.eligibleFor(device, *command),
                      "general: each command qualifies against the CPU packer");
    }

    const auto wideRun =
        runArm(*arm.arm, previewCommand, mixedInput, mixed, DisplayPixelLayout::Rgb10A2);
    checks.expect(wideRun.poll == GpuOcioExecutorPollResult::Ready &&
                      wideRun.layout == DisplayPixelLayout::Rgb10A2 &&
                      cellsMatch(DisplayPixelLayout::Rgb10A2, wideRun.cells, cpuWide->cells, 1),
                  "general 10:10:10:2: codes within one code of the CPU's, alpha exact");
    checks.expect(wideRun.report.has_value() && wideRun.report->allowsRgb10A2 &&
                      wideRun.report->counters == cpuWide->counters &&
                      wideRun.imageReport == wideRun.report,
                  "general 10:10:10:2: the layout flag and the counters equal the CPU's");

    // One layout holds for a frame and the device learns which by packing: the refused frame
    // is packed again by the same command, from the same picture, as 8:8:8:8.
    const auto refused =
        runArm(*arm.arm, previewCommand, softInput, soft, DisplayPixelLayout::Rgb10A2);
    checks.expect(refused.poll == GpuOcioExecutorPollResult::Failure &&
                      refused.code == GpuOcioExecutorDiagnosticCode::LayoutRefused &&
                      refused.report.has_value() && !refused.report->allowsRgb10A2 &&
                      refused.cells.empty(),
                  "general: a 10:10:10:2 pack of a soft frame is refused, nothing published");
    const auto softRun =
        runArm(*arm.arm, previewCommand, softInput, soft, DisplayPixelLayout::Straight8888);
    checks.expect(softRun.poll == GpuOcioExecutorPollResult::Ready &&
                      softRun.layout == DisplayPixelLayout::Straight8888 &&
                      cellsMatch(DisplayPixelLayout::Straight8888, softRun.cells, cpuSoft->cells,
                                 1) &&
                      softRun.report.has_value() && !softRun.report->allowsRgb10A2 &&
                      softRun.report->counters == cpuSoft->counters,
                  "general 8:8:8:8: the soft frame packs with the CPU's pixels and counters");
    // A straight pack of a frame that allows 10:10:10:2 says so, and that frame is packed again.
    const auto fitsRun =
        runArm(*arm.arm, previewCommand, mixedInput, mixed, DisplayPixelLayout::Straight8888);
    checks.expect(fitsRun.poll == GpuOcioExecutorPollResult::Ready &&
                      fitsRun.layout == DisplayPixelLayout::Straight8888 &&
                      fitsRun.report.has_value() && fitsRun.report->allowsRgb10A2 &&
                      fitsRun.report->counters == cpuWide->counters,
                  "general 8:8:8:8: a frame that allows 10:10:10:2 is reported as such");
    const auto wideAgain =
        runArm(*arm.arm, previewCommand, mixedInput, mixed, DisplayPixelLayout::Rgb10A2);
    checks.expect(wideAgain.poll == GpuOcioExecutorPollResult::Ready &&
                      wideAgain.cells == wideRun.cells && wideAgain.report == wideRun.report,
                  "general: the same frame packs to the same words whatever was packed before");
    const auto legacyRun =
        runArm(*arm.arm, legacy, softInput, soft, DisplayPixelLayout::Straight8888);
    checks.expect(legacyRun.poll == GpuOcioExecutorPollResult::Ready &&
                      legacyRun.cells == softRun.cells && !legacyRun.report.has_value() &&
                      !legacyRun.imageReport.has_value(),
                  "general: the long-standing pack gives the same words and reports nothing");
    const auto legacyWide =
        runArm(*arm.arm, legacy, mixedInput, mixed, DisplayPixelLayout::Rgb10A2);
    checks.expect(legacyWide.begun == GpuOcioDisplayArmDiagnosticCode::InvalidRequest,
                  "general: the long-standing pack has one layout and refuses another");

    // The Viewer's exposure and gamma change the packed codes, never the frame counters.
    constexpr ViewAdjust adjust{.exposure = 1.0, .gamma = 0.8};
    const auto adjustedCommand = prepare(preview, adjust);
    checks.expect(adjustedCommand != nullptr &&
                      adjustedCommand->identity() != previewCommand->identity(),
                  "general: an adjusted pack is a command of its own");
    if (adjustedCommand == nullptr) {
        return;
    }
    const auto adjustedRun = runArm(*arm.arm, adjustedCommand, mixedInput, mixed,
                                    DisplayPixelLayout::Rgb10A2, adjust);
    bool adjustedMatch = adjustedRun.poll == GpuOcioExecutorPollResult::Ready &&
                         adjustedRun.cells.size() == mixed.pixels.size();
    for (std::size_t index = 0; adjustedMatch && index < mixed.pixels.size(); ++index) {
        const auto& pixel = mixed.pixels[index];
        const auto codes = bloom::render::rgb10A2Codes(adjustedRun.cells[index]);
        if (pixel.alpha() == 0.0F) {
            adjustedMatch = bloom::render::displayWord(adjustedRun.cells[index]) == 0U;
            continue;
        }
        const auto encoded = cpuHandle.handle()->referenceToDisplayLinear(
            bloom::core::Color4d{static_cast<double>(pixel.red()),
                                 static_cast<double>(pixel.green()),
                                 static_cast<double>(pixel.blue()), 1.0});
        if (!encoded.has_value()) {
            adjustedMatch = false;
            break;
        }
        const auto near = [](const std::uint32_t left, const std::uint32_t right) {
            return (left > right ? left - right : right - left) <= 1U;
        };
        adjustedMatch =
            codes.alpha == bloom::render::kDisplayAlpha2Opaque &&
            near(codes.red, ViewAdjust::quantize10(adjust.fromEncoded(encoded->red))) &&
            near(codes.green, ViewAdjust::quantize10(adjust.fromEncoded(encoded->green))) &&
            near(codes.blue, ViewAdjust::quantize10(adjust.fromEncoded(encoded->blue)));
    }
    checks.expect(adjustedMatch,
                  "general: adjusted codes are within one code of the CPU's adjusted codes");
    checks.expect(adjustedRun.report.has_value() &&
                      adjustedRun.report->counters == cpuWide->counters,
                  "general: the counters are the display signal's, before the adjustment");

    // The scopes' display step of a frame no Viewer prepared (a headless read). One program
    // finds each pixel's stored cell with the preview pack's statements and writes the float
    // signal a scope counts for it, so its picture is the display arm's packed cells, unpacked,
    // bit for bit, with the same report; the frame never leaves the device as a picture.
    GpuOcioTransformSpec storedSpec;
    storedSpec.kind = GpuOcioTransformKind::ScopeDisplay;
    storedSpec.display = displayName;
    storedSpec.view = viewName;
    storedSpec.workingSpaceId = std::string(config.processColorSpaceId());
    storedSpec.displayPacking = preview;
    const auto storedCommand =
        preparer.prepare(config, storedSpec, geometry, compileOptions()).command;
    auto scopeExecutor = bloom::runtime::GpuOcioProgramExecutor::create(device);
    checks.expect(storedCommand != nullptr && scopeExecutor.hasValue(),
                  "stored signal: the scope display command and its executor prepare");
    if (storedCommand == nullptr || !scopeExecutor) {
        return;
    }
    struct StoredRun final {
        GpuOcioExecutorPollResult poll = GpuOcioExecutorPollResult::Failure;
        GpuOcioExecutorDiagnosticCode code = GpuOcioExecutorDiagnosticCode::None;
        std::optional<GpuDisplayPackReport> report;
        std::vector<Rgba32f> pixels;
    };
    const auto runStored = [&](const std::shared_ptr<const GpuImage>& input,
                               const DisplayPixelLayout layout) {
        StoredRun run;
        auto& executor = *scopeExecutor.executor;
        if (executor.begin(storedCommand, input, {}, kBudget, layout).code !=
            GpuOcioExecutorDiagnosticCode::None) {
            return run;
        }
        run.poll = GpuOcioExecutorPollResult::Pending;
        while (run.poll == GpuOcioExecutorPollResult::Pending) {
            run.poll = executor.poll();
        }
        run.report = executor.displayPackReport();
        run.code = executor.diagnostic().code;
        if (run.poll != GpuOcioExecutorPollResult::Ready) {
            return run;
        }
        const auto output = executor.takeEffectOutput();
        if (output != nullptr) {
            auto readback = bloom::render::readbackResidentImage(*output, kBudget);
            if (readback.hasValue()) {
                run.pixels = std::move(readback.pixels);
            }
        }
        return run;
    };
    const auto unpacks = [](const std::span<const Rgba32f> signal, const DisplayPixelLayout layout,
                            const std::span<const Rgba8> cells) {
        if (signal.empty() || signal.size() != cells.size()) {
            return false;
        }
        for (std::size_t index = 0; index < cells.size(); ++index) {
            const auto expected = bloom::render::displaySignalPixel(layout, cells[index]);
            if (std::bit_cast<std::array<std::uint32_t, 4>>(signal[index]) !=
                std::bit_cast<std::array<std::uint32_t, 4>>(expected)) {
                return false;
            }
        }
        return true;
    };
    const auto storedWide = runStored(mixedInput, DisplayPixelLayout::Rgb10A2);
    checks.expect(storedWide.poll == GpuOcioExecutorPollResult::Ready &&
                      unpacks(storedWide.pixels, DisplayPixelLayout::Rgb10A2, wideRun.cells) &&
                      storedWide.report == wideRun.report,
                  "stored signal 10:10:10:2: the picture is the packed cells, unpacked, and the "
                  "report is the pack's");
    const auto storedRefused = runStored(softInput, DisplayPixelLayout::Rgb10A2);
    checks.expect(storedRefused.poll == GpuOcioExecutorPollResult::Failure &&
                      storedRefused.code == GpuOcioExecutorDiagnosticCode::LayoutRefused &&
                      storedRefused.report.has_value() && !storedRefused.report->allowsRgb10A2 &&
                      storedRefused.pixels.empty(),
                  "stored signal: a soft frame refuses 10:10:10:2 and publishes no picture");
    const auto storedSoft = runStored(softInput, DisplayPixelLayout::Straight8888);
    checks.expect(storedSoft.poll == GpuOcioExecutorPollResult::Ready &&
                      unpacks(storedSoft.pixels, DisplayPixelLayout::Straight8888, softRun.cells) &&
                      storedSoft.report == softRun.report,
                  "stored signal 8:8:8:8: the picture is the packed cells, unpacked, and the "
                  "report is the pack's");
    // The float display step keeps the one layout.
    GpuOcioTransformSpec floatSpec = storedSpec;
    floatSpec.displayPacking = {};
    const auto floatCommand =
        preparer.prepare(config, floatSpec, geometry, compileOptions()).command;
    checks.expect(
        floatCommand != nullptr && floatCommand->identity() != storedCommand->identity() &&
            scopeExecutor.executor
                    ->begin(floatCommand, mixedInput, {}, kBudget, DisplayPixelLayout::Rgb10A2)
                    .code == GpuOcioExecutorDiagnosticCode::InvalidArgument,
        "stored signal: a scope display command without the packing is a command of "
        "its own and takes no layout");
}

#endif // BLOOM_GPUSHADER_TOOLS_DIR

[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildNeutralProcessor() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (!resolution.ready()) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!built) {
        return std::nullopt;
    }
    return std::move(built).takeHandle();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const bool requireDevice = parseRequireDevice(argc, argv);
        Expectations checks;
        auto processor = buildNeutralProcessor();
        checks.expect(processor.has_value(), "the Bloom Neutral CPU display processor builds");
        if (!processor.has_value()) {
            return 1;
        }
        auto device = GpuDevice::create(bloom::render::GpuDeviceCreationOptions{});
        if (!device) {
            if (requireDevice) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return kSkipExit;
        }
        {
            auto neutral = GpuNeutralDisplay::create(*device.device);
            checks.expect(neutral.hasValue(), "the neutral display pipeline is created");
            if (!neutral) {
                return 1;
            }
            testNeutral(checks, *neutral.display, *processor);
            testResident(checks, *device.device, *neutral.display);
        }
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
        const auto revision = bloom::color::ocioBuiltInContentRevision(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
            bloom::color::kBloomNeutralV1ConfigUri);
        checks.expect(revision.has_value(), "the Bloom Neutral built-in has a revision");
        if (revision.has_value()) {
            auto resolution = bloom::color::resolveOcioBuiltIn(
                bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                bloom::color::kBloomNeutralV1ConfigUri, *revision, std::string{});
            auto config = std::move(resolution).takeResolved();
            checks.expect(config.has_value(), "the Bloom Neutral built-in resolves");
            bloom::render::GpuProcessingPhaseBudget root;
            root.setHostCapacity(kBudget * 2, true);
            root.setDeviceCapacity(kBudget * 2, true);
            auto admission = root.reserve({.fixedHostBytes = 1,
                                           .requestedSourceBytes = kBudget / 4,
                                           .minimumSourceBytes = kBudget / 4,
                                           .requestedExecutorBytes = kBudget,
                                           .minimumExecutorBytes = kBudget,
                                           .sourceFree = false});
            checks.expect(admission.lease && device.device->retainProcessingPhase(admission.lease),
                          "the general display runs in a processing phase");
            if (config.has_value() && admission.lease) {
                testGeneralDisplay(checks, *device.device, *config);
                device.device->retireProcessingPhase(admission.lease->id(), true);
            }
            admission.lease.reset();
        }
#else
        if (requireDevice) {
            std::cerr << "FAIL: --require-device requested but the shader tools are not packaged\n";
            return 1;
        }
        std::cout << "NOTE: the shader tools are not packaged; the general display program was "
                     "not run\n";
#endif
        if (!checks.ok()) {
            std::cerr << "FAIL: device display packing\n";
            return 1;
        }
        std::cout << "PASS: device display packing in both layouts\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
