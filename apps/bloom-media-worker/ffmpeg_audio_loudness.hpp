#pragma once
// #771: the loudness and true peak of the sound a worker wrote, decoded back from the file and
// measured with the reference library (libebur128, ITU-R BS.1770 / EBU R128), so an export's report
// and its true-peak limit speak of the delivered file rather than of what the encoder was given.
// Worker-private: FFmpeg types never leave the media worker. The FFmpeg worker's verifying reopen
// feeds the frames it decodes anyway; the macOS worker reads its AVAssetWriter file back by path.
#include <cstdint>
#include <memory>
#include <string>

struct AVFrame;

namespace bloom::media::ffmpeg {

struct WrittenAudioLoudness {
    // Non-empty when the sound could not be read or measured; names what failed.
    std::string failure;
    bool loudnessMeasured = false;
    double integratedLufs = 0.0, truePeakDbtp = 0.0;
    std::uint64_t frames = 0;
};

// Measures decoded frames in order. Takes planar or interleaved 16-bit, 32-bit, float or double
// samples, the first `count` frames of each.
class WrittenAudioMeter final {
  public:
    WrittenAudioMeter(unsigned channels, unsigned sampleRate);
    ~WrittenAudioMeter();
    WrittenAudioMeter(const WrittenAudioMeter&) = delete;
    WrittenAudioMeter& operator=(const WrittenAudioMeter&) = delete;
    // False when the frame's layout or sample format is not one the meter reads.
    [[nodiscard]] bool push(const AVFrame& frame, std::uint64_t count);
    [[nodiscard]] WrittenAudioLoudness finish() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// Decodes the first audio stream of the file at `path` from its start (the container's priming
// already skipped by the decoder) and measures its first `frames` frames.
[[nodiscard]] WrittenAudioLoudness measureWrittenAudio(const std::string& path, unsigned channels,
                                                       unsigned sampleRate, std::uint64_t frames);

} // namespace bloom::media::ffmpeg
