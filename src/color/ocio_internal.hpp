#pragma once

// Private (non-installed) header shared only among bloom_color_ocio's own translation units. This
// is the sole place OpenColorIO's headers are included; nothing under include/bloom/color may
// name an OCIO type, per the "CPU Display Processor Boundary" contract ("OCIO classes,
// exceptions, pointers, and enums remain private"). Defines the out-of-line bodies of the opaque
// `Impl` nested classes forward-declared in the public headers, so the .cpp files that need real
// OCIO types (registry resolution, processor construction, display-frame application) share one
// consistent definition instead of three incompatible local ones.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>

#include <bloom/render/ocio_gpu_program.hpp>

#include <OpenColorIO/OpenColorIO.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// OpenColorIO's own headers deliberately do not define this alias themselves (OpenColorIO.h's
// "namespace OCIO = OCIO_NAMESPACE;" appears only inside its usage-example doc comment); every
// consumer is expected to declare it. OCIO_NAMESPACE itself is a versioned macro
// (OpenColorABI.h), so this alias is the one place that versioning is absorbed.
namespace OCIO = OCIO_NAMESPACE;

namespace bloom::color {

namespace detail {

struct ConfigDescription;
struct ConfigContextVariable;

// A processor that lives in the supervised helper. `rgba` is whole pixels of four floats; only
// the first three of each are transformed. `packed` selects CPUProcessor::apply on one packed
// description instead of applyRGB per pixel, so the helper runs the call the in-process path of
// the same processor runs. False when the helper failed or produced a non-finite value.
class RemoteRgbProcessor {
  public:
    virtual ~RemoteRgbProcessor() = default;
    [[nodiscard]] virtual bool apply(std::span<float> rgba, bool packed) const noexcept = 0;
};

struct RemoteProcessorBuild final {
    std::shared_ptr<const RemoteRgbProcessor> processor;
    // 0 is success; otherwise a ConfigProcessorError, or kRemoteHelperFailed when the helper
    // itself failed.
    std::uint32_t error = 0;
    bool identity = false;
    std::string cacheId;
};
inline constexpr std::uint32_t kRemoteHelperFailed = 0xffffffffU;

// A config that was parsed in the supervised helper and stays there. The host holds this handle
// in place of an OCIO config; every processor it hands out is a token of the same helper.
class RemoteOcioConfig {
  public:
    virtual ~RemoteOcioConfig() = default;
    // kind is a ConfigProcessorKind.
    [[nodiscard]] virtual RemoteProcessorBuild
    buildProcessor(std::uint32_t kind, std::string_view a, std::string_view b) const noexcept = 0;
    // The serialized GPU program of the same recipe, for deserializeOcioGpuProgram. Empty with an
    // error on failure.
    [[nodiscard]] virtual std::vector<std::byte>
    extractGpuProgram(std::uint32_t kind, std::string_view a, std::string_view b,
                      render::OcioGpuProgramStage stage, std::string_view semanticsId,
                      const render::OcioGpuProgramLimits& limits,
                      render::OcioGpuProgramError& error) const noexcept = 0;
    // Execution provenance the helper reported for itself.
    [[nodiscard]] virtual const ConfigDescription& description() const noexcept = 0;
    // The project's context variables this config was opened with.
    [[nodiscard]] virtual std::span<const ConfigContextVariable> context() const noexcept = 0;
};

} // namespace detail

class ResolvedBloomNeutralConfig::Impl final {
  public:
    explicit Impl(OCIO::ConstConfigRcPtr config) noexcept : config_(std::move(config)) {}
    explicit Impl(std::shared_ptr<const detail::RemoteOcioConfig> remote) noexcept
        : remote_(std::move(remote)) {}

    // Null for a config that lives in the supervised helper.
    [[nodiscard]] const OCIO::ConstConfigRcPtr& config() const& noexcept { return config_; }
    // Null for an in-process built-in.
    [[nodiscard]] const std::shared_ptr<const detail::RemoteOcioConfig>& remote() const& noexcept {
        return remote_;
    }

  private:
    OCIO::ConstConfigRcPtr config_;
    std::shared_ptr<const detail::RemoteOcioConfig> remote_;
};

class PreparedCpuDisplayProcessorHandle::Impl final {
  public:
    explicit Impl(OCIO::ConstCPUProcessorRcPtr cpuProcessor,
                  OCIO::ConstCPUProcessorRcPtr inverseProcessor) noexcept
        : cpuProcessor_(std::move(cpuProcessor)), inverseProcessor_(std::move(inverseProcessor)) {}
    explicit Impl(std::shared_ptr<const detail::RemoteRgbProcessor> forward,
                  std::shared_ptr<const detail::RemoteRgbProcessor> inverse) noexcept
        : remoteForward_(std::move(forward)), remoteInverse_(std::move(inverse)) {}

    [[nodiscard]] const OCIO::ConstCPUProcessorRcPtr& inverseProcessor() const& noexcept {
        return inverseProcessor_;
    }

    [[nodiscard]] const OCIO::ConstCPUProcessorRcPtr& cpuProcessor() const& noexcept {
        return cpuProcessor_;
    }

    // Set, in place of the two OCIO processors, for a helper-token lease.
    [[nodiscard]] const std::shared_ptr<const detail::RemoteRgbProcessor>&
    remoteForward() const& noexcept {
        return remoteForward_;
    }
    [[nodiscard]] const std::shared_ptr<const detail::RemoteRgbProcessor>&
    remoteInverse() const& noexcept {
        return remoteInverse_;
    }

  private:
    OCIO::ConstCPUProcessorRcPtr cpuProcessor_;
    OCIO::ConstCPUProcessorRcPtr inverseProcessor_;
    std::shared_ptr<const detail::RemoteRgbProcessor> remoteForward_;
    std::shared_ptr<const detail::RemoteRgbProcessor> remoteInverse_;
};

namespace detail {

// The one place outside the built-in resolver that may construct resolution products.
struct OcioConfigAccess final {
    // Proves the working space, enumerates displays and views, adds Bloom's synthesized input
    // spaces and builds the Ready product around `config`; otherwise Invalid with its reason.
    // `isNeutral` selects Bloom Neutral v1's exact interop-id mapping rules. An external config
    // may declare an "environment:" section; a built-in may not.
    [[nodiscard]] static OcioBuiltInResolutionResult
    describe(OCIO::ConstConfigRcPtr config, bool isNeutral, std::string_view locatorValue,
             const core::Sha256Digest& revision, std::string_view requestedWorkingColorSpaceId,
             bool allowDeclaredEnvironment) noexcept;

    [[nodiscard]] static OcioBuiltInResolutionResult
    outcome(OcioBuiltInRegistryOutcome outcome,
            const std::optional<core::Sha256Digest>& recomputed = {},
            OcioBuiltInInvalidReason invalidReason = OcioBuiltInInvalidReason::None,
            OcioHelperError helperError = OcioHelperError::None) noexcept;

    // The Ready product of a config that lives in the helper.
    [[nodiscard]] static OcioBuiltInResolutionResult
    remote(std::shared_ptr<const RemoteOcioConfig> remote, const core::Sha256Digest& revision,
           const ConfigDescription& description);
    // A Ready result around a copy of an already resolved product.
    [[nodiscard]] static OcioBuiltInResolutionResult
    ready(const ResolvedBloomNeutralConfig& resolved);
};

} // namespace detail

} // namespace bloom::color
