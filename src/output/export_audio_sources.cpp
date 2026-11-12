#include <bloom/media/image.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/provider_chain.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/output/export_audio_sources.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace bloom::output {
namespace {
using media::provider::Error;
using media::provider::Unavailable;

void collect(const runtime::CompiledCompositionPlan& plan, std::vector<ExportAudioSourceV1>& out,
             const std::size_t depth) {
    // The compiler refuses composition cycles; the depth bound only keeps a malformed plan finite.
    if (depth > 64)
        return;
    const auto& mix = plan.audioMix();
    for (const auto& layer : mix.layers) {
        if (layer.sourceIndex >= mix.sources.size())
            continue;
        const auto& source = mix.sources[layer.sourceIndex];
        const ExportAudioSourceV1 entry{source.assetId, source.audioStream};
        if (std::ranges::find(out, entry) == out.end())
            out.push_back(entry);
    }
    for (const auto& nested : mix.nestedLayers)
        if (nested.source.nestedPlanIndex < plan.nestedPlans().size() &&
            plan.nestedPlans()[nested.source.nestedPlanIndex])
            collect(*plan.nestedPlans()[nested.source.nestedPlanIndex], out, depth + 1);
}

std::string assetName(const document::AssetId asset) {
    return "Audio asset " + std::to_string(asset.value());
}

bool deeperThan16Bit(const std::string_view codec) {
    // Float, 24-bit and 32-bit PCM; 8-bit, 16-bit, A-law, mu-law and lossy codecs fit 16 bits.
    static constexpr std::array<std::string_view, 10> kDeep{
        "pcm_s24le", "pcm_s24be", "pcm_s32le", "pcm_s32be",   "pcm_f32le",
        "pcm_f32be", "pcm_f64le", "pcm_f64be", "pcm_s24daud", "pcm_s24le_planar"};
    return std::ranges::find(kDeep, codec) != kDeep.end();
}
} // namespace

std::vector<ExportAudioSourceV1>
exportAudioSourcesV1(const runtime::CompiledCompositionPlan& plan) {
    std::vector<ExportAudioSourceV1> out;
    collect(plan, out, 0);
    return out;
}

bool exportAudioPathDecodesV1(const std::string& codec) {
    // Kept in a variable: the declaration found points into the handshake.
    const auto handshake = media::provider::ffmpegHandshake();
    const auto* found = media::provider::findDecodeDeclarationV1(
        handshake, media::provider::Role::AudioDecode, codec);
    return found != nullptr && found->capability.codec == codec;
}

std::variant<ExportAudioSourceFactsV1, Unavailable> preflightExportAudioSourcesV1(
    const document::Snapshot& snapshot, const runtime::CompiledCompositionPlan& plan,
    const std::filesystem::path& assetBaseDirectory, const std::function<bool()>& cancel) {
    ExportAudioSourceFactsV1 facts;
    for (const auto& source : exportAudioSourcesV1(plan)) {
        if (cancel && cancel())
            return Unavailable{Error::Cancelled, "Composition output cancelled"};
        const auto* asset = snapshot.project().findAsset(source.asset);
        if (asset == nullptr)
            return Unavailable{Error::Unavailable,
                               assetName(source.asset) + " is missing from the project."};
        const auto path = media::resolveImagePath(asset->locator.path, asset->locator.relinkHint,
                                                  assetBaseDirectory);
        media::video::VideoDecodeSession session(path);
        const auto probed = session.probe(cancel);
        if (const auto* error = std::get_if<Unavailable>(&probed)) {
            if (error->reason == Error::Cancelled)
                return *error;
            return Unavailable{error->reason, assetName(source.asset) +
                                                  " can't be read by the export audio path (" +
                                                  error->detail + ")."};
        }
        const auto& probe = std::get<media::provider::ProbeResult>(probed);
        if (probe.sourceDigest != asset->contentDigest)
            return Unavailable{Error::SourceChanged,
                               assetName(source.asset) +
                                   " changed on disk since it was imported; reload it before "
                                   "exporting."};
        const auto* stream = media::video::audioStreamAt(probe, source.audioStream);
        if (stream == nullptr)
            return Unavailable{Error::Unavailable,
                               assetName(source.asset) + " has no audio stream " +
                                   std::to_string(source.audioStream + 1) + "."};
        if (!exportAudioPathDecodesV1(stream->codec))
            return Unavailable{Error::Unavailable, assetName(source.asset) + " (" + stream->codec +
                                                       ") is not accepted by the export audio "
                                                       "path."};
        facts.deeperThan16Bit = facts.deeperThan16Bit || deeperThan16Bit(stream->codec);
    }
    return facts;
}

} // namespace bloom::output
