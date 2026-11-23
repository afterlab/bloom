#include <bloom/media/image.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/provider_chain.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/output/export_audio_sources.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace bloom::output {

platform::LocalCopyOverride
exportAudioLocalCopiesV1(const document::AssetInterpretation& interpretation) noexcept {
    switch (interpretation.localCopies) {
    case document::AssetLocalCopies::Always:
        return platform::LocalCopyOverride::Always;
    case document::AssetLocalCopies::Never:
        return platform::LocalCopyOverride::Never;
    case document::AssetLocalCopies::Automatic:
        break;
    }
    return platform::LocalCopyOverride::Inherit;
}

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
        session.setSourceStore(platform::processSourceStore(),
                               exportAudioLocalCopiesV1(asset->interpretation));
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

bool exportAudioClipSoundsV1(const runtime::AudioClipDescription& clip,
                             const document::AssetRecord* const asset,
                             const core::RationalTime from, const core::RationalTime to,
                             const std::uint32_t rate) {
    if (clip.muted)
        return false;
    if (rate == 0 || from.denominator() <= 0 || to.denominator() <= 0)
        return true;
    const auto seconds = [](const core::RationalTime time) {
        return static_cast<double>(time.numerator()) / static_cast<double>(time.denominator());
    };
    // The mixer rounds each mapped time to its output frame; two frames cover that either way.
    const double margin = 2.0 / static_cast<double>(rate);
    const double start = seconds(clip.startTime) - margin;
    double end = seconds(clip.endTime) + margin;
    if (asset != nullptr && asset->kind == document::AssetKind::Audio && asset->rate != 0 &&
        asset->frames != 0)
        end = std::min(
            end, seconds(clip.startTime) +
                     static_cast<double>(asset->frames) / static_cast<double>(asset->rate) + 0.5);
    if (!(start < end))
        return false;
    // Without mappings the mixer reads the clip at the programme's own time.
    if (clip.timeMappings.empty())
        return seconds(from) - margin < end && seconds(to) + margin > start;
    // mapAudioClipTime silences every time outside the first mapping's range.
    const auto& range = clip.timeMappings.front();
    if (seconds(to) + margin <= seconds(range.inPoint) ||
        seconds(from) - margin >= seconds(range.outPoint))
        return false;
    // Otherwise each output frame is mapped as the mixer maps it (mapAudioClipTime).
    const double first = std::floor(seconds(from) * rate) - 2.0;
    const double last = std::ceil(seconds(to) * rate) + 2.0;
    constexpr double representable = 9.0e15;
    if (!(first > -representable && last < representable))
        return true;
    for (auto frame = static_cast<std::int64_t>(first); frame < static_cast<std::int64_t>(last);
         ++frame) {
        const auto at = core::RationalTime::create(frame, static_cast<std::int64_t>(rate));
        if (!at)
            return true;
        const auto mapped = runtime::mapAudioClipTime(clip, *at);
        if (mapped && seconds(*mapped) >= start && seconds(*mapped) < end)
            return true;
    }
    return false;
}

std::string describeExportAudioAdmissionRefusalV1(const ExportAudioAdmissionRefusalV1& refusal) {
    std::string text = refusal.refused + ": audio asset " + std::to_string(refusal.asset.value()) +
                       " needs " + std::to_string(refusal.bytes) + " bytes of memory for " +
                       refusal.purpose + ", but ";
    switch (refusal.status) {
    case ExportResourceAdmissionStatusV1::JobAllowanceExceeded:
        text += "that is more than is left of the export's memory allowance";
        if (refusal.allowance)
            text += " of " + std::to_string(*refusal.allowance) + " bytes";
        break;
    case ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded:
        // The processing root refused it when it has less free than the claim; otherwise the
        // allowance shared by running exports did.
        if (refusal.hostMemory && refusal.hostMemory->freeBytes < refusal.bytes)
            text += "there is not enough free memory: the processing memory has " +
                    std::to_string(refusal.hostMemory->freeBytes) + " of " +
                    std::to_string(refusal.hostMemory->usableBytes) + " bytes free";
        else if (refusal.serviceAllowance)
            text += "that is more than is left of the memory allowed to running exports (" +
                    std::to_string(*refusal.serviceAllowance) + " bytes)";
        else
            text += "there is not enough free memory or processing capacity";
        break;
    case ExportResourceAdmissionStatusV1::CapacityRefreshFailed:
        text += "the free memory could not be measured";
        break;
    case ExportResourceAdmissionStatusV1::ArithmeticOverflow:
        text += "that byte count overflows";
        break;
    case ExportResourceAdmissionStatusV1::Reserved:
        text += "it was refused";
        break;
    }
    text += " (" + std::to_string(refusal.decodedSources) + " audio source" +
            (refusal.decodedSources == 1 ? "" : "s") + " of this export were decoded at the time).";
    return text;
}

} // namespace bloom::output
