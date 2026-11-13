#include <bloom/runtime/scope_frame_measure.hpp>

#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/gpu_scope_measure.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

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

// The Display signal of `source`: straight color through the display, the Viewer's adjustment when
// it is not neutral, then alpha again. False when the processor could not run or on cancellation
// (`cancelled` set).
[[nodiscard]] bool displaySignal(const std::span<const render::Rgba32f> source,
                                 const ScopeSignalColor& color,
                                 const CancellationToken& cancellation,
                                 std::vector<render::Rgba32f>& signal, bool& cancelled) {
    const auto* const processor = color.displayProcessor();
    if (processor == nullptr)
        return false;
    const auto& adjust = color.viewAdjust();
    const bool adjusted = !adjust.neutral();
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
            float red = scratch[index * 4 + 0];
            float green = scratch[index * 4 + 1];
            float blue = scratch[index * 4 + 2];
            if (adjusted && finite(red, green, blue)) {
                red = static_cast<float>(adjust.fromEncoded(static_cast<double>(red)));
                green = static_cast<float>(adjust.fromEncoded(static_cast<double>(green)));
                blue = static_cast<float>(adjust.fromEncoded(static_cast<double>(blue)));
            }
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

// The CIE XYZ of every pixel of `signal` (its stored color, not divided by alpha). A pixel that
// is not finite, or whose XYZ is not, is zero: the measure counts it as having no chromaticity.
[[nodiscard]] bool xyzPicture(const std::span<const render::Rgba32f> signal,
                              const ScopeSignalColor& color, const CancellationToken& cancellation,
                              std::vector<render::Rgba32f>& xyz, bool& cancelled) {
    xyz.assign(signal.size(), render::Rgba32f::transparent());
    const std::size_t chunk = std::min(kChunkPixels, signal.size());
    std::vector<float> rgb(chunk * 3);
    std::vector<float> out(chunk * 3);
    for (std::size_t first = 0; first < signal.size(); first += kChunkPixels) {
        if (cancellation.isCancellationRequested()) {
            cancelled = true;
            return false;
        }
        const std::size_t count = std::min(kChunkPixels, signal.size() - first);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& pixel = signal[first + index];
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
            const auto& pixel = signal[first + index];
            if (!finite(pixel.red(), pixel.green(), pixel.blue()))
                continue;
            xyz[first + index] = pixelOf(outChunk[index * 3 + 0], outChunk[index * 3 + 1],
                                         outChunk[index * 3 + 2], pixel.alpha());
        }
    }
    return true;
}

[[nodiscard]] std::string deviceMessage(const GpuScopeMeasureOutcome& outcome) {
    return outcome.diagnostic.message.empty()
               ? std::string("The device route could not measure the frame.")
               : outcome.diagnostic.message;
}

} // namespace

ScopeSignalColorResult ScopeSignalColor::prepare(const ScopeSignalColorRequest& request) {
    ScopeSignalColorResult result;
    if (request.config == nullptr || !request.viewAdjust.valid()) {
        result.error = ScopeSignalColorError::InvalidRequest;
        result.message = "The scope signal names no color config or an adjustment out of range.";
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
        color->viewAdjust_ = request.viewAdjust;
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
                       color->display_ + '|' + color->view_ + '|' +
                       std::to_string(std::bit_cast<std::uint64_t>(color->viewAdjust_.exposure)) +
                       '|' + std::to_string(std::bit_cast<std::uint64_t>(color->viewAdjust_.gamma));
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

ScopeFrameMeasure
measureScopeFrameOnDevice(GpuProcessFrameEvaluator& evaluator, const GpuSceneOcioContext& ocio,
                          std::shared_ptr<const CompiledCompositionPlan> plan,
                          const EvaluationRequest& request, const ScopeMeasureSettings& settings,
                          const ScopeSignalColor& color, const CancellationToken& cancellation,
                          const std::optional<std::filesystem::path>& assetBaseDirectory) {
    if (settings.signal != color.signal())
        return failed("The scope signal and its color objects do not match.");
    GpuScopeMeasureJob job;
    job.scopes = settings.scopes;
    job.axisMaximum = settings.axisMaximum;
    job.luma = color.luma();
    job.ignoreTransparent = settings.ignoreTransparent;
    job.region = settings.region;
    const auto factory = [&ocio, &color](GpuOcioTransformSpec spec) -> GpuScopeCommandFactory {
        return
            [&ocio, &color, spec = std::move(spec)](
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
        spec.viewAdjust = color.viewAdjust();
        job.displayCommand = factory(std::move(spec));
    }
    if ((settings.scopes & render::kScopeTableChromaticity) != 0U) {
        GpuOcioTransformSpec spec;
        spec.kind = GpuOcioTransformKind::ScopeXyz;
        spec.fromId = color.colorSpaceId();
        job.xyzCommand = factory(std::move(spec));
    }
    auto outcome =
        evaluator.measureScopes(std::move(plan), request, job, cancellation, assetBaseDirectory);
    ScopeFrameMeasure result;
    result.route = ScopeRoute::Gpu;
    result.area = outcome.area;
    switch (outcome.status) {
    case GpuScopeMeasureStatus::Measured:
        result.status = ScopeFrameStatus::Measured;
        result.counts = std::move(outcome.counts);
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
