#pragma once

// The host's side of one supervised config helper (docs/architecture/color-management.md,
// "Supervised OCIO Execution"). A session owns one bloom-color-worker process that parsed one
// external config; the config, its processors and every OCIO object stay in that process. The host
// keeps this handle, which is the RemoteOcioConfig behind a ResolvedBloomNeutralConfig, and opaque
// processor tokens that are valid only for this session.
//
// One helper serves one request at a time: every exchange is serialized. A deadline, a protocol
// failure or a helper that ended fails the session for good, terminates the helper and
// invalidates all of its tokens.

#include "ocio_config_protocol.hpp"
#include "ocio_internal.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace bloom::color::detail {

class ConfigSession final : public RemoteOcioConfig,
                            public std::enable_shared_from_this<ConfigSession> {
  public:
    struct Opened final {
        // Set when the helper accepted the request group and answered; the reply says whether the
        // config is Ready.
        std::shared_ptr<ConfigSession> session;
        std::optional<ConfigOpenReply> reply;
        OcioHelperError helperError = OcioHelperError::None;
    };

    // Launches a helper, hands it the parameters and the sealed resource (`resource` is a sealed
    // shared file of exactly `resourceBytes`: an archive, or a loose config's resource table) and
    // returns its description of the config. Blocking: 5 s for start and handshake, 30 s for the
    // parse.
    [[nodiscard]] static Opened open(const ConfigParameters& parameters, int resource,
                                     std::uint64_t resourceBytes,
                                     const std::function<bool()>& cancellation = {});

    ~ConfigSession() override;
    ConfigSession(const ConfigSession&) = delete;
    ConfigSession& operator=(const ConfigSession&) = delete;

    [[nodiscard]] RemoteProcessorBuild buildProcessor(std::uint32_t kind, std::string_view a,
                                                      std::string_view b) const noexcept override;
    [[nodiscard]] std::vector<std::byte>
    extractGpuProgram(std::uint32_t kind, std::string_view a, std::string_view b,
                      render::OcioGpuProgramStage stage, std::string_view semanticsId,
                      const render::OcioGpuProgramLimits& limits,
                      render::OcioGpuProgramError& error) const noexcept override;
    [[nodiscard]] const ConfigDescription& description() const noexcept override {
        return description_;
    }
    [[nodiscard]] std::span<const ConfigContextVariable> context() const noexcept override {
        return parameters_.context;
    }

    // Applies one processor of this session to whole pixels of four floats, in slabs of at most
    // 16 MiB with a 10 s deadline each.
    [[nodiscard]] bool apply(std::uint64_t token, std::span<float> rgba,
                             bool packed) const noexcept;
    // None while the helper serves; otherwise why it stopped.
    [[nodiscard]] OcioHelperError failure() const noexcept;
    // The helper's process id, as the Profiler lists it.
    [[nodiscard]] std::int64_t processId() const noexcept;

  private:
    struct Impl;
    explicit ConfigSession(std::unique_ptr<Impl> impl, ConfigParameters parameters);
    std::unique_ptr<Impl> impl_;
    ConfigParameters parameters_;
    ConfigDescription description_;
};

} // namespace bloom::color::detail
