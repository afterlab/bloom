#pragma once
#include <bloom/media/provider/encode.hpp>
#include <bloom/media/provider/registry.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace bloom::media::provider {
inline constexpr std::uint16_t kProtocolVersion = 1, kSchemaVersion = 9;
inline constexpr std::uint32_t kEnvelopeBytes = 25;
enum class MessageKind : std::uint8_t {
    Handshake = 1,
    Call = 2,
    Probe = 3,
    Frame = 4,
    Cancel = 5,
    Shutdown = 6,
    Ack = 7,
    Failure = 8,
    Index = 9,
    Audio = 10,
    EncodeBegin = 11,
    EncodeFinish = 12,
    EncodeRead = 13,
    EncodedChunk = 14,
    EncodeQc = 15,
    // Issue #474: a decoded frame whose planes the retained worker wrote into the host's shared
    // frame arena; the message carries the plane geometry and arena offsets only.
    ArenaFrame = 16
};
// The planes of `frame` carry width, height and stride with no bytes; `offsets[i]` is where plane
// i's stride * height bytes start in the arena.
struct ArenaFrameV1 {
    FrameProduct frame;
    std::vector<std::uint64_t> offsets;
    friend bool operator==(const ArenaFrameV1&, const ArenaFrameV1&) = default;
};
// Worker side: copies the planes of `product` back to back into `arena`. Empty when they do not fit,
// and the frame then crosses the pipe as an ordinary Frame.
[[nodiscard]] std::optional<ArenaFrameV1> placeInArena(const FrameProduct& product,
                                                       std::span<std::byte> arena);
// Host side: copies an arena frame out into an ordinary product. Every plane must lie inside the
// arena and the result must be a valid layout; the copy is taken before the next request, so the
// worker never rewrites the planes while they are read.
[[nodiscard]] Result<FrameProduct> takeArenaFrame(const ArenaFrameV1& placed,
                                                  std::span<const std::byte> arena);
struct Handshake {
    ProviderExecutionKeyV1 execution;
    std::vector<ProviderDeclaration> declarations;
    std::vector<Digest> pipelines;
    std::vector<Transport> transports;
    friend bool operator==(const Handshake&, const Handshake&) = default;
};
struct CallRequest {
    MediaCapabilityKeyV1 capability;
    std::string source;
    std::uint32_t width = 1, height = 1;
    std::uint64_t frame = 0;
    std::uint32_t stream = 0, samples = 1024;
    Digest sourceDigest{};
    // Nonzero only for a host-owned descriptor capability. A wire value by itself cannot grant
    // this authority: the worker also requires the anonymous read-only inherited source handle.
    std::uint64_t snapshotBytes = 0;
};
using Payload = std::variant<Handshake, CallRequest, ProbeResult, FrameProduct, DemuxIndex,
                             AudioBlock, EncodeSettingsV1, EncodedChunkV1, EncodeQcV1,
                             std::uint64_t, std::monostate, Unavailable, ArenaFrameV1>;
struct Message {
    MessageKind kind = MessageKind::Ack;
    std::uint64_t session = 0, sequence = 0;
    Payload payload = std::monostate{};
};
[[nodiscard]] Result<Bytes> encodeMessage(const Message& message);
// Includes the u32 frame length. Validates the entire frame before publishing a value.
[[nodiscard]] Result<Message> decodeMessage(std::span<const std::byte> frame);
[[nodiscard]] Result<std::uint32_t> messageLength(std::span<const std::byte> prefix);
// Host-side stateful validation; rejected frames cannot advance session state.
class HostProtocol final {
  public:
    HostProtocol(std::uint64_t session, Handshake expected)
        : session_(session), expected_(std::move(expected)) {}
    [[nodiscard]] Result<Message> receive(std::span<const std::byte> frame,
                                          MessageKind expectedKind);
    [[nodiscard]] const Handshake& expectedHandshake() const { return expected_; }
    // A host that installed a shared frame arena accepts ArenaFrame where it expects a Frame.
    void acceptArenaFrames() noexcept { arenaFrames_ = true; }

  private:
    std::uint64_t session_ = 0, lastSequence_ = 0;
    Handshake expected_;
    bool handshaken_ = false;
    bool arenaFrames_ = false;
};
} // namespace bloom::media::provider
