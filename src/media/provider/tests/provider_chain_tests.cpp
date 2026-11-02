// Provider-chain selection with fake providers (ADR 0035): a missing or refusing platform
// provider falls back to the bundled FFmpeg provider, selection is per operation, provenance
// records every fallback, and an ending error (cancellation) never tries another provider.
#include "support.hpp"

#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/provider_chain.hpp>

#include <string>
#include <vector>

namespace {
using namespace bloom::media::provider;
using test::check;

ProviderDeclaration declaration(const Role role, const std::string& codec,
                                const std::string& container = "none") {
    ProviderDeclaration d;
    d.capability = test::capability(role);
    d.capability.codec = codec;
    d.capability.container = container;
    return d;
}

// A fake platform framework: decodes H.264/ProRes and encodes H.264/ProRes into MOV/MP4 only.
Handshake nativeHandshake() {
    Handshake h;
    h.execution.provider = "fake.native";
    for (const auto* codec : {"h264", "prores"}) {
        h.declarations.push_back(declaration(Role::VideoDecode, codec));
        h.declarations.push_back(declaration(Role::VideoEncode, codec, "mov"));
    }
    for (const auto* container : {"mov", "mp4"})
        h.declarations.push_back(declaration(Role::Mux, "native-encoders", container));
    return h;
}

// A fake FFmpeg worker: the provider spelling "prores_ks" and the wider container list.
Handshake ffmpegHandshake() {
    Handshake h;
    h.execution.provider = "fake.ffmpeg";
    for (const auto* codec : {"h264", "hevc", "prores", "dnxhd"})
        h.declarations.push_back(declaration(Role::VideoDecode, codec));
    for (const auto* codec : {"prores_ks", "dnxhd", "tiff"})
        h.declarations.push_back(declaration(Role::VideoEncode, codec, "mov"));
    h.declarations.push_back(declaration(Role::AudioEncode, "pcm_s24le", "intake-muxers-v1"));
    for (const auto* container : {"mov", "mxf", "wav", "tiff"})
        h.declarations.push_back(declaration(Role::Mux, "intake-encoders-v1", container));
    return h;
}

struct FakeProvider final {
    std::string id;
    bool native = false;
    bool installed = true;
    Handshake handshake;
    // The typed failure this provider answers with, or none.
    std::optional<Unavailable> failure;
    int attempts = 0;
};

MediaProviderChainV1 chainOf(std::vector<FakeProvider>& providers) {
    std::vector<MediaProviderCandidateV1> candidates;
    for (auto& provider : providers)
        candidates.push_back({provider.id, provider.native,
                              [&provider] { return provider.installed; },
                              [&provider](const MediaOperationV1& operation) {
                                  return declaresMediaOperationV1(provider.handshake, operation);
                              }});
    return MediaProviderChainV1(std::move(candidates));
}

Result<std::string> runOn(std::vector<FakeProvider>& providers, const MediaProviderChainV1& chain,
                          const MediaOperationV1& operation,
                          MediaProviderProvenanceV1& provenance) {
    return chain.run<std::string>(
        operation,
        [&](const MediaProviderCandidateV1& candidate) -> Result<std::string> {
            for (auto& provider : providers)
                if (provider.id == candidate.id) {
                    ++provider.attempts;
                    if (provider.failure)
                        return *provider.failure;
                    return "served by " + provider.id;
                }
            return Unavailable{Error::UnexpectedMessage, "unknown fake provider"};
        },
        provenance);
}

std::vector<FakeProvider> platformProviders() {
    return {{"videotoolbox", true, true, nativeHandshake(), std::nullopt, 0},
            {"ffmpeg", false, true, ffmpegHandshake(), std::nullopt, 0}};
}

void nativeMissingFallsBackToFfmpeg() {
    auto providers = platformProviders();
    providers[0].installed = false;
    const auto chain = chainOf(providers);
    MediaProviderProvenanceV1 provenance;
    const auto result = runOn(providers, chain, {MediaDirectionV1::Decode, "h264", {}}, provenance);
    check(std::holds_alternative<std::string>(result) &&
              std::get<std::string>(result) == "served by ffmpeg",
          "a missing platform provider falls back to FFmpeg");
    check(providers[0].attempts == 0 && providers[1].attempts == 1,
          "a missing provider is never attempted");
    check(provenance.provider == "ffmpeg" && provenance.fellBack() &&
              provenance.fallbacks.size() == 1 &&
              provenance.fallbacks[0].provider == "videotoolbox" &&
              provenance.fallbacks[0].cause == MediaFallbackCauseV1::NotInstalled,
          "provenance records the missing platform provider");
}

void nativeFailureFallsBackToFfmpeg() {
    auto providers = platformProviders();
    providers[0].failure = Unavailable{Error::Crashed, "encoder session refused the settings"};
    const auto chain = chainOf(providers);
    MediaProviderProvenanceV1 provenance;
    const auto result =
        runOn(providers, chain, {MediaDirectionV1::Encode, "prores", "mov"}, provenance);
    check(std::holds_alternative<std::string>(result) &&
              std::get<std::string>(result) == "served by ffmpeg",
          "a failing platform provider falls back to FFmpeg");
    check(providers[0].attempts == 1 && providers[1].attempts == 1,
          "the platform provider is tried first, then FFmpeg once");
    check(provenance.provider == "ffmpeg" && provenance.fallbacks.size() == 1 &&
              provenance.fallbacks[0] ==
                  MediaProviderFallbackV1{"videotoolbox", MediaFallbackCauseV1::Failed,
                                          Error::Crashed, "encoder session refused the settings"},
          "provenance records the failed platform provider with its typed error");
    const auto diagnostic = provenance.diagnostic();
    check(diagnostic.starts_with("ffmpeg (after videotoolbox: failed") &&
              diagnostic.find("encoder session refused the settings") != std::string::npos,
          "the diagnostic names the serving provider and the fallback reason");
}

void selectionIsPerOperation() {
    auto providers = platformProviders();
    const auto chain = chainOf(providers);
    MediaProviderProvenanceV1 provenance;
    auto result = runOn(providers, chain, {MediaDirectionV1::Encode, "h264", "mp4"}, provenance);
    check(std::get<std::string>(result) == "served by videotoolbox" && !provenance.fellBack(),
          "an operation the platform provider declares stays native");
    result = runOn(providers, chain, {MediaDirectionV1::Encode, "dnxhd", "mxf"}, provenance);
    check(std::get<std::string>(result) == "served by ffmpeg" && provenance.fallbacks.size() == 1 &&
              provenance.fallbacks[0].cause == MediaFallbackCauseV1::NotDeclared,
          "an undeclared codec/container selects FFmpeg for that operation only");
    result = runOn(providers, chain, {MediaDirectionV1::Decode, "hevc", {}}, provenance);
    check(std::get<std::string>(result) == "served by ffmpeg",
          "decode selection is independent of encode selection");
    result = runOn(providers, chain, {MediaDirectionV1::Decode, "prores", {}}, provenance);
    check(std::get<std::string>(result) == "served by videotoolbox",
          "the platform provider serves the decodes it declares");
    check(providers[0].attempts == 2 && providers[1].attempts == 2,
          "each operation attempts only its selected provider");
}

void endingErrorsDoNotFallBack() {
    auto providers = platformProviders();
    providers[0].failure = Unavailable{Error::Cancelled, "Encode cancelled"};
    const auto chain = chainOf(providers);
    MediaProviderProvenanceV1 provenance;
    const auto result =
        runOn(providers, chain, {MediaDirectionV1::Encode, "prores", "mov"}, provenance);
    check(std::holds_alternative<Unavailable>(result) &&
              std::get<Unavailable>(result).reason == Error::Cancelled &&
              providers[1].attempts == 0,
          "cancellation ends the operation instead of trying FFmpeg");
    check(!mediaFallbackPermittedV1({Error::Shutdown, {}}) &&
              !mediaFallbackPermittedV1({Error::SourceChanged, {}}) &&
              mediaFallbackPermittedV1({Error::Unavailable, {}}) &&
              mediaFallbackPermittedV1({Error::Timeout, {}}),
          "only cancellation, shutdown and a changed source end the chain");
}

void exhaustedChainReportsTheLastError() {
    auto providers = platformProviders();
    providers[0].failure = Unavailable{Error::Crashed, "native crashed"};
    providers[1].failure = Unavailable{Error::Io, "ffmpeg could not write"};
    const auto chain = chainOf(providers);
    MediaProviderProvenanceV1 provenance;
    const auto result =
        runOn(providers, chain, {MediaDirectionV1::Encode, "prores", "mov"}, provenance);
    check(std::holds_alternative<Unavailable>(result) &&
              std::get<Unavailable>(result).reason == Error::Io && provenance.provider.empty() &&
              provenance.fallbacks.size() == 2,
          "an exhausted chain returns the last typed error with full provenance");
    MediaProviderChainV1 empty;
    const auto none = empty.run<std::string>(
        {MediaDirectionV1::Decode, "h264", {}},
        [](const MediaProviderCandidateV1&) -> Result<std::string> { return std::string{}; },
        provenance);
    check(std::holds_alternative<Unavailable>(none), "an empty chain serves nothing");
}

void codecFamilies() {
    check(mediaCodecFamilyV1("prores_ks") == "prores" && mediaCodecFamilyV1("prores") == "prores",
          "ProRes spellings share a family");
    check(mediaCodecFamilyV1("dnxhr_hq") == "dnxhd" && mediaCodecFamilyV1("pcm_s24le") == "pcm" &&
              mediaCodecFamilyV1("h264_vaapi") == "h264" && mediaCodecFamilyV1("aac") == "aac",
          "provider codec spellings normalise to their family");
}

// #428: a decoder pins its declaration's codec exactly, so a stream is served by the declaration
// of its own codec; a sibling of the family answers only when the exact codec is not declared.
void decodeDeclarationsPinTheExactCodec() {
    Handshake h;
    for (const auto* codec : {"pcm_s16le", "pcm_s24le", "aac"})
        h.declarations.push_back(declaration(Role::AudioDecode, codec));
    const auto* exact = findDecodeDeclarationV1(h, Role::AudioDecode, "pcm_s24le");
    check(exact != nullptr && exact->capability.codec == "pcm_s24le",
          "a 24-bit stream pins the 24-bit declaration, not the first PCM one");
    const auto* sibling = findDecodeDeclarationV1(h, Role::AudioDecode, "pcm_f32le");
    check(sibling != nullptr && sibling->capability.codec == "pcm_s16le",
          "an undeclared PCM spelling falls back to its family");
    check(findDecodeDeclarationV1(h, Role::AudioDecode, "opus") == nullptr &&
              findDecodeDeclarationV1(h, Role::VideoDecode, "pcm_s24le") == nullptr,
          "an undeclared codec or role finds nothing");
    // The bundled FFmpeg worker declares every codec its WAV demuxer reports for the PCM, float,
    // A-law and mu-law encodings the WAV importer accepts; IMA and Microsoft ADPCM have no
    // decoder in this build.
    const auto ffmpeg = bloom::media::provider::ffmpegHandshake();
    for (const auto* codec : {"pcm_u8", "pcm_s16le", "pcm_s24le", "pcm_s32le", "pcm_f32le",
                              "pcm_f64le", "pcm_alaw", "pcm_mulaw", "mp3", "aac", "opus"}) {
        const auto* found = findDecodeDeclarationV1(ffmpeg, Role::AudioDecode, codec);
        check(found != nullptr && found->capability.codec == codec,
              (std::string("the FFmpeg worker pins ") + codec + " exactly").c_str());
    }
    for (const auto* codec : {"adpcm_ms", "adpcm_ima_wav"})
        check(findDecodeDeclarationV1(ffmpeg, Role::AudioDecode, codec) == nullptr,
              (std::string("the FFmpeg worker does not claim ") + codec).c_str());
}

} // namespace

int main() {
    try {
        nativeMissingFallsBackToFfmpeg();
        nativeFailureFallsBackToFfmpeg();
        selectionIsPerOperation();
        endingErrorsDoNotFallBack();
        exhaustedChainReportsTheLastError();
        codecFamilies();
        decodeDeclarationsPinTheExactCodec();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "provider chain: ok\n";
    return 0;
}
