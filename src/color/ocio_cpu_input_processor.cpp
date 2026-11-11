#include "ocio_config_protocol.hpp"
#include "ocio_internal.hpp"
#include <bloom/color/ocio_cpu_input_processor.hpp>
#include <cmath>
#include <utility>

namespace bloom::color {
class CpuInputProcessor::Impl final {
  public:
    explicit Impl(OCIO::ConstCPUProcessorRcPtr value) : processor(std::move(value)) {}
    explicit Impl(std::shared_ptr<const detail::RemoteRgbProcessor> value)
        : remote(std::move(value)) {}
    OCIO::ConstCPUProcessorRcPtr processor;
    // Set instead of processor for a config that lives in the supervised helper.
    std::shared_ptr<const detail::RemoteRgbProcessor> remote;
};
CpuInputProcessor::CpuInputProcessor(std::unique_ptr<Impl> impl, core::Sha256Digest revision)
    : impl_(std::move(impl)), revision_(revision) {}
CpuInputProcessor::~CpuInputProcessor() = default;
std::shared_ptr<const CpuInputProcessor>
CpuInputProcessor::prepare(const ResolvedBloomNeutralConfig& config) {
    try {
        if (const auto& remote = config.impl().remote()) {
            const auto built = remote->buildProcessor(
                static_cast<std::uint32_t>(detail::ConfigProcessorKind::InputInverseDisplay), {},
                {});
            if (built.error != 0 || !built.processor)
                return {};
            return std::shared_ptr<const CpuInputProcessor>(new CpuInputProcessor(
                std::make_unique<Impl>(built.processor), config.expectedRevision()));
        }
        const OCIO::ConstContextRcPtr context = OCIO::Context::Create();
        const auto processor = config.impl().config()->getProcessor(
            context, std::string(config.processColorSpaceId()).c_str(),
            std::string(config.displayName()).c_str(), std::string(config.viewName()).c_str(),
            OCIO::TRANSFORM_DIR_INVERSE);
        return std::shared_ptr<const CpuInputProcessor>(
            new CpuInputProcessor(std::make_unique<Impl>(processor->getDefaultCPUProcessor()),
                                  config.expectedRevision()));
    } catch (const std::exception&) {
        return {};
    }
}
bool CpuInputProcessor::apply(std::span<std::array<float, 4>> pixels) const noexcept {
    try {
        if (impl_->remote) {
            if (!pixels.empty() &&
                !impl_->remote->apply({pixels.front().data(), pixels.size() * 4}, false))
                return false;
            for (const auto& pixel : pixels) {
                for (const auto channel : pixel) {
                    if (!std::isfinite(channel))
                        return false;
                }
            }
            return true;
        }
        for (auto& pixel : pixels) {
            impl_->processor->applyRGB(pixel.data());
            for (const auto channel : pixel) {
                if (!std::isfinite(channel))
                    return false;
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace bloom::color
