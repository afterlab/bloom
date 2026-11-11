#pragma once
#include <bloom/media/provider/protocol.hpp>
#include <bloom/media/provider/provider_chain.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::media::provider {
// What the encode worker's memory did during a session (export "worker memory"). Measured from the
// operating system's report for the worker process; nothing is estimated.
struct EncodeWorkerMemoryV1 {
    // False when the platform could not report the worker's memory (the media framework route, or
    // an unsupported platform): the fields below are then zero and no cap could be enforced.
    bool measured = false;
    // The largest resident size the worker reached, in bytes.
    std::uint64_t peakResidentBytes = 0;
    // The cap the session ran under (0: none).
    std::uint64_t capBytes = 0;
    // How many times a product was held back because the worker was at the cap, and for how long.
    std::uint32_t waits = 0;
    std::chrono::milliseconds waited{0};
    // The session ended because the worker stayed at the cap with nothing left to finish.
    bool capReached = false;
};
// How the platform media framework's encoder (ADR 0035; the system encoder on macOS) takes part
// in an encode it declares. It has no effect where no such provider is installed or it does not
// declare the codec and container.
enum class PlatformEncoderV1 : std::uint8_t {
    // First, with the bundled FFmpeg worker as the silent fallback.
    Preferred = 1,
    // Only: when it refuses, begin() fails and platformEncoderRefused() is true. Nothing else is
    // substituted without the caller asking for it.
    Required = 2,
    // Not at all: the bundled FFmpeg worker encodes.
    Skipped = 3,
};
struct EncodeSessionOptionsV1 {
    // The FFmpeg worker. Empty selects the installed worker beside the application, else this
    // build tree's worker. The platform media framework's worker is always located by the host.
    std::string executable;
    bool vaapi = false;
    // Try the NVIDIA NVENC generation first (ADR 0036): NVENC, then VA-API when `vaapi`, then the
    // bundled software encoders. A computer without an NVIDIA driver falls through silently.
    bool nvenc = false;
    PlatformEncoderV1 platformEncoder = PlatformEncoderV1::Preferred;
    std::chrono::milliseconds timeout{120000};
    std::function<void(std::int64_t)> launched;
    // ADR 0033 section 16: the host's staged artifact. The worker writes the container into it
    // and verifies it in place; the host then adopts it instead of reading chunks. Linux FFmpeg
    // routes only; a platform media framework route ignores it.
    std::shared_ptr<const platform::StagedArtifactInheritableWriter> stagedOutput;
    // Opt-in cap on the worker's resident memory, in bytes; zero (the default) applies none and
    // leaves the session exactly as it is without this option. At the cap the session stops
    // sending products and waits for the worker to finish what it holds; if the worker is still at
    // or above the cap after it has finished everything and `memorySettle` has passed, the session
    // fails with a plain sentence naming the memory and the cap instead of going on.
    std::uint64_t memoryCapBytes = 0;
    // How long a worker that holds nothing more to finish may stay at the cap before the session
    // gives up (its allocator may still be returning memory to the system).
    std::chrono::milliseconds memorySettle{3000};
    // Test seam: replaces the operating system's report of the worker's memory.
    std::function<std::optional<platform::ProcessMemory>(std::int64_t)> memorySampler;
};
// begin() selects the provider through the platform's encode chain (ADR 0035): the platform media
// framework first, then the bundled FFmpeg worker (Linux: FFmpeg with VA-API when requested, then
// software). A provider that is missing, does not declare the codec/container, or refuses the
// launch, handshake or EncodeBegin is skipped silently and recorded in provenance(). Once begin()
// succeeds the provider is pinned for the whole attempt.
// BlockingIo-only, one request/ack at a time. A failure poisons this session and reaps its worker.
// Destruction cancels unfinished work. Neither worker nor client can publish a destination.
class EncodeSessionV1 final {
  public:
    explicit EncodeSessionV1(EncodeSessionOptionsV1 options,
                             platform::ProcessCancellation cancellation = {});
    ~EncodeSessionV1();
    EncodeSessionV1(const EncodeSessionV1&) = delete;
    EncodeSessionV1& operator=(const EncodeSessionV1&) = delete;
    [[nodiscard]] std::optional<Unavailable> begin(const EncodeSettingsV1& settings);
    // Frames and audio blocks are written in order without waiting for the worker's
    // acknowledgement (a bounded number stay unread); a refused product surfaces as the error of a
    // later call, and finish/read/close first collect every outstanding acknowledgement.
    [[nodiscard]] std::optional<Unavailable> video(FrameProduct frame);
    [[nodiscard]] std::optional<Unavailable> audio(AudioBlock block);
    [[nodiscard]] Result<EncodeQcV1> finish();
    [[nodiscard]] Result<EncodedChunkV1> read(std::uint64_t offset);
    [[nodiscard]] std::optional<Unavailable> close();
    [[nodiscard]] const ProviderExecutionKeyV1& execution() const;
    // The serving provider's handshake and the selection provenance (diagnostics only).
    [[nodiscard]] const Handshake& handshake() const;
    [[nodiscard]] const MediaProviderProvenanceV1& provenance() const;
    // begin() failed because the platform encoder was Required and refused the encode; the
    // bundled FFmpeg worker was not tried (PlatformEncoderV1::Skipped selects it).
    [[nodiscard]] bool platformEncoderRefused() const noexcept;
    [[nodiscard]] static std::string defaultWorker();
    // The worker's measured memory so far (peak over the samples taken at each product, at finish
    // and when the session ends); valid after the worker has gone.
    [[nodiscard]] EncodeWorkerMemoryV1 workerMemory() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace bloom::media::provider
