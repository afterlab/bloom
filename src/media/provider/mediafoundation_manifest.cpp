#include <bloom/media/provider/mediafoundation_manifest.hpp>

#include <cstdint>

namespace bloom::media::provider {

Handshake mediaFoundationHandshake() {
    Handshake handshake;
    auto& execution = handshake.execution;
    execution.provider = "bloom.mediafoundation";
    execution.build = BLOOM_MEDIAFOUNDATION_BUILD_ID;
    if (const auto lock = Digest::fromLowercaseHex(BLOOM_MEDIA_DEPENDENCY_LOCK))
        execution.dependencyLock = *lock;
    execution.os = BLOOM_MEDIA_OS;
    execution.architecture = BLOOM_MEDIA_ARCH;
    // The container is read and written by the worker's libavformat; the pictures are coded by
    // the system's transforms.
    execution.sdk = "Media Foundation; FFmpeg-8.1.2";
    execution.driver = "media-foundation";
    execution.device = "windows-media-foundation";
    // The transforms run on the processor or on the graphics hardware as Windows and its drivers
    // decide; the provider is declared as the platform's hardware-capable implementation, as the
    // macOS one is.
    execution.implementation = Implementation::Hardware;
    execution.synchronization = "pipe-order";
    execution.resourceProfile = "media-v1";
    execution.entitlement = "apple_authorized=false;delivery_qualified=false";
    execution.trustDomain = "windows-media-foundation";

    QualificationEvidenceV1 evidence;
    if (const auto fixtures = Digest::fromLowercaseHex(BLOOM_MEDIAFOUNDATION_FIXTURE_ID))
        evidence.fixtures = *fixtures;
    evidence.reviewDate = "2026-11-12";
    evidence.qualification = Qualification::PreviewQualified;
    evidence.result = QcResult::Pass;

    auto add = [&](Role role, const char* codec) -> ProviderDeclaration& {
        MediaCapabilityKeyV1 capability;
        capability.role = role;
        capability.container = "intake-demuxers-v1";
        capability.mapping = role == Role::Probe ? "bounded-discovery-v1" : "stream-selection-v1";
        capability.codec = codec;
        capability.profile = "stream-declared";
        capability.level = capability.tier = capability.sampleEntry = "stream-declared";
        capability.bitDepth = 16;
        capability.range = capability.chroma = capability.chromaLocation = "stream-declared";
        capability.alpha = "preserve";
        capability.fieldMode = "progressive";
        capability.maxWidth = capability.maxHeight = Limits::dimension;
        capability.maxRate = {1000, 1};
        capability.audioFormat = "planar-float";
        capability.channelLayout = "stream-declared";
        capability.colorFeatures = "h273-original";
        capability.hdrFeatures = "metadata-only";
        capability.timecodeFeatures = "original-tag";
        capability.metadataFeatures = "duration-rate-timebase";
        capability.timing = "presentation-order-exact";
        capability.surfaceSemantics = "cpu-planes-v2";
        handshake.declarations.push_back({std::move(capability), execution, evidence});
        return handshake.declarations.back();
    };

    add(Role::Probe, "intake-discovery");
    add(Role::DemuxIndex, "intake-discovery");
    // The system decoders' own codecs. Every other picture codec of a source is decoded by the
    // software generation, stream by stream.
    for (const auto* codec : {"h264", "hevc"})
        add(Role::VideoDecode, codec);
    // Uncompressed audio is the stored samples. Coded audio is not declared: the system's AAC
    // decoder returns 16-bit samples, which are not the software decoder's, so the software
    // generation decodes the coded audio of a source whose pictures this one reads.
    for (const auto* codec :
         {"pcm_s16le", "pcm_s16be", "pcm_s24le", "pcm_s24be", "pcm_s32le", "pcm_s32be", "pcm_f32le",
          "pcm_f32be", "pcm_f64le", "pcm_f64be", "pcm_u8", "pcm_s8"})
        add(Role::AudioDecode, codec);

    // Encode: H.264 and HEVC pictures through the system's encoders; the audio, the container
    // and the reopen check are the software generation's.
    auto encode = [&](Role role, const char* codec, const char* container,
                      const char* profile) -> ProviderDeclaration& {
        auto& declaration = add(role, codec);
        declaration.capability.purpose = Purpose::Export;
        declaration.capability.container = container;
        declaration.capability.profile = profile;
        declaration.capability.mapping = "bounded-encode-v1";
        declaration.capability.surfaceSemantics = "rgba16-srgb-pcm-v1";
        declaration.evidence.qualification = Qualification::Development;
        if (const auto fixtures = Digest::fromLowercaseHex(BLOOM_MEDIAFOUNDATION_ENCODE_FIXTURE_ID))
            declaration.evidence.fixtures = *fixtures;
        return declaration;
    };
    struct SystemEncode final {
        const char* codec;
        const char* profile;
        std::uint32_t depth;
    };
    for (const auto [codec, profile, depth] :
         {SystemEncode{"h264", "high", 8}, SystemEncode{"hevc", "main", 8},
          SystemEncode{"hevc", "main10", 10}}) {
        auto& declaration = encode(Role::VideoEncode, codec, "mov", profile);
        declaration.capability.bitDepth = depth;
        declaration.capability.chroma = depth == 8U ? "yuv420p" : "yuv420p10";
        declaration.capability.range = "stream-declared";
        declaration.capability.colorFeatures = "h273-export-tags";
    }
    for (const auto* codec : {"pcm_s16le", "pcm_s24le", "aac"})
        encode(Role::AudioEncode, codec, "intake-muxers-v1", "source-rate");
    for (const auto* container : {"mov", "mp4"}) {
        encode(Role::Mux, "intake-encoders-v1", container, "closed-stream-layout-v1");
        encode(Role::ReopenDecode, "intake-encoders-v1", container, "first-last-pcm-v1");
    }

    handshake.transports = {Transport::PipeCopiesV0};
    return handshake;
}

PipelineQualificationV1 mediaFoundationPipeline(const ProviderDeclaration& declaration) {
    PipelineQualificationV1 pipeline;
    pipeline.steps.push_back({std::get<Digest>(digest(declaration.capability)),
                              std::get<Digest>(digest(declaration.execution)),
                              std::get<Digest>(digest(declaration.evidence))});
    pipeline.profile = "media3-preview-read-v1";
    pipeline.fixtures = declaration.evidence.fixtures;
    pipeline.conversionVersions = {"cpu-planes-v2"};
    pipeline.determinism = MediaDeterminismV1::NoDeterminismClaim;
    pipeline.reopenPolicy = "read-only";
    pipeline.qcProfile = "media3-generated-read-v1";
    pipeline.result = QcResult::Pass;
    if (declaration.capability.purpose == Purpose::Export) {
        pipeline.purpose = Purpose::Export;
        pipeline.profile = "media4-export-component-v1";
        EncodeSettingsV1 settings;
        settings.videoCodec =
            declaration.capability.role == Role::AudioEncode ? "" : declaration.capability.codec;
        settings.audioCodec =
            declaration.capability.role == Role::AudioEncode ? declaration.capability.codec : "";
        // The system's encoders are tolerance-class, never exact.
        pipeline.determinism = encodeDeterminism(settings);
        pipeline.toleranceProfile = encodeTolerance(settings);
        pipeline.reopenPolicy = "same-provider-not-independent";
        pipeline.qcProfile = "first-last-layout-time-pcm-v1";
    }
    return pipeline;
}

} // namespace bloom::media::provider
