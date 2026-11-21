#include <bloom/runtime/subtitle_tracks.hpp>

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/cue_markup.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/runtime/composition_time.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace bloom::runtime {
namespace {

// The deepest nesting the walk follows; the compiler refuses cycles, so this only bounds a
// pathological but valid chain.
constexpr int kMaximumNesting = 64;

struct NodeSettings final {
    bool subtitles = false;
    bool exportTrack = true;
    bool speakerPrefix = false;
    std::string language;
    std::string title;
};

NodeSettings readNodeSettings(const document::Project& project, const document::NodeId id) {
    NodeSettings settings;
    for (const auto& composition : project.compositions()) {
        const auto* node = composition.graph().findNode(id);
        if (node == nullptr || node->typeId != document::kSubtitlesNodeType)
            continue;
        settings.subtitles = true;
        for (const auto& binding : node->parameters) {
            const auto* stored = composition.parameters().find(binding.parameterId);
            const auto* constant =
                stored == nullptr ? nullptr
                                  : std::get_if<document::ConstantValueSource>(&stored->source);
            if (constant == nullptr)
                continue;
            if (binding.role == "exportTrack") {
                if (const auto* value = std::get_if<bool>(&constant->value))
                    settings.exportTrack = *value;
            } else if (binding.role == "language") {
                if (const auto* value = std::get_if<std::string>(&constant->value))
                    settings.language = *value;
            } else if (binding.role == "trackName") {
                if (const auto* value = std::get_if<std::string>(&constant->value))
                    settings.title = *value;
            } else if (binding.role == "showSpeaker") {
                if (const auto* value = std::get_if<std::int64_t>(&constant->value))
                    settings.speakerPrefix = *value == document::kSubtitlesSpeakerPrefix;
            }
        }
        break;
    }
    return settings;
}

// One Subtitles operation the walk reached at one frame.
struct Reached final {
    document::NodeId node;
    std::shared_ptr<const document::CueListValue> list;
    std::vector<std::size_t> active;
    core::RationalTime time;
};

class Walker final {
  public:
    Walker(const CancellationToken& cancellation, std::vector<Reached>& reached)
        : cancellation_(cancellation), reached_(reached) {}

    // False on a failure or cancellation (failure() / cancelled()).
    bool walkPlan(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                  const core::RationalTime time, const int depth) {
        if (depth > kMaximumNesting)
            return fail("The compositions nest too deeply to follow their subtitles");
        bool cancelled = false;
        auto base = detail::sampleParametersAt(*plan, time, cancellation_, cancelled);
        if (cancelled || cancellation_.isCancellationRequested())
            return cancel();
        if (!base)
            return fail("The composition's animation could not be sampled for its subtitles");
        EvaluationRequest request{
            .time = time, .output = plan->output(), .resolution = CompositionFormatResolution{}};
        auto outcome = detail::resolveLayerClocks(plan, request, std::move(*base), cancellation_,
                                                  nullptr, nullptr);
        if (outcome.cancelled)
            return cancel();
        if (!outcome.resolved)
            return fail(outcome.diagnostic ? outcome.diagnostic->summary
                                           : std::string("A layer time could not be resolved"));
        std::vector<std::uint8_t> seen(plan->operations().size(), 0);
        return walkOperation(*plan, *outcome.resolved, time, plan->output(), seen, depth);
    }

    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

  private:
    bool fail(std::string message) {
        failure_ = std::move(message);
        return false;
    }
    bool cancel() {
        cancelled_ = true;
        return false;
    }

    bool walkOperation(const CompiledCompositionPlan& plan,
                       const detail::ResolvedEvaluation& resolved, const core::RationalTime time,
                       const OperationIndex index, std::vector<std::uint8_t>& seen,
                       const int depth) {
        const auto operations = plan.operations();
        if (index.value() >= operations.size() || seen[index.value()] != 0)
            return true;
        seen[index.value()] = 1;
        // ADR 0024: an operation on an absent clock produces nothing (the evaluator's rule).
        const auto operandClock = plan.operationClock(index);
        const auto operandTime = detail::timeForClock(resolved, time, operandClock);
        const auto placementTime = detail::timeForClock(resolved, time, plan.placementClock(index));
        if (!operandTime || !placementTime)
            return true;
        const auto& operation = operations[index.value()];
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation)) {
            // Outside its range a layer publishes no image (the evaluator's rule); its parent is a
            // transform, not a picture.
            if (*placementTime < layer->inPoint ||
                (layer->outPoint && *placementTime >= *layer->outPoint))
                return true;
            return walkOperation(plan, resolved, time, layer->input, seen, depth);
        }
        const auto& operands = detail::resolvedForClock(resolved, operandClock);
        if (const auto* source = std::get_if<CompiledCompositionSource>(&operation)) {
            if (source->nestedPlanIndex >= plan.nestedPlans().size() ||
                !plan.nestedPlans()[source->nestedPlanIndex])
                return fail("A nested composition of the export is missing");
            const auto& nested = plan.nestedPlans()[source->nestedPlanIndex];
            const auto offset =
                detail::resolveParameter(source->timeMapping.offset, plan, operands);
            const auto scale = detail::resolveParameter(source->timeMapping.scale, plan, operands);
            const auto mapped =
                offset && scale
                    ? mapCompositionTime(*operandTime, offset->value, scale->value,
                                         nested->duration(), nested->format().frameRate(),
                                         source->timeMapping.loopMode)
                    : std::nullopt;
            if (!mapped)
                return fail("A nested composition's time cannot be represented");
            return walkPlan(nested, *mapped, depth + 1);
        }
        if (const auto* subtitles = std::get_if<CompiledSubtitles>(&operation)) {
            const CompiledCueList* list = &subtitles->cues;
            if (subtitles->drivenCues) {
                list = detail::resolvedValue<CompiledCueList>(*subtitles->drivenCues, operands);
                if (list == nullptr)
                    return fail("The cues of Subtitles node " +
                                std::to_string(subtitles->sourceNodeId.value()) +
                                " could not be resolved");
            }
            reached_.push_back(
                {subtitles->sourceNodeId, list->list, list->activeAt(*operandTime), *operandTime});
            return true;
        }
        bool walked = true;
        detail::forEachInput(operation, [&](const OperationIndex input) {
            if (walked)
                walked = walkOperation(plan, resolved, time, input, seen, depth);
        });
        return walked;
    }

    const CancellationToken& cancellation_;
    std::vector<Reached>& reached_;
    std::string failure_;
    bool cancelled_ = false;
};

// A cue showing over consecutive frames.
struct Run final {
    document::NodeId node;
    std::shared_ptr<const document::CueListValue> list;
    document::CueHandle cue;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    // The cue's timestamp tags (milliseconds on its own clock, ascending, distinct), the first
    // frame that reached each, and whether the clock moved forward on every frame of the run.
    std::vector<std::int64_t> tags;
    std::vector<std::optional<std::uint64_t>> tagFrames;
    std::size_t nextTag = 0;
    core::RationalTime previous;
    bool forward = true;
};

void collectTimestamps(const std::vector<document::cue_markup::Node>& nodes,
                       std::vector<std::int64_t>& out) {
    for (const auto& node : nodes) {
        if (node.kind == document::cue_markup::NodeKind::Timestamp)
            out.push_back(node.milliseconds);
        collectTimestamps(node.children, out);
    }
}

void advanceTags(Run& run, const std::uint64_t frame, const core::RationalTime time) {
    if (frame != run.first && time < run.previous)
        run.forward = false;
    run.previous = time;
    while (run.nextTag < run.tags.size()) {
        const auto tag = core::RationalTime::create(run.tags[run.nextTag], 1000);
        if (!tag || *tag > time)
            break;
        run.tagFrames[run.nextTag++] = frame;
    }
}

// Rewrites the timestamp tags of `nodes`: a tag a frame of the run reached names that frame's
// output time; any other tag is dropped.
void moveTimestamps(std::vector<document::cue_markup::Node>& nodes, const Run& run,
                    const std::uint64_t rangeFirst, const document::FrameRate rate) {
    std::vector<document::cue_markup::Node> kept;
    kept.reserve(nodes.size());
    for (auto& node : nodes) {
        if (node.kind == document::cue_markup::NodeKind::Timestamp) {
            const auto found = std::ranges::lower_bound(run.tags, node.milliseconds);
            const auto at = static_cast<std::size_t>(found - run.tags.begin());
            if (!run.forward || found == run.tags.end() || !run.tagFrames[at])
                continue;
            // Milliseconds of the output time (frame - first) / rate, rounded down.
            const auto frames = *run.tagFrames[at] - rangeFirst;
            node.milliseconds =
                static_cast<std::int64_t>(frames * 1000U * rate.denominator() / rate.numerator());
        }
        moveTimestamps(node.children, run, rangeFirst, rate);
        kept.push_back(std::move(node));
    }
    nodes = std::move(kept);
}

std::optional<core::RationalTime> outputTime(const std::uint64_t frames,
                                             const document::FrameRate rate) {
    if (frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                     std::max<std::uint32_t>(rate.denominator(), 1U))
        return std::nullopt;
    return core::RationalTime::create(static_cast<std::int64_t>(frames * rate.denominator()),
                                      static_cast<std::int64_t>(rate.numerator()));
}

} // namespace

SubtitleTracksOutcome collectSubtitleTracks(const SubtitleTrackRequest& request,
                                            const document::Project& project,
                                            const CancellationToken& cancellation,
                                            const SubtitleTrackProgress& progress) {
    if (!request.plan || request.lastFrame < request.firstFrame)
        return SubtitleTrackFailure{"The export range has no frames", false};
    const auto& plan = request.plan;
    const auto rate = plan->format().frameRate();
    const auto mapping =
        core::FrameTimeMapping::create(plan->duration(), rate.numerator(), rate.denominator());
    if (!mapping)
        return SubtitleTrackFailure{"The composition's frame timing is invalid", false};

    std::map<document::NodeId, NodeSettings> settings;
    const auto settingsOf = [&](const document::NodeId node) -> const NodeSettings& {
        auto found = settings.find(node);
        if (found == settings.end())
            found = settings.emplace(node, readNodeSettings(project, node)).first;
        return found->second;
    };
    const auto wanted = [&](const document::NodeId node) {
        if (request.nodes)
            return std::ranges::binary_search(*request.nodes, node);
        return settingsOf(node).exportTrack;
    };

    std::set<document::NodeId> reachedNodes;
    std::vector<Run> open;
    std::vector<Run> closed;
    std::vector<Reached> reached;
    const auto total = request.lastFrame - request.firstFrame + 1;
    for (std::uint64_t frame = request.firstFrame; frame <= request.lastFrame; ++frame) {
        if (cancellation.isCancellationRequested())
            return SubtitleTrackFailure{{}, true};
        const auto time = mapping.value()->timeForFrame(frame);
        if (!time)
            return SubtitleTrackFailure{
                "Frame " + std::to_string(frame) + " lies outside the composition", false};
        reached.clear();
        Walker walker(cancellation, reached);
        if (!walker.walkPlan(plan, *time.value(), 0)) {
            if (walker.cancelled())
                return SubtitleTrackFailure{{}, true};
            return SubtitleTrackFailure{
                "At frame " + std::to_string(frame) + ": " + walker.failure(), false};
        }
        // The cues showing at this frame, each once (a node reached twice shows a cue once).
        struct Showing final {
            const Reached* from = nullptr;
            document::CueHandle cue;
        };
        std::map<std::pair<document::NodeId, std::uint64_t>, Showing> showing;
        for (const auto& item : reached) {
            if (!wanted(item.node))
                continue;
            reachedNodes.insert(item.node);
            for (const auto index : item.active)
                if (index < item.list->cues.size())
                    showing.try_emplace({item.node, item.list->cues[index]->id},
                                        Showing{&item, item.list->cues[index]});
        }
        // Continue or close the open runs.
        std::vector<Run> still;
        still.reserve(open.size());
        for (auto& run : open) {
            const auto found = showing.find({run.node, run.cue->id});
            if (found != showing.end() && *found->second.cue == *run.cue) {
                run.last = frame;
                advanceTags(run, frame, found->second.from->time);
                showing.erase(found);
                still.push_back(std::move(run));
            } else {
                closed.push_back(std::move(run));
            }
        }
        open = std::move(still);
        // Open a run for every cue that starts showing here.
        for (auto& [key, item] : showing) {
            Run run;
            run.node = key.first;
            run.list = item.from->list;
            run.cue = item.cue;
            run.first = frame;
            run.last = frame;
            collectTimestamps(document::cue_markup::parse(run.cue->text).tree.children, run.tags);
            std::ranges::sort(run.tags);
            run.tags.erase(std::ranges::unique(run.tags).begin(), run.tags.end());
            run.tagFrames.assign(run.tags.size(), std::nullopt);
            advanceTags(run, frame, item.from->time);
            open.push_back(std::move(run));
        }
        if (progress)
            progress(frame - request.firstFrame + 1, total);
    }
    for (auto& run : open)
        closed.push_back(std::move(run));

    SubtitleTracks result;
    std::map<document::NodeId, SubtitleTrack> tracks;
    std::map<document::NodeId, std::vector<std::pair<document::Cue, std::string>>> cues;
    for (const auto& run : closed) {
        const auto& nodeSettings = settingsOf(run.node);
        document::Cue cue = *run.cue;
        const auto start = outputTime(run.first - request.firstFrame, rate);
        const auto end = outputTime(run.last + 1 - request.firstFrame, rate);
        if (!start || !end)
            return SubtitleTrackFailure{"A subtitle cue time cannot be represented", false};
        cue.start = *start;
        cue.end = *end;
        if (!run.tags.empty()) {
            auto tree = document::cue_markup::parse(cue.text).tree;
            moveTimestamps(tree.children, run, request.firstFrame, rate);
            cue.text = document::cue_markup::serialize(tree);
        }
        if (nodeSettings.speakerPrefix && !cue.speaker.empty()) {
            cue.text = document::cue_markup::escapeText(cue.speaker + ": ") + cue.text;
            cue.speaker.clear();
        }
        auto plain = document::cue_markup::plainText(cue.text);
        // A cue with nothing to read draws nothing, so the track has nothing to show either.
        if (plain.empty())
            continue;
        auto& track = tracks[run.node];
        if (track.cues.cues.empty() && cues[run.node].empty()) {
            track.node = run.node;
            track.title = nodeSettings.title;
            track.language =
                !nodeSettings.language.empty() ? nodeSettings.language : run.list->language;
            track.cues.language = track.language;
            track.cues.styles = run.list->styles;
        }
        cues[run.node].emplace_back(std::move(cue), std::move(plain));
    }
    for (auto& [node, list] : cues) {
        auto& track = tracks[node];
        std::ranges::stable_sort(list, [](const auto& left, const auto& right) {
            return std::pair{left.first.start, left.first.end} <
                   std::pair{right.first.start, right.first.end};
        });
        std::uint64_t id = 1;
        for (auto& [cue, plain] : list) {
            cue.id = id++;
            track.cues.cues.push_back(std::make_shared<const document::Cue>(std::move(cue)));
            track.plainText.push_back(std::move(plain));
        }
        track.cues.nextCueId = id;
        if (const auto problem = track.cues.problem())
            return SubtitleTrackFailure{"The subtitle track of node " +
                                            std::to_string(node.value()) +
                                            " cannot be written: " + *problem,
                                        false};
        result.tracks.push_back(std::move(track));
    }
    // What the log says about nodes that wrote no track.
    if (request.nodes)
        for (const auto node : *request.nodes)
            if (!reachedNodes.contains(node))
                result.notes.push_back(
                    settingsOf(node).subtitles
                        ? "Subtitles node " + std::to_string(node.value()) +
                              " does not reach the output in the exported range; it has no track"
                        : "Node " + std::to_string(node.value()) +
                              " is not a Subtitles node; it has no track");
    for (const auto node : reachedNodes)
        if (!cues.contains(node))
            result.notes.push_back("Subtitles node " + std::to_string(node.value()) +
                                   " shows no cue in the exported range; it has no track");
    return result;
}

} // namespace bloom::runtime
