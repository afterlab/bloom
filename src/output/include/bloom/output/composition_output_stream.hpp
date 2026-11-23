#pragma once
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace bloom::output {
// #570: what the export is asked to do to the programme's loudness.
struct AudioLoudnessTargetV1 final {
    // Integrated loudness in LUFS (EBU R128 / ITU-R BS.1770).
    double targetLufs = -16.0;
    // The true-peak ceiling in dBTP. It holds in the delivered file (#771), not only in what the
    // encoder is given.
    double truePeakDbtp = -1.0;
    // #771: extra room under `truePeakDbtp`, in dB, for the limiter of a lossy codec, on top of
    // what the trial encodes find. Zero for a first attempt; an export queue that renders an output
    // again because its delivered file still went over adds the overshoot here.
    double encoderHeadroomDb = 0.0;
    // Test seam (#771): false skips the trial encodes, so the limiter holds the limit less
    // `encoderHeadroomDb` whatever the codec, as an encoder the trials do not model would leave it.
    bool trialEncodes = true;
};
// #570: what a loudness export measured and delivered. Loudness values are negative infinity for
// audio with nothing above the BS.1770 absolute gate (silence), and `measured` / `finalMeasured`
// say so.
struct AudioLoudnessReportV1 final {
    double targetLufs = -16.0;
    double truePeakLimitDbtp = -1.0;
    // The mix as rendered, before the gain and the limiter.
    bool measured = false;
    double measuredLufs = 0.0;
    double measuredTruePeakDbtp = 0.0;
    // The gain applied to reach the target, in dB (zero when nothing was measurable).
    double gainDb = 0.0;
    // The audio as delivered: by the output stream, what the encoder was given; once the export
    // has reopened its file (#771), the sound decoded back from the delivered file
    // (`finalFromDeliveredFile`), measured with the same reference library.
    bool finalMeasured = false;
    double finalLufs = 0.0;
    double finalTruePeakDbtp = 0.0;
    // The largest gain reduction the true-peak limiter applied, in dB (zero when it never acted).
    double limiterReductionDb = 0.0;
    // #771: the true-peak ceiling the limiter held before the encoder, in dBTP: the limit itself,
    // or lower for a lossy codec by the headroom its encoder needed.
    double limiterCeilingDbtp = -1.0;
    // #771: the audio-only trial encodes run to find that headroom (lossy codecs), and the true
    // peak the first (the limiter at the limit, less any requested headroom) and the last measured
    // in their decoded files.
    std::uint32_t trialEncodes = 0;
    double firstTrialTruePeakDbtp = 0.0;
    double trialTruePeakDbtp = 0.0;
    // #771: what the encoder was given, kept when final* become the delivered file's measurement.
    bool encoderInputMeasured = false;
    double encoderInputLufs = 0.0;
    double encoderInputTruePeakDbtp = 0.0;
    bool finalFromDeliveredFile = false;
};
struct CompositionOutputSourceV1 {
    document::Snapshot snapshot;
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    core::RationalTime origin;
    std::filesystem::path assetBaseDirectory;
    std::shared_ptr<const PreparedOutputDisplayV1> display = {};
    // Normalize the programme's loudness (#570); absent exports the audio as mixed.
    std::optional<AudioLoudnessTargetV1> loudness = {};
};
// Blocking output adapter. Owns the isolated encoder and offline mix clients. The caller
// serializes calls on an I/O executor and retains the admission reservation through close.
class CompositionOutputStreamV1 final {
  public:
    CompositionOutputStreamV1(CompositionOutputSourceV1, media::provider::EncodeSettingsV1,
                              media::provider::EncodeSessionOptionsV1,
                              std::shared_ptr<ExportResourceReservationV1>,
                              platform::ProcessCancellation);
    ~CompositionOutputStreamV1();
    CompositionOutputStreamV1(const CompositionOutputStreamV1&) = delete;
    CompositionOutputStreamV1& operator=(const CompositionOutputStreamV1&) = delete;
    [[nodiscard]] std::optional<media::provider::Unavailable>
    writeFrame(const render::Rgba32fImage&, std::uint64_t frame, std::uint64_t samplesThrough,
               runtime::TaskContext&);
    // Writes a frame whose RGBA16 display product the caller already prepared at the codec
    // boundary (prepareMediaRgba16FromDisplayV1 over the GPU display payload).
    [[nodiscard]] std::optional<media::provider::Unavailable>
    writePreparedFrame(media::provider::FrameProduct prepared, std::uint64_t samplesThrough,
                       runtime::TaskContext&);
    [[nodiscard]] media::provider::Result<media::provider::EncodeQcV1> finish();
    [[nodiscard]] media::provider::Result<media::provider::EncodedChunkV1>
    read(std::uint64_t offset);
    [[nodiscard]] std::optional<media::provider::Unavailable> close();
    // The provider that serves this stream once the first frame began it, and how it was
    // selected (ADR 0035; diagnostics and provenance only).
    // True after an audio source failed to read or decode for the mix (#428): the failure that
    // writeFrame returned belongs to the audio source stage, not the encoder.
    [[nodiscard]] bool audioSourceFailed() const noexcept;
    // How the audio sources read so far were routed away from the platform media framework (ADR
    // 0035): each source's name and VideoDecodeSession::routingNotes sentence. Read before the
    // stream is closed, for the export log.
    [[nodiscard]] std::vector<std::string> sourceRoutingNotes() const;
    // The failure is the required platform encoder refusing the encode (EncodeSessionV1).
    [[nodiscard]] bool platformEncoderRefused() const noexcept;
    // The preferred platform encoder began the stream and then failed it (EncodeSessionV1): the
    // caller may render the export again with PlatformEncoderV1::Skipped.
    [[nodiscard]] bool platformEncoderFailed() const noexcept;
    // #570: the measured and delivered loudness of a normalizing export, once every audio block has
    // been written; absent otherwise.
    [[nodiscard]] std::optional<AudioLoudnessReportV1> loudnessReport() const;
    [[nodiscard]] const media::provider::Handshake& handshake() const;
    [[nodiscard]] const media::provider::MediaProviderProvenanceV1& provenance() const;
    // The encode worker's measured memory so far (peak, cap, waits); see EncodeWorkerMemoryV1.
    [[nodiscard]] media::provider::EncodeWorkerMemoryV1 workerMemory() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
[[nodiscard]] media::provider::Result<media::provider::MediaQcEvidenceV1>
makeMediaQcEvidenceV1(const MediaOutputAnalysisV1&, const media::provider::EncodeQcV1&,
                      core::Sha256Digest approval,
                      const media::provider::Handshake* served = nullptr);
} // namespace bloom::output
