#include "support.hpp"
#include <bloom/media/provider/protocol.hpp>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>
using namespace bloom::media::provider;
namespace {
void rejected(const Bytes& bytes, Error error) {
    const auto result = decodeMessage(bytes);
    test::check(std::holds_alternative<Unavailable>(result) &&
                    std::get<Unavailable>(result).reason == error,
                "typed hostile-frame rejection");
}
// Issue #474: a retained decoder's frame crosses as plane geometry plus offsets into the host's
// shared arena; the host copies it out and checks every plane against the arena.
void arenaFrames(const Handshake& h) {
    FrameProduct frame;
    frame.format = PixelFormat::Yuv420p8;
    frame.pts = {7, 24};
    frame.colour = {1, 1, 1, 1};
    for (const auto& [width, height] : {std::pair{4U, 3U}, {2U, 2U}, {2U, 2U}}) {
        CpuPlane plane{width, height, width, {}};
        for (std::uint32_t i = 0; i < width * height; ++i)
            plane.bytes.push_back(static_cast<std::byte>(i * 7U + width));
        frame.planes.push_back(std::move(plane));
    }
    std::vector<std::byte> arena(64, std::byte{0xEE});
    test::check(!placeInArena(frame, std::span(arena).first(19)).has_value(),
                "a frame larger than the arena crosses the pipe instead");
    const auto placed = placeInArena(frame, arena);
    test::check(placed.has_value() && placed->offsets == std::vector<std::uint64_t>{0, 12, 16} &&
                    std::ranges::all_of(placed->frame.planes,
                                        [](const CpuPlane& p) { return p.bytes.empty(); }),
                "planes are placed back to back and leave the message empty");
    if (!placed)
        return;
    const auto taken = takeArenaFrame(*placed, arena);
    test::check(std::holds_alternative<FrameProduct>(taken) &&
                    std::get<FrameProduct>(taken) == frame,
                "the copied-out frame equals the decoded frame");
    const auto encoded =
        std::get<Bytes>(encodeMessage({MessageKind::ArenaFrame, 42, 2, *placed}));
    const auto handshake = std::get<Bytes>(encodeMessage({MessageKind::Handshake, 42, 1, h}));
    {
        HostProtocol pipeOnly(42, h);
        (void)pipeOnly.receive(handshake, MessageKind::Handshake);
        test::check(std::get<Unavailable>(pipeOnly.receive(encoded, MessageKind::Frame)).reason ==
                        Error::UnexpectedMessage,
                    "a host without an arena refuses arena frames");
    }
    HostProtocol host(42, h);
    host.acceptArenaFrames();
    test::check(std::holds_alternative<Unavailable>(host.receive(encoded, MessageKind::Handshake)),
                "an arena frame is no handshake");
    (void)host.receive(handshake, MessageKind::Handshake);
    const auto received = host.receive(encoded, MessageKind::Frame);
    test::check(std::holds_alternative<Message>(received) &&
                    std::get<ArenaFrameV1>(std::get<Message>(received).payload) == *placed,
                "an arena host accepts the frame where it expects a Frame");
    // Planes outside the arena, or a geometry that does not match the layout, are refused.
    auto outside = *placed;
    outside.offsets[2] = 61;
    test::check(std::get<Unavailable>(takeArenaFrame(outside, arena)).reason == Error::BadLength,
                "a plane past the arena end is refused");
    outside.offsets[2] = std::numeric_limits<std::uint64_t>::max() - 1;
    test::check(std::get<Unavailable>(takeArenaFrame(outside, arena)).reason == Error::BadLength,
                "an offset that would wrap is refused");
    auto mismatched = *placed;
    mismatched.frame.planes[1].width = 3;
    mismatched.frame.planes[1].stride = 3;
    test::check(std::holds_alternative<Unavailable>(takeArenaFrame(mismatched, arena)),
                "a chroma plane that does not match the luma plane is refused");
    auto missing = *placed;
    missing.offsets.pop_back();
    test::check(std::holds_alternative<Unavailable>(takeArenaFrame(missing, arena)) &&
                    std::holds_alternative<Unavailable>(
                        encodeMessage({MessageKind::ArenaFrame, 42, 2, missing})),
                "every plane needs an offset");
    // Any mutation of the message either fails to decode or copies out a valid frame from inside
    // the arena; it never reads outside it.
    for (std::size_t size = 0; size < encoded.size(); ++size)
        test::check(
            std::holds_alternative<Unavailable>(decodeMessage(std::span(encoded).first(size))),
            "truncated arena frame refused");
    for (std::size_t i = 0; i < encoded.size(); ++i)
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto mutation = encoded;
            mutation[i] ^= static_cast<std::byte>(1U << bit);
            const auto decoded = decodeMessage(mutation);
            if (const auto* accepted = std::get_if<Message>(&decoded))
                if (const auto* hostile = std::get_if<ArenaFrameV1>(&accepted->payload)) {
                    const auto copied = takeArenaFrame(*hostile, arena);
                    if (const auto* product = std::get_if<FrameProduct>(&copied))
                        test::check(validLayout(*product), "a hostile arena frame stays bounded");
                }
        }
}
void run() {
    Handshake h{test::execution(),
                {{test::capability(), test::execution(), test::evidence()}},
                {},
                {Transport::PipeCopiesV0}};
    const auto bytes = std::get<Bytes>(encodeMessage({MessageKind::Handshake, 42, 1, h}));
    // Every handshake byte is either framing or bound identity. Mutating any bit must be
    // rejected without advancing the receiver, including otherwise well-formed string changes.
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto changed = bytes;
            changed[index] ^= static_cast<std::byte>(1U << bit);
            HostProtocol validation(42, h);
            test::check(std::holds_alternative<Unavailable>(
                            validation.receive(changed, MessageKind::Handshake)),
                        "mutated handshake rejected");
            test::check(
                std::holds_alternative<Message>(validation.receive(bytes, MessageKind::Handshake)),
                "rejected handshake leaves state unchanged");
        }
    }
    HostProtocol host(42, h);
    test::check(std::holds_alternative<Message>(host.receive(bytes, MessageKind::Handshake)),
                "host handshake");
    test::check(std::get<Unavailable>(host.receive(bytes, MessageKind::Handshake)).reason ==
                    Error::Replay,
                "replayed frame");
    for (std::size_t size = 0; size < bytes.size(); ++size)
        test::check(
            std::holds_alternative<Unavailable>(decodeMessage(std::span(bytes).first(size))),
            "every truncation rejected");
    auto bad = bytes;
    bad[0] = std::byte{255};
    bad[1] = std::byte{255};
    bad[2] = std::byte{255};
    bad[3] = std::byte{255};
    rejected(bad, Error::Oversized);
    bad = bytes;
    bad[12] = std::byte{255};
    rejected(bad, Error::BadEnum);
    bad = bytes;
    bad[8] = std::byte{2};
    rejected(bad, Error::VersionMismatch);
    bad = bytes;
    bad[29] = std::byte{255};
    bad[30] = std::byte{15};
    rejected(bad, Error::BadLength);
    bad = bytes;
    bad.push_back(std::byte{0});
    rejected(bad, Error::BadLength);
    auto different = h;
    different.execution.dependencyLock = digestBytes(bytes);
    HostProtocol mismatch(42, different);
    test::check(std::get<Unavailable>(mismatch.receive(bytes, MessageKind::Handshake)).reason ==
                    Error::IdentityMismatch,
                "lock digest mismatch");
    HostProtocol otherSession(43, h);
    test::check(std::get<Unavailable>(otherSession.receive(bytes, MessageKind::Handshake)).reason ==
                    Error::Replay,
                "cross-session replay");
    FrameProduct frame;
    CpuPlane plane;
    plane.width = 1;
    plane.height = 1;
    plane.stride = 4;
    plane.bytes = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    frame.planes = {plane};
    auto encoded = std::get<Bytes>(encodeMessage({MessageKind::Frame, 42, 2, frame}));
    test::check(std::get<FrameProduct>(
                    std::get<Message>(host.receive(encoded, MessageKind::Frame)).payload) == frame,
                "copied plane");
    auto excessivePlanes = encoded;
    excessivePlanes[62] = std::byte{5};
    rejected(excessivePlanes, Error::Oversized);
    const AudioBlock audio{{0, 1}, 48000, {"FC"}, {{0.25F, -0.5F}}};
    const DemuxIndex index{{{0, {0, 1}, {0, 1}}, {0, {1, 2}, {1, 2}}}};
    for (const auto& message :
         {Message{MessageKind::Audio, 42, 3, audio}, Message{MessageKind::Index, 42, 3, index}}) {
        const auto original = std::get<Bytes>(encodeMessage(message));
        for (std::size_t size = 0; size < original.size(); ++size)
            test::check(
                std::holds_alternative<Unavailable>(decodeMessage(std::span(original).first(size))),
                "truncated read product refused");
        for (std::size_t i = 0; i < original.size(); ++i)
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto mutation = original;
                mutation[i] ^= static_cast<std::byte>(1U << bit);
                const auto decoded = decodeMessage(mutation);
                if (const auto* accepted = std::get_if<Message>(&decoded)) {
                    if (const auto* block = std::get_if<AudioBlock>(&accepted->payload))
                        test::check(valid(*block), "hostile audio cannot bypass bounds");
                    if (const auto* entries = std::get_if<DemuxIndex>(&accepted->payload))
                        test::check(valid(*entries), "hostile index cannot bypass bounds");
                }
            }
    }
    encoded.pop_back();
    rejected(encoded, Error::Truncated);
    arenaFrames(h);
}
} // namespace
int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
