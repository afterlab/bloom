#include <bloom/runtime/scope_frame_measure.hpp>

#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/gpu_scope_measure.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

// OCIO's name for "the display's own color space" in a shared view.
constexpr std::string_view kUseDisplayName = "<USE_DISPLAY_NAME>";
// Pixels converted at a time by the CPU reference; cancellation is asked between chunks.
constexpr std::size_t kChunkPixels = 16384;

[[nodiscard]] ScopeFrameMeasure failed(std::string message) {
    ScopeFrameMeasure result;
    result.status = ScopeFrameStatus::Failed;
    result.message = std::move(message);
    return result;
}

[[nodiscard]] ScopeFrameMeasure cancelledMeasure() {
    ScopeFrameMeasure result;
    result.status = ScopeFrameStatus::Cancelled;
    return result;
}

[[nodiscard]] render::Rgba32f pixelOf(const float red, const float green, const float blue,
                                      const float alpha) noexcept {
    return std::bit_cast<render::Rgba32f>(std::array<float, 4>{red, green, blue, alpha});
}

[[nodiscard]] bool finite(const float a, const float b, const float c) noexcept {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c);
}

// A pixel the measure counts as not finite.
[[nodiscard]] render::Rgba32f notFinitePixel() noexcept {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    return pixelOf(nan, nan, nan, nan);
}

// The Display signal of `source`: straight color through the display transform, then alpha again.
// False when the processor could not run or on cancellation
// (`cancelled` set).
[[nodiscard]] bool displaySignal(const std::span<const render::Rgba32f> source,
                                 const ScopeSignalColor& color,
                                 const CancellationToken& cancellation,
                                 std::vector<render::Rgba32f>& signal, bool& cancelled) {
    const auto* const processor = color.displayProcessor();
    if (processor == nullptr)
        return false;
    signal.resize(source.size(), render::Rgba32f::transparent());
    std::vector<float> scratch(std::min(kChunkPixels, source.size()) * 4);
    for (std::size_t first = 0; first < source.size(); first += kChunkPixels) {
        if (cancellation.isCancellationRequested()) {
            cancelled = true;
            return false;
        }
        const std::size_t count = std::min(kChunkPixels, source.size() - first);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& pixel = source[first + index];
            const float alpha = pixel.alpha();
            float red = 0.0F;
            float green = 0.0F;
            float blue = 0.0F;
            if (finite(pixel.red(), pixel.green(), pixel.blue()) && std::isfinite(alpha) &&
                alpha != 0.0F) {
                red = pixel.red() / alpha;
                green = pixel.green() / alpha;
                blue = pixel.blue() / alpha;
                if (!finite(red, green, blue))
                    red = green = blue = 0.0F;
            }
            scratch[index * 4 + 0] = red;
            scratch[index * 4 + 1] = green;
            scratch[index * 4 + 2] = blue;
            scratch[index * 4 + 3] = alpha;
        }
        if (color::applyDisplayFloatChunk(*processor, std::span<float>(scratch).first(count * 4))
                .has_value())
            return false;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& pixel = source[first + index];
            const float alpha = pixel.alpha();
            if (!finite(pixel.red(), pixel.green(), pixel.blue()) || !std::isfinite(alpha)) {
                signal[first + index] = notFinitePixel();
                continue;
            }
            // A pixel with no alpha is stored as it is, as the device's display step stores it.
            if (alpha == 0.0F) {
                signal[first + index] = pixel;
                continue;
            }
            if (!finite(pixel.red() / alpha, pixel.green() / alpha, pixel.blue() / alpha)) {
                signal[first + index] = notFinitePixel();
                continue;
            }
            const float red = scratch[index * 4 + 0];
            const float green = scratch[index * 4 + 1];
            const float blue = scratch[index * 4 + 2];
            const float storedRed = red * alpha;
            const float storedGreen = green * alpha;
            const float storedBlue = blue * alpha;
            signal[first + index] = finite(storedRed, storedGreen, storedBlue)
                                        ? pixelOf(storedRed, storedGreen, storedBlue, alpha)
                                        : notFinitePixel();
        }
    }
    return true;
}

// The CIE XYZ of every pixel of a signal picture of `pixels` pixels, `pixelAt(index)` each (its
// stored color, not divided by alpha). A pixel that is not finite, or whose XYZ is not, is zero:
// the measure counts it as having no chromaticity.
template <typename PixelAt, typename Cancelled>
[[nodiscard]] bool xyzPictureOf(const std::size_t pixels, const PixelAt& pixelAt,
                                const ScopeSignalColor& color, const Cancelled& isCancelled,
                                std::vector<render::Rgba32f>& xyz, bool& cancelled) {
    xyz.assign(pixels, render::Rgba32f::transparent());
    const std::size_t chunk = std::min(kChunkPixels, pixels);
    std::vector<float> rgb(chunk * 3);
    std::vector<float> out(chunk * 3);
    for (std::size_t first = 0; first < pixels; first += kChunkPixels) {
        if (isCancelled()) {
            cancelled = true;
            return false;
        }
        const std::size_t count = std::min(kChunkPixels, pixels - first);
        for (std::size_t index = 0; index < count; ++index) {
            const render::Rgba32f pixel = pixelAt(first + index);
            const bool good = finite(pixel.red(), pixel.green(), pixel.blue());
            rgb[index * 3 + 0] = good ? pixel.red() : 0.0F;
            rgb[index * 3 + 1] = good ? pixel.green() : 0.0F;
            rgb[index * 3 + 2] = good ? pixel.blue() : 0.0F;
        }
        const auto rgbChunk = std::span<const float>(rgb).first(count * 3);
        const auto outChunk = std::span<float>(out).first(count * 3);
        if (!color.xyz().apply(rgbChunk, outChunk)) {
            // One result of the chunk is not finite (or the transform cannot run): convert the
            // pixels one at a time so only those are zero.
            for (std::size_t index = 0; index < count; ++index) {
                if (!color.xyz().apply(rgbChunk.subspan(index * 3, 3),
                                       outChunk.subspan(index * 3, 3))) {
                    outChunk[index * 3 + 0] = 0.0F;
                    outChunk[index * 3 + 1] = 0.0F;
                    outChunk[index * 3 + 2] = 0.0F;
                }
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            const render::Rgba32f pixel = pixelAt(first + index);
            if (!finite(pixel.red(), pixel.green(), pixel.blue()))
                continue;
            xyz[first + index] = pixelOf(outChunk[index * 3 + 0], outChunk[index * 3 + 1],
                                         outChunk[index * 3 + 2], pixel.alpha());
        }
    }
    return true;
}

[[nodiscard]] bool xyzPicture(const std::span<const render::Rgba32f> signal,
                              const ScopeSignalColor& color, const CancellationToken& cancellation,
                              std::vector<render::Rgba32f>& xyz, bool& cancelled) {
    return xyzPictureOf(
        signal.size(), [&signal](const std::size_t index) { return signal[index]; }, color,
        [&cancellation] { return cancellation.isCancellationRequested(); }, xyz, cancelled);
}

[[nodiscard]] std::string deviceMessage(const GpuScopeMeasureOutcome& outcome) {
    return outcome.diagnostic.message.empty()
               ? std::string("The device route could not measure the frame.")
               : outcome.diagnostic.message;
}

} // namespace

ScopeSignalColorResult ScopeSignalColor::prepare(const ScopeSignalColorRequest& request) {
    ScopeSignalColorResult result;
    if (request.config == nullptr) {
        result.error = ScopeSignalColorError::InvalidRequest;
        result.message = "The scope signal names no color config.";
        return result;
    }
    const auto& config = *request.config;
    std::shared_ptr<ScopeSignalColor> color(new ScopeSignalColor());
    color->config_ = request.config;
    color->signal_ = request.signal;
    if (request.signal == ScopeSignal::Display) {
        color->display_ =
            request.display.empty() ? std::string(config.displayName()) : request.display;
        color->view_ = request.view.empty() ? std::string(config.viewName()) : request.view;
        const auto displays = config.displays();
        const auto entry = std::find_if(displays.begin(), displays.end(), [&](const auto& pair) {
            return pair.display == color->display_ && pair.view == color->view_;
        });
        if (entry == displays.end()) {
            result.error = ScopeSignalColorError::DisplayUnavailable;
            result.message = "The color config has no such display and view.";
            return result;
        }
        color->colorSpaceId_ =
            entry->colourSpaceId.empty() || entry->colourSpaceId == kUseDisplayName
                ? color->display_
                : entry->colourSpaceId;
        auto handle = color::buildCpuDisplayProcessorForView(config, color->display_, color->view_)
                          .takeHandle();
        if (!handle) {
            result.error = ScopeSignalColorError::DisplayUnavailable;
            result.message = "The display transform could not be prepared.";
            return result;
        }
        color->displayProcessor_ =
            std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(std::move(*handle));
    } else {
        color->colorSpaceId_ = std::string(config.processColorSpaceId());
    }
    auto xyz = color::ScopeXyzProcessor::prepare(config, color->colorSpaceId_);
    if (!xyz) {
        result.error = ScopeSignalColorError::XyzUnavailable;
        result.message = "The signal's color space has no conversion to CIE XYZ in this config.";
        return result;
    }
    color->xyz_ = std::move(xyz.processor);
    const auto& luma = color->xyz_->colorimetry().luma;
    color->luma_ = scopeLuma(luma[0], luma[1], luma[2]);
    const auto revision = color->xyz_->configRevision().toLowercaseHex();
    color->identity_ = std::string(revision.data(), revision.size()) + '|' +
                       color->xyz_->identity() + '|' +
                       (request.signal == ScopeSignal::Display ? "display" : "working") + '|' +
                       color->display_ + '|' + color->view_;
    result.color = std::move(color);
    return result;
}

ScopeFrameMeasure measureScopeFrameCpu(const ProcessFrame& frame,
                                       const ScopeMeasureSettings& settings,
                                       const ScopeSignalColor& color,
                                       const CancellationToken& cancellation) {
    try {
        const auto& image = frame.processImage();
        const auto* const descriptor = image.descriptor();
        const auto& plan = frame.identity().plan;
        if (descriptor == nullptr || plan == nullptr)
            return failed("The frame has no picture to measure.");
        if (settings.signal != color.signal())
            return failed("The scope signal and its color objects do not match.");
        const auto area = scopeMeasuredArea(descriptor->dataWindow(), descriptor->displayWindow(),
                                            frame.identity().roi, settings.region,
                                            plan->format().width(), plan->format().height());
        ScopeFrameMeasure result;
        result.route = ScopeRoute::Cpu;
        if (!area) {
            result.status = ScopeFrameStatus::EmptyArea;
            return result;
        }
        result.area = *area;

        const auto source = image.pixels();
        std::vector<render::Rgba32f> displayed;
        std::span<const render::Rgba32f> signal = source;
        bool cancelled = false;
        if (settings.signal == ScopeSignal::Display) {
            if (!displaySignal(source, color, cancellation, displayed, cancelled))
                return cancelled ? cancelledMeasure()
                                 : failed("The display transform could not run on the frame.");
            signal = displayed;
        }
        std::vector<render::Rgba32f> xyz;
        if ((settings.scopes & render::kScopeTableChromaticity) != 0U) {
            if (!xyzPicture(signal, color, cancellation, xyz, cancelled))
                return cancelled ? cancelledMeasure()
                                 : failed("The conversion to CIE XYZ could not run on the frame.");
        }
        const auto extent = descriptor->dataWindow().extent();
        ScopeMeasureInput input;
        input.signal = signal;
        input.width = static_cast<std::uint32_t>(extent.width());
        input.height = static_cast<std::uint32_t>(extent.height());
        input.xyz = xyz;
        input.areaX = area->x;
        input.areaY = area->y;
        input.areaWidth = area->width;
        input.areaHeight = area->height;
        input.scopes = settings.scopes;
        input.axisMaximum = settings.axisMaximum;
        input.luma = color.luma();
        input.ignoreTransparent = settings.ignoreTransparent;
        auto counts = measureScopesCpu(
            input, [&cancellation] { return cancellation.isCancellationRequested(); });
        if (!counts)
            return cancellation.isCancellationRequested()
                       ? cancelledMeasure()
                       : failed("The scope settings are not valid for this frame.");
        result.status = ScopeFrameStatus::Measured;
        result.counts = std::move(*counts);
        return result;
    } catch (const std::bad_alloc&) {
        return failed("The scope measure could not allocate its pictures.");
    }
}

ScopeFrameMeasure measureScopeStoredFrameCpu(const ScopeStoredFrame& frame,
                                             const ScopeMeasureSettings& settings,
                                             const render::ScopeTableReduction reduction,
                                             const ScopeSignalColor& color,
                                             const std::function<bool()>& cancelled) {
    try {
        const auto isCancelled = [&cancelled] { return cancelled && cancelled(); };
        if (settings.signal != ScopeSignal::Display || color.signal() != ScopeSignal::Display ||
            settings.axisMaximum != 1U)
            return failed("A stored display frame is measured as the Display signal.");
        const auto extent = frame.dataWindow.extent();
        const std::uint64_t width = extent.width();
        const std::uint64_t height = extent.height();
        if (frame.cells.empty() || width == 0 || height == 0 ||
            width > std::numeric_limits<std::uint32_t>::max() ||
            height > std::numeric_limits<std::uint32_t>::max() ||
            frame.cells.size() != width * height)
            return failed("The stored frame has no picture to measure.");
        const auto area =
            scopeMeasuredArea(frame.dataWindow, frame.displayWindow, frame.roi, settings.region,
                              frame.compositionWidth, frame.compositionHeight);
        ScopeFrameMeasure result;
        result.route = ScopeRoute::Cpu;
        if (!area) {
            result.status = ScopeFrameStatus::EmptyArea;
            return result;
        }
        result.area = *area;

        // Only the rows the area reaches are read: a region's measure touches no other cell.
        const auto firstRow = static_cast<std::uint64_t>(
            std::clamp<std::int64_t>(area->y, 0, static_cast<std::int64_t>(height)));
        const auto endRow = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(area->y) + static_cast<std::int64_t>(area->height), 0,
            static_cast<std::int64_t>(height)));
        if (endRow <= firstRow) {
            result.status = ScopeFrameStatus::EmptyArea;
            return result;
        }
        const auto rows = frame.cells.subspan(static_cast<std::size_t>(firstRow * width),
                                              static_cast<std::size_t>((endRow - firstRow) * width));
        std::vector<render::Rgba32f> xyz;
        if ((settings.scopes & render::kScopeTableChromaticity) != 0U) {
            bool wasCancelled = false;
            const auto layout = frame.layout;
            if (!xyzPictureOf(
                    rows.size(),
                    [&rows, layout](const std::size_t index) {
                        return std::bit_cast<render::Rgba32f>(
                            render::displaySignalPixel(layout, rows[index]));
                    },
                    color, isCancelled, xyz, wasCancelled))
                return wasCancelled
                           ? cancelledMeasure()
                           : failed("The conversion to CIE XYZ could not run on the frame.");
        }
        ScopeMeasureInput input;
        input.packed = rows;
        input.packedLayout = frame.layout;
        input.width = static_cast<std::uint32_t>(width);
        input.height = static_cast<std::uint32_t>(endRow - firstRow);
        input.xyz = xyz;
        input.areaX = area->x;
        input.areaY = area->y - static_cast<std::int32_t>(firstRow);
        input.areaWidth = area->width;
        input.areaHeight = area->height;
        input.scopes = settings.scopes;
        input.axisMaximum = settings.axisMaximum;
        input.luma = color.luma();
        input.ignoreTransparent = settings.ignoreTransparent;
        input.reduction = reduction;
        auto counts = measureScopesCpu(input, isCancelled);
        if (!counts)
            return isCancelled() ? cancelledMeasure()
                                 : failed("The scope settings are not valid for this frame.");
        result.status = ScopeFrameStatus::Measured;
        result.counts = std::move(*counts);
        return result;
    } catch (const std::bad_alloc&) {
        return failed("The scope measure could not allocate its pictures.");
    }
}

GpuScopeMeasureJob makeScopeMeasureJob(const ScopeMeasureSettings& settings,
                                       const ScopeSignalColor& color,
                                       const GpuSceneOcioContext& ocio,
                                       std::shared_ptr<const void> keepAlive) {
    GpuScopeMeasureJob job;
    job.scopes = settings.scopes;
    job.axisMaximum = settings.axisMaximum;
    job.luma = color.luma();
    job.ignoreTransparent = settings.ignoreTransparent;
    job.region = settings.region;
    const auto factory = [&ocio, &color,
                          &keepAlive](GpuOcioTransformSpec spec) -> GpuScopeCommandFactory {
        return
            [&ocio, &color, keepAlive, spec = std::move(spec)](
                const GpuOcioCommandGeometry geometry,
                const CancellationToken& token) -> std::shared_ptr<const PreparedGpuOcioCommand> {
                if (ocio.preparer == nullptr)
                    return nullptr;
                return ocio.preparer
                    ->prepare(color.config(), spec, geometry, ocio.compileOptions,
                              [&token] { return token.isCancellationRequested(); })
                    .command;
            };
    };
    if (settings.signal == ScopeSignal::Display) {
        GpuOcioTransformSpec spec;
        spec.kind = GpuOcioTransformKind::ScopeDisplay;
        spec.display = color.display();
        spec.view = color.view();
        spec.workingSpaceId = std::string(color.config().processColorSpaceId());
        job.displayCommand = factory(std::move(spec));
    }
    if ((settings.scopes & render::kScopeTableChromaticity) != 0U) {
        GpuOcioTransformSpec spec;
        spec.kind = GpuOcioTransformKind::ScopeXyz;
        spec.fromId = color.colorSpaceId();
        job.xyzCommand = factory(std::move(spec));
    }
    return job;
}

ScopeFrameMeasure
measureScopeFrameOnDevice(GpuProcessFrameEvaluator& evaluator, const GpuSceneOcioContext& ocio,
                          std::shared_ptr<const CompiledCompositionPlan> plan,
                          const EvaluationRequest& request, const ScopeMeasureSettings& settings,
                          const ScopeSignalColor& color, const CancellationToken& cancellation,
                          const std::optional<std::filesystem::path>& assetBaseDirectory) {
    if (settings.signal != color.signal())
        return failed("The scope signal and its color objects do not match.");
    const auto job = makeScopeMeasureJob(settings, color, ocio);
    auto outcome =
        evaluator.measureScopes(std::move(plan), request, job, cancellation, assetBaseDirectory);
    ScopeFrameMeasure result;
    result.route = ScopeRoute::Gpu;
    result.area = outcome.area;
    result.colorDispatches = outcome.colorDispatches;
    result.tableReadbacks = outcome.tableReadbacks;
    result.pictureReadbacks = outcome.pictureReadbacks;
    switch (outcome.status) {
    case GpuScopeMeasureStatus::Measured:
        result.status = ScopeFrameStatus::Measured;
        result.counts = std::move(outcome.counts);
        result.tableReadbackBytes = outcome.tableReadbackBytes;
        break;
    case GpuScopeMeasureStatus::EmptyArea:
        result.status = ScopeFrameStatus::EmptyArea;
        break;
    case GpuScopeMeasureStatus::Cancelled:
        result.status = ScopeFrameStatus::Cancelled;
        break;
    case GpuScopeMeasureStatus::Unavailable:
    case GpuScopeMeasureStatus::Failed:
        // A frame the device cannot count (no room, a scene outside its subset, a pixel that is
        // not finite, which its color step refuses) is counted by the CPU reference instead.
        result.status = ScopeFrameStatus::Unavailable;
        result.message = deviceMessage(outcome);
        break;
    }
    return result;
}

ScopeFrameMeasure measureScopeFrame(
    const ScopeFrameRoutes& routes, const std::shared_ptr<const CompiledCompositionPlan>& plan,
    const EvaluationRequest& request, const ScopeMeasureSettings& settings,
    const ScopeSignalColor& color, const bool preferDevice, const CancellationToken& cancellation,
    const std::optional<std::filesystem::path>& assetBaseDirectory) {
    std::string deviceReason;
    if (preferDevice) {
        const auto device = routes.device ? routes.device() : nullptr;
        const auto ocio = routes.ocio ? routes.ocio() : nullptr;
        if (device == nullptr || ocio == nullptr) {
            deviceReason = "The device route is not ready to measure the frame.";
        } else {
            auto measured = measureScopeFrameOnDevice(*device, *ocio, plan, request, settings,
                                                      color, cancellation, assetBaseDirectory);
            if (measured.status != ScopeFrameStatus::Unavailable)
                return measured;
            deviceReason = std::move(measured.message);
        }
    }
    if (cancellation.isCancellationRequested())
        return cancelledMeasure();
    if (!routes.evaluateCpu)
        return failed("No CPU evaluator is available to measure the frame.");
    const auto evaluated = routes.evaluateCpu(plan, request, cancellation);
    if (evaluated.status() == EvaluationStatus::Cancelled)
        return cancelledMeasure();
    if (evaluated.frame() == nullptr) {
        return failed(evaluated.diagnostics().empty()
                          ? std::string("The frame could not be evaluated.")
                          : evaluated.diagnostics().front().summary);
    }
    auto measured = measureScopeFrameCpu(*evaluated.frame(), settings, color, cancellation);
    if (measured.status == ScopeFrameStatus::Measured)
        measured.message = std::move(deviceReason);
    return measured;
}

} // namespace bloom::runtime
