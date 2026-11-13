// bloom.scripting.scope-read-native (#724 Scopes): the scripting read on a real device.
//
// A 64 x 64 composition of eight color bars is read through scripting::readScopes with the device
// routes of a real GpuExportProvider (headlessScopeRoutes), the routes MCP and Python use in a
// headless session, and against the CPU reference of the same frame (the same read with no device).
// Asserted:
//   * the read reports route gpu with no routeNote, and every pixel of the frame is counted;
//   * the Working signal's Histogram, Waveform and Vectorscope equal the CPU reference's, cell for
//     cell;
//   * through the OCIO steps (the Display signal, and the CIE table) every pixel is counted and no
//     pixel is more than one cell from where the CPU reference puts it along each table axis (the
//     comparison of bloom.runtime.scope_frame_measure_native);
//   * a provider with no device answers with route cpu and a routeNote saying so.
// The read exposes no count of picture readbacks, so that proof stays in
// bloom.runtime.scope_frame_measure_native. Skips (exit 77) without a device or the packaged tools,
// and fails under --require-device.

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/scope_read.hpp>
#include <bloom/scripting/session.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
namespace render_ns = bloom::render;

int gFailures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++gFailures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

// Eight bars of 8 x 64 pixels, as 75% linear colors.
constexpr std::array<std::array<double, 3>, 8> kBars{{{0.75, 0.75, 0.75},
                                                      {0.75, 0.75, 0.0},
                                                      {0.0, 0.75, 0.75},
                                                      {0.0, 0.75, 0.0},
                                                      {0.75, 0.0, 0.75},
                                                      {0.75, 0.0, 0.0},
                                                      {0.0, 0.0, 0.75},
                                                      {0.0, 0.0, 0.0}}};

struct Bars final {
    std::unique_ptr<scripting::Session> session;
    document::CompositionId composition;

    Bars() {
        auto created = scripting::Session::createNew(
            "Scopes", "Bars", core::RationalTime::fromInteger(1),
            *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(24, 1)));
        if (!created)
            std::abort();
        session = std::move(created).takeSession();
        composition = session->snapshot().project().compositions().front().id();
        for (std::size_t index = 0; index < kBars.size(); ++index) {
            const auto added = run<commands::AddSolidLayer>(
                composition, "Bar " + std::to_string(index),
                core::Color4d{kBars[index][0], kBars[index][1], kBars[index][2], 1.0},
                document::Vec2d{8.0 * static_cast<double>(index) + 4.0, 32.0});
            const auto solid =
                added.command->outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput)
                    .value();
            run<commands::SetParameterSource>(composition,
                                              role(solid, document::kSolidWidthParameterRole),
                                              document::ConstantValueSource{8.0});
            run<commands::SetParameterSource>(composition,
                                              role(solid, document::kSolidHeightParameterRole),
                                              document::ConstantValueSource{64.0});
        }
    }

    template <typename Operation, typename... Arguments>
    scripting::SessionCommandResult run(Arguments&&... args) {
        commands::Transaction transaction("Bars", session->snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(args)...);
        auto result = session->execute(std::move(transaction));
        expect(result.succeeded(), "a fixture command applies");
        return result;
    }

    [[nodiscard]] document::ParameterId role(const document::NodeId node,
                                             const std::string_view name) const {
        const auto snapshot = session->snapshot();
        const auto* record =
            snapshot.project().findComposition(composition)->graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == name)
                return binding.parameterId;
        return {};
    }
};

// ---- The comparison of bloom.runtime.scope_frame_measure_native --------------------------------

// True when no count of `left` is more than one cell from where `right` has it, along one axis:
// each running total of one lies between the other's running totals one cell before and at it.
[[nodiscard]] bool withinOneCell(const std::vector<std::uint64_t>& left,
                                 const std::vector<std::uint64_t>& right) {
    if (left.size() != right.size())
        return false;
    std::uint64_t leftTotal = 0;
    std::uint64_t rightTotal = 0;
    std::uint64_t leftBefore = 0;
    std::uint64_t rightBefore = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        leftTotal += left[index];
        rightTotal += right[index];
        if (leftBefore > rightTotal || rightBefore > leftTotal)
            return false;
        leftBefore = leftTotal;
        rightBefore = rightTotal;
    }
    return leftTotal == rightTotal;
}

[[nodiscard]] std::vector<std::uint64_t> histogramAxis(const runtime::ScopeCounts& counts,
                                                       const std::uint32_t channel) {
    std::vector<std::uint64_t> axis;
    axis.push_back(counts.histogram(channel, render_ns::kScopeCodeUnder));
    for (std::uint32_t code = 0; code < 1024; ++code)
        axis.push_back(counts.histogram(channel, code));
    axis.push_back(counts.histogram(channel, render_ns::kScopeCodeOver));
    return axis;
}

[[nodiscard]] std::vector<std::uint64_t> waveformAxis(const runtime::ScopeCounts& counts,
                                                      const std::uint32_t channel,
                                                      const std::uint32_t column) {
    std::vector<std::uint64_t> axis;
    axis.push_back(counts.waveform(channel, column, render_ns::kScopeWaveformLevelUnder));
    for (std::uint32_t level = 0; level < 512; ++level)
        axis.push_back(counts.waveform(channel, column, level));
    axis.push_back(counts.waveform(channel, column, render_ns::kScopeWaveformLevelOver));
    return axis;
}

// A 512 x 512 table summed onto one of its axes, with the pixels outside it as a last cell.
template <typename Cell>
[[nodiscard]] std::vector<std::uint64_t> planeAxis(const Cell& cell, const bool alongFirst,
                                                   const std::uint32_t outside) {
    std::vector<std::uint64_t> axis(513, 0);
    for (std::uint32_t first = 0; first < 512; ++first) {
        for (std::uint32_t second = 0; second < 512; ++second)
            axis[alongFirst ? first : second] += cell(first, second);
    }
    axis[512] = outside;
    return axis;
}

// The pixels outside a 512 x 512 table are one count for both axes, so a pixel that crosses the
// table's edge is not placed along an axis: the axes are compared only when both sides agree on
// how many are outside.
[[nodiscard]] bool planesWithinOneCell(const runtime::ScopeCounts& device,
                                       const runtime::ScopeCounts& reference,
                                       const bool chromaticity) {
    const auto cellOf = [chromaticity](const runtime::ScopeCounts& counts) {
        return [&counts, chromaticity](const std::uint32_t first, const std::uint32_t second) {
            return chromaticity ? counts.chromaticity(first, second)
                                : counts.vectorscope(first, second);
        };
    };
    const auto outsideOf = [chromaticity](const runtime::ScopeCounts& counts) {
        return chromaticity ? counts.chromaticityOutside() + counts.noChromaticity()
                            : counts.vectorscopeOutside();
    };
    for (const bool alongFirst : {true, false}) {
        auto left = planeAxis(cellOf(device), alongFirst, outsideOf(device));
        auto right = planeAxis(cellOf(reference), alongFirst, outsideOf(reference));
        if (left[512] != right[512]) {
            // Some pixels sit on the table's edge: only the totals can be compared.
            std::uint64_t leftTotal = 0;
            std::uint64_t rightTotal = 0;
            for (std::size_t index = 0; index < left.size(); ++index) {
                leftTotal += left[index];
                rightTotal += right[index];
            }
            if (leftTotal != rightTotal)
                return false;
            continue;
        }
        if (!withinOneCell(left, right))
            return false;
    }
    return true;
}

[[nodiscard]] bool tablesWithinOneCell(const runtime::ScopeCounts& device,
                                       const runtime::ScopeCounts& reference) {
    if (device.layout != reference.layout || device.counted() != reference.counted() ||
        device.notFinite() != reference.notFinite())
        return false;
    const auto& layout = device.layout;
    for (std::uint32_t channel = 0; channel < render_ns::kScopeChannels; ++channel) {
        if (layout.has(render_ns::kScopeTableHistogram) &&
            !withinOneCell(histogramAxis(device, channel), histogramAxis(reference, channel)))
            return false;
        if (layout.has(render_ns::kScopeTableWaveform)) {
            for (std::uint32_t column = 0; column < layout.columns; ++column) {
                if (!withinOneCell(waveformAxis(device, channel, column),
                                   waveformAxis(reference, channel, column)))
                    return false;
            }
        }
    }
    if (layout.has(render_ns::kScopeTableVectorscope) &&
        !planesWithinOneCell(device, reference, false))
        return false;
    if (layout.has(render_ns::kScopeTableChromaticity) &&
        !planesWithinOneCell(device, reference, true))
        return false;
    return true;
}

// ---- Reads -------------------------------------------------------------------------------------

[[nodiscard]] scripting::ScopeReadFrame read(const Bars& bars, runtime::TaskScheduler& scheduler,
                                             const runtime::ScopeFrameRoutes& routes,
                                             const runtime::ScopeSignal signal,
                                             const std::uint32_t scopes) {
    scripting::ScopeReadRequest request;
    request.composition = bars.composition;
    request.signal = signal;
    request.scopes = scopes;
    scripting::ScopeReadPorts ports;
    ports.scheduler = &scheduler;
    ports.routes = routes;
    const auto result = scripting::readScopes(*bars.session, request, ports);
    expect(result.ok() && result.frames.size() == 1,
           "the read measures the frame: " + result.error.message);
    return result.ok() && result.frames.size() == 1 ? result.frames.front()
                                                    : scripting::ScopeReadFrame{};
}

[[nodiscard]] runtime::GpuProcessFrameEvaluatorOptions enabledOptions(const std::string& loader) {
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    // The production packaged resolver: the same packaged GPU shader tools the desktop, CLI and MCP
    // roots compose from their own target definitions.
    options.ocioResolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
    return options;
}

// A scheduler for the reads; it outlives them.
struct Reads final {
    runtime::TaskScheduler scheduler;
    ~Reads() {
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent())
            std::this_thread::yield();
    }
};

} // namespace

int main(int argc, char** argv) {
    bool requireDevice = false;
    for (int index = 1; index < argc; ++index)
        if (std::string_view(argv[index]) == "--require-device")
            requireDevice = true;
    try {
        const Bars bars;
        Reads reads;
        auto& scheduler = reads.scheduler;
        const auto cancelled = [] { return false; };

        // No device: the same read counts with the CPU reference and says why.
        {
            runtime::GpuProcessFrameEvaluatorOptions disabled;
            disabled.enabled = false;
            auto provider = host::GpuExportProvider::create(disabled);
            const auto routes = scripting::headlessScopeRoutes(provider, scheduler, cancelled);
            const auto frame = read(bars, scheduler, routes, runtime::ScopeSignal::Working,
                                    render_ns::kScopeTableHistogram);
            expect(frame.route == runtime::ScopeRoute::Cpu && !frame.routeNote.empty() &&
                       frame.counted == 64U * 64U,
                   "with no device the read is counted by the CPU reference and carries a note");
            provider->beginShutdown();
            while (!provider->retirementComplete())
                std::this_thread::yield();
            static_cast<void>(provider->collectRetired());
        }

        const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
        if (loader == nullptr || *loader == '\0') {
            if (requireDevice) {
                std::cerr << "FAIL: --require-device needs BLOOM_TEST_VULKAN_LOADER\n";
                return 1;
            }
            std::cout << "SKIP: BLOOM_TEST_VULKAN_LOADER is not set\n";
            return gFailures != 0 ? 1 : 77;
        }
        auto provider = host::GpuExportProvider::create(enabledOptions(loader));
        provider->prepare(scheduler);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!provider->prepared() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        int exitCode = 0;
        if (!provider->deviceAvailable() || !provider->gpuDisplayPreparationAvailable()) {
            if (requireDevice) {
                std::cerr << "FAIL: a device and the packaged tools are required\n";
                exitCode = 1;
            } else {
                std::cout << "SKIP: no compatible device or packaged tools\n";
                exitCode = gFailures != 0 ? 1 : 77;
            }
        } else {
            const auto device = scripting::headlessScopeRoutes(provider, scheduler, cancelled);
            const runtime::ScopeFrameRoutes none;
            constexpr auto exactScopes = render_ns::kScopeTableHistogram |
                                         render_ns::kScopeTableWaveform |
                                         render_ns::kScopeTableVectorscope;

            // The Working signal's Histogram, Waveform and Vectorscope pass through no OCIO step.
            {
                const auto gpu =
                    read(bars, scheduler, device, runtime::ScopeSignal::Working, exactScopes);
                const auto cpu =
                    read(bars, scheduler, none, runtime::ScopeSignal::Working, exactScopes);
                expect(gpu.route == runtime::ScopeRoute::Gpu && gpu.routeNote.empty(),
                       "the Working read is counted on the device with no note");
                expect(cpu.route == runtime::ScopeRoute::Cpu && !cpu.routeNote.empty(),
                       "the same read with no device is the CPU reference, with a note");
                expect(gpu.counted == 64U * 64U && cpu.counted == 64U * 64U,
                       "every pixel is counted by both routes");
                expect(gpu.counts == cpu.counts,
                       "the Working signal's Histogram, Waveform and Vectorscope equal the CPU "
                       "reference cell for cell");
            }
            // The CIE table goes through an OCIO program: one cell along each axis.
            {
                const auto gpu = read(bars, scheduler, device, runtime::ScopeSignal::Working,
                                      render_ns::kScopeTableAll);
                const auto cpu = read(bars, scheduler, none, runtime::ScopeSignal::Working,
                                      render_ns::kScopeTableAll);
                expect(gpu.route == runtime::ScopeRoute::Gpu && gpu.counted == cpu.counted &&
                           gpu.counted == 64U * 64U,
                       "the Working read of all five tables is counted on the device");
                expect(tablesWithinOneCell(gpu.counts, cpu.counts),
                       "the CIE table is within one cell of the CPU reference along each axis");
            }
            // The Display signal goes through the display transform.
            {
                const auto gpu = read(bars, scheduler, device, runtime::ScopeSignal::Display,
                                      render_ns::kScopeTableAll);
                const auto cpu = read(bars, scheduler, none, runtime::ScopeSignal::Display,
                                      render_ns::kScopeTableAll);
                expect(gpu.route == runtime::ScopeRoute::Gpu && gpu.routeNote.empty() &&
                           gpu.counted == cpu.counted && gpu.counted == 64U * 64U,
                       "the Display read is counted on the device, every pixel counted");
                expect(tablesWithinOneCell(gpu.counts, cpu.counts),
                       "every Display table is within one cell of the CPU reference along each "
                       "axis");
            }
        }
        provider->beginShutdown();
        while (!provider->retirementComplete())
            std::this_thread::yield();
        static_cast<void>(provider->collectRetired());
        if (exitCode != 0)
            return exitCode;
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (gFailures != 0) {
        std::cerr << "scope read native: " << gFailures << " failure(s)\n";
        return 1;
    }
    std::cout << "scope read native: ok\n";
    return 0;
}
