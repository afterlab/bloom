#include <bloom/color/scope_colorimetry.hpp>

#include "ocio_config_protocol.hpp"
#include "ocio_internal.hpp"
#include "ocio_scope_xyz.hpp"
#include <bloom/core/floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace bloom::color {

namespace {
inline constexpr std::size_t kMaxColorSpaceIdBytes = 256;
// Pixels per call into a helper-held processor, which takes four floats per pixel.
inline constexpr std::size_t kRemoteChunkPixels = 256;

[[nodiscard]] bool validId(const std::string_view id) noexcept {
    return !id.empty() && id.size() <= kMaxColorSpaceIdBytes &&
           id.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool finite3(const float* const values) noexcept {
    return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
}

[[nodiscard]] ScopeXyzError fromBuildError(const detail::ScopeXyzBuildError error) noexcept {
    switch (error) {
    case detail::ScopeXyzBuildError::None:
        return ScopeXyzError::None;
    case detail::ScopeXyzBuildError::MissingSource:
        return ScopeXyzError::MissingColorSpace;
    case detail::ScopeXyzBuildError::SourceIsData:
        return ScopeXyzError::ColorSpaceIsData;
    case detail::ScopeXyzBuildError::NoInterchange:
        return ScopeXyzError::NoXyzInterchange;
    case detail::ScopeXyzBuildError::BuildFailed:
        break;
    }
    return ScopeXyzError::TransformBuildFailed;
}

[[nodiscard]] std::string makeIdentity(const std::string_view colorSpaceId,
                                       const std::string_view cacheId) {
    return detail::scopeXyzDefinitionIdentity() + ";from=" + std::string(colorSpaceId) +
           ";proc=" + std::string(cacheId);
}

[[nodiscard]] bool chromaticity(const std::array<float, 3>& xyz, ScopeChromaticity& out,
                                double& luminance) noexcept {
    const double x = static_cast<double>(xyz[0]);
    const double y = static_cast<double>(xyz[1]);
    const double z = static_cast<double>(xyz[2]);
    const double sum = x + y + z;
    if (!(sum > 0.0) || !std::isfinite(sum))
        return false;
    out = {x / sum, y / sum};
    luminance = y;
    return true;
}
} // namespace

class ScopeXyzProcessor::Impl final {
  public:
    explicit Impl(OCIO::ConstCPUProcessorRcPtr processor) noexcept
        : processor_(std::move(processor)) {}
    explicit Impl(std::shared_ptr<const detail::RemoteRgbProcessor> remote) noexcept
        : remote_(std::move(remote)) {}

    [[nodiscard]] bool apply(std::span<const float> rgb, std::span<float> xyz) const {
        const std::size_t pixels = rgb.size() / 3;
        if (processor_) {
            for (std::size_t index = 0; index < pixels; ++index) {
                float pixel[3] = {rgb[index * 3], rgb[index * 3 + 1], rgb[index * 3 + 2]};
                if (!finite3(pixel))
                    return false;
                processor_->applyRGB(pixel);
                if (!finite3(pixel))
                    return false;
                xyz[index * 3] = pixel[0];
                xyz[index * 3 + 1] = pixel[1];
                xyz[index * 3 + 2] = pixel[2];
            }
            return true;
        }
        std::array<float, kRemoteChunkPixels * 4> chunk{};
        for (std::size_t first = 0; first < pixels; first += kRemoteChunkPixels) {
            const std::size_t count = std::min(kRemoteChunkPixels, pixels - first);
            for (std::size_t index = 0; index < count; ++index) {
                const float* const source = rgb.data() + (first + index) * 3;
                if (!finite3(source))
                    return false;
                chunk[index * 4] = source[0];
                chunk[index * 4 + 1] = source[1];
                chunk[index * 4 + 2] = source[2];
                chunk[index * 4 + 3] = 1.0F;
            }
            // The helper applies each pixel with OCIO's applyRGB and rejects a non-finite result.
            if (!remote_->apply({chunk.data(), count * 4}, false))
                return false;
            for (std::size_t index = 0; index < count; ++index) {
                float* const target = xyz.data() + (first + index) * 3;
                target[0] = chunk[index * 4];
                target[1] = chunk[index * 4 + 1];
                target[2] = chunk[index * 4 + 2];
            }
        }
        return true;
    }

  private:
    OCIO::ConstCPUProcessorRcPtr processor_;
    // Set instead of processor_ for a config that lives in the supervised helper.
    std::shared_ptr<const detail::RemoteRgbProcessor> remote_;
};

ScopeXyzProcessor::ScopeXyzProcessor(std::unique_ptr<Impl> impl, const core::Sha256Digest revision,
                                     std::string colorSpaceId, std::string identity) noexcept
    : impl_(std::move(impl)), revision_(revision), colorSpaceId_(std::move(colorSpaceId)),
      identity_(std::move(identity)) {}

ScopeXyzProcessor::~ScopeXyzProcessor() = default;

ScopeXyzProcessorResult ScopeXyzProcessor::prepare(const ResolvedBloomNeutralConfig& config,
                                                   const std::string_view colorSpaceId) noexcept {
    if (!validId(colorSpaceId))
        return {{}, ScopeXyzError::InvalidColorSpaceId};
    if (!core::supportsReferenceFloatingPointEnvironment<float>())
        return {{}, ScopeXyzError::UnsupportedFloatingPointEnvironment};
    const std::string id(colorSpaceId);
    std::unique_ptr<Impl> impl;
    std::string cacheId;
    try {
        if (const auto& remote = config.impl().remote()) {
            const auto built = remote->buildProcessor(
                static_cast<std::uint32_t>(detail::ConfigProcessorKind::ScopeXyz), id, {});
            switch (static_cast<detail::ConfigProcessorError>(built.error)) {
            case detail::ConfigProcessorError::None:
                break;
            case detail::ConfigProcessorError::MissingSource:
                return {{}, ScopeXyzError::MissingColorSpace};
            case detail::ConfigProcessorError::SourceIsData:
                return {{}, ScopeXyzError::ColorSpaceIsData};
            case detail::ConfigProcessorError::NoXyzInterchange:
                return {{}, ScopeXyzError::NoXyzInterchange};
            case detail::ConfigProcessorError::CpuUnavailable:
                return {{}, ScopeXyzError::CpuProcessorUnavailable};
            default:
                return {{}, ScopeXyzError::TransformBuildFailed};
            }
            if (!built.processor)
                return {{}, ScopeXyzError::TransformBuildFailed};
            impl = std::make_unique<Impl>(built.processor);
            cacheId = built.cacheId;
        } else {
            const auto built = detail::buildScopeXyzProcessor(config.impl().config(),
                                                              detail::isBloomNeutralConfig(config),
                                                              id, OCIO::Context::Create());
            if (built.error != detail::ScopeXyzBuildError::None)
                return {{}, fromBuildError(built.error)};
            const auto cpu = built.processor->getDefaultCPUProcessor();
            if (!cpu)
                return {{}, ScopeXyzError::CpuProcessorUnavailable};
            impl = std::make_unique<Impl>(cpu);
            cacheId = built.processor->getCacheID();
        }
        std::shared_ptr<ScopeXyzProcessor> processor(new ScopeXyzProcessor(
            std::move(impl), config.expectedRevision(), id, makeIdentity(id, cacheId)));

        // The unit primaries and white of the signal's own encoding, through the same processor.
        constexpr std::array<float, 12> kUnit{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                              0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F};
        std::array<float, 12> xyz{};
        if (!processor->apply(kUnit, xyz))
            return {{}, ScopeXyzError::DegenerateColorimetry};
        std::array<ScopeChromaticity, 4> points;
        std::array<double, 3> primaryY{};
        for (std::size_t index = 0; index < 4; ++index) {
            const std::array<float, 3> value{xyz[index * 3], xyz[index * 3 + 1],
                                             xyz[index * 3 + 2]};
            double luminance = 0.0;
            if (!chromaticity(value, points[index], luminance))
                return {{}, ScopeXyzError::DegenerateColorimetry};
            if (index < 3)
                primaryY[index] = luminance;
        }
        const double sumY = primaryY[0] + primaryY[1] + primaryY[2];
        if (!(sumY > 0.0) || !std::isfinite(sumY))
            return {{}, ScopeXyzError::DegenerateColorimetry};
        auto& colorimetry = processor->colorimetry_;
        colorimetry.luma = {static_cast<float>(primaryY[0] / sumY),
                            static_cast<float>(primaryY[1] / sumY),
                            static_cast<float>(primaryY[2] / sumY)};
        colorimetry.red = points[0];
        colorimetry.green = points[1];
        colorimetry.blue = points[2];
        colorimetry.white = points[3];
        colorimetry.colorSpaceName = id;
        return {std::move(processor), ScopeXyzError::None};
    } catch (const std::exception&) {
        return {{}, ScopeXyzError::TransformBuildFailed};
    }
}

bool ScopeXyzProcessor::apply(const std::span<const float> rgb,
                              const std::span<float> xyz) const noexcept {
    if (rgb.size() % 3 != 0 || xyz.size() != rgb.size())
        return false;
    if (!core::supportsReferenceFloatingPointEnvironment<float>())
        return false;
    try {
        return impl_->apply(rgb, xyz);
    } catch (const std::exception&) {
        return false;
    }
}

std::string formatChromaticityXy(const ScopeChromaticity value) {
    const auto one = [](const double v) {
        if (!std::isfinite(v))
            return std::string("nan");
        const bool negative = v < 0.0;
        const double scaled = std::floor(std::fabs(v) * 10000.0 + 0.5);
        if (scaled > 9.0e15)
            return std::string("nan");
        const auto units = static_cast<std::uint64_t>(scaled);
        std::string digits = std::to_string(units % 10000);
        digits.insert(0, 4 - digits.size(), '0');
        std::string out = std::to_string(units / 10000) + "." + digits;
        return negative && units != 0 ? "-" + out : out;
    };
    return one(value.x) + " " + one(value.y);
}

} // namespace bloom::color
