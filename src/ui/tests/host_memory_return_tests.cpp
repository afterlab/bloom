// Issue #673. Memory Bloom frees after a large render has to go back to the operating system:
// a cache that evicts a gigabyte of frames relieves nothing while the allocator keeps the
// gigabyte charged to the process. The macOS allocator keeps freed large blocks that way, so
// every frame-sized buffer on the frame, media and cache paths is mapped from the system
// (core::allocateHostBlock) and unmapped when freed.
//
// Each case here takes what a large render leaves in memory, in the real types that hold it, a
// gigabyte or so at a time with every page written, lets it go, and requires the process's
// memory to come back to where it started. Asserted on macOS (physical footprint) and Linux
// (resident set); other platforms run the cases and print what they would have measured.
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/composite_frame_cache.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/memory_pressure_monitor.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#endif

namespace {
using namespace bloom;

constexpr std::size_t kMebibyte = std::size_t{1024} * 1024;
constexpr std::uint32_t kUhdWidth = 3840;
constexpr std::uint32_t kUhdHeight = 2160;

int failures = 0;
void expect(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

// What the operating system charges the process for, or 0 where this test has no reading.
[[nodiscard]] std::size_t chargedBytes() {
#if defined(__APPLE__)
    rusage_info_v4 usage{};
    if (::proc_pid_rusage(::getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage)) ==
        0)
        return static_cast<std::size_t>(usage.ri_phys_footprint);
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        constexpr std::string_view label = "VmRSS:";
        if (std::string_view(line).starts_with(label))
            return static_cast<std::size_t>(
                       std::strtoull(line.c_str() + label.size(), nullptr, 10)) *
                   1024U;
    }
#endif
    return 0;
}

// A UHD float frame with every row written, as an evaluated frame is.
[[nodiscard]] std::shared_ptr<const render::Rgba32fImage> renderedFrame(const float seed) {
    const auto window = render::ImageWindow::create(0, 0, kUhdWidth, kUhdHeight);
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(
        *descriptor.value(), std::size_t{kUhdWidth} * kUhdHeight * 16U * 2U);
    const auto pixel = render::Rgba32f::fromPremultiplied(seed, seed * 0.5F, 0.25F, 1.0F);
    for (std::uint32_t y = 0; y < kUhdHeight; ++y) {
        auto row = builder.value()->row(y);
        std::fill((*row.value()).begin(), (*row.value()).end(), *pixel.value());
    }
    auto frozen = std::move(*builder.value()).freeze();
    return std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
}

// Holds what `fill` builds (it reports how many bytes that is), lets it go, and requires the
// charge to return to within `slack` of where it started.
void returnsToStart(const std::string& what, const std::function<std::size_t()>& fillAndRelease,
                    const std::function<std::size_t()>& peakOf) {
    constexpr std::size_t slack = 96 * kMebibyte;
    runtime::releaseFreedHostMemory();
    const auto start = chargedBytes();
    const auto held = fillAndRelease();
    const auto peak = peakOf();
    runtime::releaseFreedHostMemory();
    const auto end = chargedBytes();
    std::cout << "HOST_MEMORY_RETURN " << what << ": held " << held / kMebibyte << " MiB, start "
              << start / kMebibyte << " MiB, peak " << peak / kMebibyte << " MiB, end "
              << end / kMebibyte << " MiB\n";
    if (start == 0 || end == 0)
        return;
    expect(peak >= start + held / 2, what + ": the memory was really in use while held");
    expect(end <= start + slack, what + ": the charge returns to within 96 MiB of its start");
}

// One case: `make(index)` builds one retained object and returns it with its size.
template <typename Make> void runCase(const std::string& what, const int count, Make&& make) {
    std::size_t peak = 0;
    returnsToStart(
        what,
        [&] {
            std::size_t held = 0;
            {
                std::vector<decltype(make(0).first)> retained;
                retained.reserve(static_cast<std::size_t>(count));
                for (int index = 0; index < count; ++index) {
                    auto made = make(index);
                    held += made.second;
                    retained.push_back(std::move(made.first));
                }
                peak = chargedBytes();
            }
            return held;
        },
        [&] { return peak; });
}

void testFootprintReturnsAfterLargeRender() {
    runtime::configureHostMemoryReturn();

    // Evaluated frames, as the operation cache and the RAM preview hold them: 8 UHD float
    // frames, 1 GiB.
    runCase("evaluated float frames", 8, [](const int index) {
        auto frame = renderedFrame(static_cast<float>(index) / 8.0F);
        return std::pair{std::move(frame), std::size_t{kUhdWidth} * kUhdHeight * 16U};
    });

    // The composite frame cache's half-float frames: 16 UHD frames, 1 GiB.
    const auto source = renderedFrame(0.5F);
    runCase("composite cache frames", 16, [&source](int) {
        auto frame = runtime::CompositeFrame::fromImage(*source);
        const auto bytes = frame ? static_cast<std::size_t>(frame->bytes()) : std::size_t{0};
        return std::pair{std::move(frame), bytes};
    });

    // Decoded video frames as the media worker hands them over and the playback cache keeps
    // them: 64 frames of 1080p 16-bit RGBA, 1 GiB.
    runCase("decoded video frames", 64, [](const int index) {
        auto frame = std::make_shared<media::provider::FrameProduct>();
        frame->format = media::provider::PixelFormat::Rgba16;
        media::provider::CpuPlane plane;
        plane.width = 1920;
        plane.height = 1080;
        plane.stride = 1920 * 8;
        plane.bytes.assign(static_cast<std::size_t>(plane.stride) * plane.height,
                           static_cast<std::byte>(index + 1));
        const auto bytes = plane.bytes.size();
        frame->planes.push_back(std::move(plane));
        return std::pair{std::move(frame), bytes};
    });

    // The same frames inside the decoded video cache, released by the cache itself when its
    // budget falls.
    {
        runtime::MemoryBudgetLedger ledger(std::size_t{16} * 1024 * kMebibyte,
                                           std::size_t{16} * 1024 * kMebibyte);
        std::size_t peak = 0;
        returnsToStart(
            "decoded video cache",
            [&] {
                media::video::DecodedVideoCache cache(std::size_t{2} * 1024 * kMebibyte, ledger);
                for (int index = 0; index < 64; ++index) {
                    auto frame = std::make_shared<media::provider::FrameProduct>();
                    frame->format = media::provider::PixelFormat::Rgba16;
                    frame->colour = {1, 1, 1, 1};
                    media::provider::CpuPlane plane;
                    plane.width = 1920;
                    plane.height = 1080;
                    plane.stride = 1920 * 8;
                    plane.bytes.assign(static_cast<std::size_t>(plane.stride) * plane.height,
                                       static_cast<std::byte>(index + 1));
                    frame->planes.push_back(std::move(plane));
                    cache.store({{}, 0, static_cast<std::uint64_t>(index), 0, {}, {}, {}},
                                std::move(frame));
                }
                const auto held = cache.residentBytes();
                peak = chargedBytes();
                return held;
            },
            [&] { return peak; });
    }
}
} // namespace

int main() {
    testFootprintReturnsAfterLargeRender();
    if (failures != 0) {
        std::cerr << failures << " host memory return expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: freed frame memory goes back to the operating system\n";
    return 0;
}
