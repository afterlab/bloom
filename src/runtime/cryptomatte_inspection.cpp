#include <bloom/runtime/cryptomatte_inspection.hpp>

#include "cpu_composition_evaluator_support.hpp"
#include "cryptomatte.hpp"

#include <bloom/document/channel_nodes.hpp>
#include <bloom/runtime/channel_inspection.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <variant>

namespace bloom::runtime {
namespace {

struct CompiledNode final {
    std::shared_ptr<const CompiledCompositionPlan> plan;
    const CompiledCryptomatte* cryptomatte = nullptr;
    OperationIndex index = OperationIndex::fromRaw(0);
    std::string diagnostic;
};

// The plan that lowers the node's CompiledCryptomatte: its matte preview by default, or the plan
// the Viewer shows (`viewed`, possibly the composition itself) for a pick.
[[nodiscard]] CompiledNode
compileNode(const SnapshotCompiler& compiler, const document::Snapshot& snapshot,
            const document::CompositionId composition, const document::NodeId node,
            const CancellationToken& cancellation,
            std::optional<std::optional<ChannelInspection>> viewed = std::nullopt) {
    CompiledNode result;
    const auto compiled = compiler.compile(
        {.snapshot = snapshot,
         .compositionId = composition,
         .parameterOverrides = {},
         .channelInspection =
             viewed ? *viewed
                    : std::optional{ChannelInspection{
                          node, std::string(document::kCryptomatteMaskOutputPort), {}}}},
        cancellation);
    if (!compiled.plan) {
        result.diagnostic = compiled.diagnostics.empty() ? "The Cryptomatte could not be compiled"
                                                         : compiled.diagnostics.front().summary;
        return result;
    }
    result.plan = compiled.plan;
    for (std::size_t index = 0; index < result.plan->operations().size(); ++index)
        if (const auto* candidate =
                std::get_if<CompiledCryptomatte>(&result.plan->operations()[index]);
            candidate != nullptr && candidate->sourceNodeId == node) {
            result.cryptomatte = candidate;
            result.index = OperationIndex::fromRaw(index);
        }
    if (result.cryptomatte == nullptr)
        result.diagnostic = "The node is not an active Cryptomatte";
    return result;
}

} // namespace

CryptomatteLayerListing
listCryptomatteLayers(const SnapshotCompiler& compiler, const document::Snapshot& snapshot,
                      const document::CompositionId composition, const document::NodeId node,
                      const std::filesystem::path& assetBase, const core::RationalTime time,
                      const EvaluationColorIntent& colorIntent,
                      const CancellationToken& cancellation) {
    CryptomatteLayerListing listing;
    const auto compiled = compileNode(compiler, snapshot, composition, node, cancellation);
    if (compiled.cryptomatte == nullptr) {
        listing.diagnostic = compiled.diagnostic;
        return listing;
    }
    const auto sources = detail::selectChannelSetSources(
        compiled.cryptomatte->channels, time, compiled.plan->format().frameRate(), assetBase,
        cancellation, colorIntent, nullptr, MediaSourceRevisionPolicy::Revalidate,
        std::numeric_limits<std::size_t>::max(), false, nullptr,
        detail::ChannelSourceClock{compiled.plan.get(), nullptr});
    if (!sources.available()) {
        listing.diagnostic = sources.cancelled ? "Cancelled" : sources.warning;
        return listing;
    }
    listing.layers = detail::cryptomatteLayers(sources.resolution.channels);
    // #487: an empty (default) layer reads the first Cryptomatte layer present.
    const auto layer =
        detail::resolveCryptomatteLayer(sources.resolution.channels, compiled.cryptomatte->layer);
    // The manifest travels in the metadata of the part holding the ranks: decode one rank channel
    // to read it.
    const auto ranks = detail::cryptomatteRanks(sources.resolution.channels, layer);
    if (ranks.empty())
        return listing;
    const auto channel = std::ranges::find(sources.resolution.channels, ranks.front().id,
                                           &detail::ResolvedChannel::name);
    if (channel == sources.resolution.channels.end() || !channel->source ||
        *channel->source >= sources.sources.size() ||
        !sources.sources[*channel->source].sourceRevision)
        return listing;
    const auto& source = sources.sources[*channel->source];
    media::ImageChannelSelection request;
    request.part = source.interpretation.part;
    request.channels = {channel->sourceChannel};
    const auto decoded = media::decodeImageChannels(
        *source.sourceRevision, request, [&] { return cancellation.isCancellationRequested(); },
        {}, media::kMaxImageStorageBytes);
    if (!decoded.value || !*decoded.value)
        return listing;
    const auto manifest = detail::cryptomatteManifest((*decoded.value)->metadata(), layer);
    listing.manifest = manifest.present;
    for (const auto& [name, id] : manifest.entries)
        listing.names.push_back(name);
    std::ranges::sort(listing.names);
    listing.names.erase(std::unique(listing.names.begin(), listing.names.end()),
                        listing.names.end());
    return listing;
}

CryptomattePick pickCryptomatte(const SnapshotCompiler& compiler,
                                const document::Snapshot& snapshot,
                                const document::CompositionId composition,
                                const document::NodeId node, const std::filesystem::path& assetBase,
                                const core::RationalTime time,
                                const EvaluationColorIntent& colorIntent,
                                const std::optional<ChannelInspection>& inspection,
                                const std::int64_t x, const std::int64_t y,
                                const CancellationToken& cancellation) {
    CryptomattePick pick;
    const auto compiled =
        compileNode(compiler, snapshot, composition, node, cancellation, inspection);
    if (compiled.cryptomatte == nullptr) {
        pick.diagnostic = compiled.diagnostic == "The node is not an active Cryptomatte"
                              ? "The Cryptomatte is not shown in the Viewer"
                              : compiled.diagnostic;
        return pick;
    }
    // ADR 0024: the node evaluates on its layer's clock, exactly as the evaluator resolves it.
    EvaluationRequest request{.time = time,
                              .output = compiled.plan->output(),
                              .resolution = CompositionFormatResolution{},
                              .colorIntent = colorIntent};
    const auto clocks = detail::resolveClockTimes(compiled.plan, request, cancellation);
    if (!clocks) {
        pick.diagnostic = cancellation.isCancellationRequested()
                              ? "Cancelled"
                              : "The layer time of the Cryptomatte cannot be resolved";
        return pick;
    }
    const auto clock = compiled.plan->operationClock(compiled.index);
    const auto nodeTime = clock < clocks->size() ? (*clocks)[clock] : std::optional{time};
    if (!nodeTime)
        return pick; // the layer's content is absent at this time
    const auto selection = detail::selectCryptomatte(
        *compiled.cryptomatte, *nodeTime, compiled.plan->format().frameRate(), assetBase,
        cancellation,
        colorIntent, nullptr, MediaSourceRevisionPolicy::Revalidate,
        std::numeric_limits<std::size_t>::max(), false, nullptr,
        detail::ChannelSourceClock{compiled.plan.get(), nullptr});
    if (!selection.available) {
        pick.diagnostic = selection.cancelled ? "Cancelled" : selection.warning;
        return pick;
    }
    const auto gather =
        detail::gatherCryptomatte(selection, std::numeric_limits<std::size_t>::max(), cancellation);
    if (gather.layers.empty()) {
        pick.diagnostic = gather.cancelled ? "Cancelled" : gather.diagnostic;
        return pick;
    }
    const auto& layer = *gather.layers.front();
    const auto window = layer.dataWindow();
    const auto display = layer.displayWindow();
    const auto sampleX = display.originX() + x;
    const auto sampleY = display.originY() + y;
    if (!window.contains(sampleX, sampleY))
        return pick;
    const auto pixel =
        static_cast<std::size_t>(sampleY - window.originY()) * window.extent().width() +
        static_cast<std::size_t>(sampleX - window.originX());
    for (std::uint32_t rank = 0; rank < gather.rankCount; ++rank) {
        const auto [id, coverage] = gather.rankPair(rank, pixel);
        if (coverage > pick.coverage) {
            pick.coverage = coverage;
            pick.id = std::bit_cast<std::uint32_t>(id);
        }
    }
    if (!pick.id)
        return pick;
    if (auto name = detail::cryptomatteManifestName(gather.manifest, *pick.id))
        pick.matte = std::move(*name);
    else
        pick.matte = formatCryptomatteRawId(*pick.id);
    return pick;
}

} // namespace bloom::runtime
