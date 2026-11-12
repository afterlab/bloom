#pragma once

#include <bloom/media/provider/protocol.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace bloom::media::videotoolbox {

// Encode and mux through AVAssetWriter and VideoToolbox. Reading is videotoolbox_reader.hpp.
class Encoder final {
  public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    [[nodiscard]] provider::Payload call(provider::MessageKind, const provider::Payload&);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] bool hardwareEncodeAvailable();

// What the PCM track of a reopened movie holds.
struct ReopenedPcm final {
    std::uint64_t samples = 0;
    // SHA-256 of the stored samples in file order, little-endian: two bytes each for 16 bits,
    // and for 24 bits four bytes with the low one zero. This is the FFmpeg worker's digest of
    // the same samples.
    provider::Digest digest;
};

// The audio half of the encoder's reopen check for a PCM track, under the tolerance profile's
// `pcm=exact` (issue #666): the track `stream` of the movie at `path` is decoded from the file
// by the macOS reader, and it has to hold exactly `settings.audioSamples` sample frames whose
// stored samples hash to `expected`. Anything else is refused with the reason.
[[nodiscard]] provider::Result<ReopenedPcm> reopenedPcm(const std::string& path,
                                                        std::uint32_t stream,
                                                        const provider::EncodeSettingsV1& settings,
                                                        const provider::Digest& expected);

// The integer the output stores for one float sample: the FFmpeg worker's conversion (round to
// the nearest even at 2^15, or at 2^31 with the low eight bits dropped for 24 bits, then clip).
// 24-bit values are returned in 32 bits with the low byte zero.
[[nodiscard]] std::int32_t pcmSample(float value, bool twentyFourBits) noexcept;

} // namespace bloom::media::videotoolbox
