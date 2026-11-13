#include "command_test_support.hpp"

#include <bloom/commands/track_operations.hpp>
#include <bloom/commands/node_layout_placement.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/four_color_gradient_nodes.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/node_group_nodes.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace bloom::commands::test {
namespace {
using namespace document;

bool sameTruth(const Snapshot& left, const Snapshot& right) {
    if (left.project().id() != right.project().id() ||
        left.project().name() != right.project().name() ||
        !std::ranges::equal(left.project().extensionRecords(),
                            right.project().extensionRecords()) ||
        left.project().compositions().size() != right.project().compositions().size())
        return false;
    for (const auto& a : left.project().compositions()) {
        const auto* b = right.project().findComposition(a.id());
        if (!b || a.name() != b->name() || a.duration() != b->duration() ||
            a.format() != b->format() || a.workArea() != b->workArea() ||
            a.nodeLayout() != b->nodeLayout() || a.nodeGroups() != b->nodeGroups() ||
            !std::ranges::equal(a.parameters().records(), b->parameters().records()) ||
            !std::ranges::equal(a.animationCurves().records(), b->animationCurves().records()) ||
            !std::ranges::equal(a.graph().nodes(), b->graph().nodes()) ||
            !std::ranges::equal(a.graph().edges(), b->graph().edges()) ||
            !std::ranges::equal(a.graph().layerOutputs(), b->graph().layerOutputs()) ||
            !std::ranges::equal(a.graph().layerStack().entries(),
                                b->graph().layerStack().entries()) ||
            a.graph().layerStack().nodeId() != b->graph().layerStack().nodeId() ||
            a.graph().compositionOutput() != b->graph().compositionOutput())
            return false;
        if (a.graph().merges().size() != b->graph().merges().size())
            return false;
        for (const auto& merge : a.graph().merges()) {
            const auto* other = b->graph().merge(merge.nodeId());
            if (!other || merge.enabled() != other->enabled() ||
                !std::ranges::equal(merge.entries(), other->entries()))
                return false;
        }
    }
    return true;
}

struct Fixture {
    Fixture() = default;
    explicit Fixture(const CommandStack::HistoryLimits limits) : stack(document, limits) {}
    Document document{makeProject()};
    CommandStack stack{document};
};

template <typename Op, typename... Args> CommandResult apply(Fixture& fixture, Args&&... args) {
    Transaction transaction("Node edit", fixture.document.snapshot().revision());
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    return fixture.stack.execute(std::move(transaction));
}

template <typename Op, typename... Args>
CommandResult exercise(TestContext& test, Fixture& fixture, Args&&... args) {
    const auto before = fixture.document.snapshot();
    const auto historySize = fixture.stack.size();
    const auto result = apply<Op>(fixture, std::forward<Args>(args)...);
    const auto after = fixture.document.snapshot();
    test.expect(result.status == CommandStatus::Succeeded &&
                    fixture.stack.size() == historySize + 1,
                "one authoring operation creates exactly one transaction/history entry (status " +
                    std::to_string(static_cast<int>(result.status)) + ", history " +
                    std::to_string(fixture.stack.size() - historySize) + ")" +
                    (result.operationFailures.empty()
                         ? std::string()
                         : ": " + result.operationFailures.front().issue.message) +
                    (result.validation.ok()
                         ? std::string()
                         : ": " + result.validation.issues().front().path + " " +
                               result.validation.issues().front().message));
    if (!result.changed())
        return result;
    test.expect(after.revision().value() == before.revision().value() + 1 &&
                    fixture.stack.trackedRevision() == after.revision(),
                "execute revision parity");
    test.expect(fixture.stack.undo().changed(), "node edit undoes once");
    const auto undone = fixture.document.snapshot();
    test.expect(sameTruth(undone, before) &&
                    undone.revision().value() == after.revision().value() + 1,
                "undo restores every exact record, edge, source, curve, order and layout at a "
                "fresh revision");
    test.expect(undone.ids().highWater() == after.ids().highWater(),
                "undo retains issued IDs under the allocator monotonicity contract");
    test.expect(fixture.stack.redo().changed(), "node edit redoes once");
    test.expect(sameTruth(fixture.document.snapshot(), after) &&
                    fixture.document.snapshot().revision().value() ==
                        undone.revision().value() + 1 &&
                    fixture.document.snapshot().ids().highWater() == after.ids().highWater(),
                "redo restores pinned IDs and exact snapshot truth without rerunning allocation");
    return result;
}

template <typename Op, typename... Args>
void refuse(TestContext& test, Fixture& fixture, const OperationIssueCode code, Args&&... args) {
    const auto before = fixture.document.snapshot();
    const auto history = fixture.stack.size();
    Transaction transaction("Atomic refusal");
    transaction.emplace<SetProjectName>("Must roll back");
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    const auto result = fixture.stack.execute(std::move(transaction));
    test.expect(result.status == CommandStatus::Rejected && !result.operationFailures.empty() &&
                    result.operationFailures.front().operationIndex == 1 &&
                    result.operationFailures.front().issue.code == code,
                "invalid node operation reports its typed refusal: " +
                    (result.operationFailures.empty()
                         ? std::string("(no failure)")
                         : result.operationFailures.front().issue.message + " [index " +
                               std::to_string(result.operationFailures.front().operationIndex) +
                               ", code " +
                               std::to_string(static_cast<int>(
                                   result.operationFailures.front().issue.code)) +
                               ", expected " + std::to_string(static_cast<int>(code)) + "]"));
    const auto after = fixture.document.snapshot();
    test.expect(sameTruth(after, before) && after.revision() == before.revision() &&
                    after.ids().highWater() == before.ids().highWater() &&
                    fixture.stack.size() == history && result.outputs.empty(),
                "a refusal rolls back all draft edits, IDs, revision and history atomically");
}

NodeId addSource(Fixture& fixture) {
    const auto result = apply<AddNode>(fixture, std::string(kSolidSourceNodeType), Vec2d{4, 5});
    const auto id = result.outputId<NodeId>(kAddNodeOutput);
    if (!(id.has_value()))
        throw std::logic_error("source fixture adds");
    return *id;
}

void testMultipleMergeCommands(TestContext& test) {
    Fixture fixture;
    const auto added = exercise<AddNode>(test, fixture, std::string(kLayerStackNodeType), Vec2d{});
    const auto mergeId = added.outputId<NodeId>(kAddNodeOutput);
    if (!mergeId)
        return;
    const auto source = addSource(fixture);
    const InputPortRef pill = LayerStackInputRef{*mergeId, {}, "content"};
    const auto first = exercise<ConnectPorts>(test, fixture, OutputPortRef{source, "image"}, pill)
                           .outputId<LayerSlotId>(kConnectPortsSlotOutput);
    const auto second =
        exercise<ConnectPorts>(test, fixture, OutputPortRef{kFirstLayerNodeId, "image"}, pill)
            .outputId<LayerSlotId>(kConnectPortsSlotOutput);
    if (!first || !second)
        return;
    exercise<ReorderMergeInput>(test, fixture, *mergeId, *second, std::size_t{0});
    const auto snapshot = fixture.document.snapshot();
    test.expect(composition(snapshot).graph().merge(*mergeId)->entries().front().slotId == *second,
                "reorder changes the addressed Merge only");
    exercise<DisconnectInput>(test, fixture,
                              InputPortRef{LayerStackInputRef{*mergeId, *first, "content"}});
    exercise<RemoveNodes>(test, fixture, std::set<NodeId>{*mergeId});
}

// Task FOLLOW-1: a slot may now carry both a content and an audio edge for the same Layer.
// kFirstLayerNodeId already feeds kFirstSlotId's content edge (command_test_support's fixture
// graph); hand-wiring its own audio output onto the audio-role sentinel must reuse that slot
// rather than opening a second one, and disconnecting just the audio edge must leave the image
// row exactly where it was -- only a slot left with neither edge is removed.
void testMergeSlotSharesContentAndAudioRoles(TestContext& test) {
    Fixture fixture;
    const InputPortRef audioPill =
        LayerStackInputRef{kLayerStackNodeId, {}, std::string(kLayerStackAudioInputRole)};
    const auto audioSlot =
        exercise<ConnectPorts>(
            test, fixture,
            OutputPortRef{kFirstLayerNodeId, std::string(kLayerOutputAudioOutputPort)}, audioPill)
            .outputId<LayerSlotId>(kConnectPortsSlotOutput);
    if (!audioSlot)
        return;
    test.expect(*audioSlot == kFirstSlotId,
                "the hand-wired audio edge reuses the Layer's existing content slot");
    auto snapshot = fixture.document.snapshot();
    test.expect(composition(snapshot).graph().merge(kLayerStackNodeId)->entries().size() == 2,
                "no second slot is opened for the same Layer");

    exercise<DisconnectInput>(
        test, fixture,
        InputPortRef{LayerStackInputRef{kLayerStackNodeId, kFirstSlotId,
                                        std::string(kLayerStackAudioInputRole)}});
    snapshot = fixture.document.snapshot();
    test.expect(composition(snapshot).graph().merge(kLayerStackNodeId)->find(kFirstSlotId) !=
                        nullptr &&
                    composition(snapshot).graph().merge(kLayerStackNodeId)->entries().size() == 2,
                "removing just the audio edge leaves the Layer's image row in place");
    const auto edgesAfterAudioDisconnect = composition(snapshot).graph().edges();
    test.expect(std::ranges::any_of(edgesAfterAudioDisconnect,
                                    [](const auto& edge) {
                                        return edge.destination ==
                                               InputPortRef(LayerStackInputRef{
                                                   kLayerStackNodeId, kFirstSlotId,
                                                   std::string(kLayerStackContentInputRole)});
                                    }),
                "the content edge itself survives the audio disconnect");

    exercise<DisconnectInput>(
        test, fixture,
        InputPortRef{LayerStackInputRef{kLayerStackNodeId, kFirstSlotId,
                                        std::string(kLayerStackContentInputRole)}});
    snapshot = fixture.document.snapshot();
    test.expect(composition(snapshot).graph().merge(kLayerStackNodeId)->find(kFirstSlotId) ==
                        nullptr &&
                    composition(snapshot).graph().merge(kLayerStackNodeId)->entries().size() == 1,
                "a slot left with neither edge is removed, exactly as before");
}

void testLayerToggles(TestContext& test) {
    Fixture fixture;
    // WORKAREA-1: a work-area edit is render-neutral -- it scopes which frames a range command
    // caches, not what a pixel looks like -- so both range commands opt out of render effect.
    const auto setArea = exercise<SetWorkArea>(test, fixture, core::RationalTime::fromInteger(1),
                                               core::RationalTime::fromInteger(3));
    test.expect(!setArea.renderAffecting, "SetWorkArea is render-neutral");
    refuse<SetWorkArea>(test, fixture, OperationIssueCode::InvalidValue,
                        core::RationalTime::fromInteger(3), core::RationalTime::fromInteger(1));
    const auto clearArea = exercise<ClearWorkArea>(test, fixture);
    test.expect(!clearArea.renderAffecting, "ClearWorkArea is render-neutral");
    // A mixed range+pixel transaction still aggregates to render-affecting.
    {
        const auto before = fixture.document.snapshot();
        Transaction mixed("Range and opacity", before.revision());
        mixed.emplace<SetWorkArea>(kCompositionId, core::RationalTime::fromInteger(2),
                                   core::RationalTime::fromInteger(4));
        mixed.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                          document::ParameterSource{ConstantValueSource{0.5}});
        const auto mixedResult = fixture.stack.execute(std::move(mixed));
        test.expect(mixedResult.changed() && mixedResult.renderAffecting,
                    "a mixed range+pixel transaction stays render-affecting");
        const auto mixedUndo = fixture.stack.undo();
        test.expect(mixedUndo.changed() && mixedUndo.renderAffecting,
                    "undo replays the stored mixed range impact");
        const auto mixedRedo = fixture.stack.redo();
        test.expect(mixedRedo.changed() && mixedRedo.renderAffecting,
                    "redo replays the stored mixed range impact");
    }
    (void)exercise<SetLayerLabelColor>(test, fixture, kFirstLayerId,
                                       std::array<std::uint8_t, 3>{12, 34, 56});
    (void)exercise<SetLayerLabelColor>(test, fixture, kFirstLayerId, std::nullopt);
    (void)exercise<SetLayerEnabled>(test, fixture, kFirstLayerId, false);
    auto snapshot = fixture.document.snapshot();
    test.expect(snapshot.project()
                    .findComposition(kCompositionId)
                    ->nodeLayout()
                    .at(kFirstLayerNodeId)
                    .muted,
                "visibility synchronizes Layer node mute");
    (void)exercise<SetLayerSolo>(test, fixture, kFirstLayerId, true);
    const auto animation = exercise<CreateAnimationForParameter>(test, fixture, kFirstPositionId,
                                                                 core::RationalTime{});
    const auto curve = animation.outputId<AnimationCurveId>(kAnimationCurveOutput);
    (void)exercise<SetLayerLocked>(test, fixture, kFirstLayerId, true);
    if (curve)
        refuse<InsertVec2Keyframe>(test, fixture, OperationIssueCode::InvalidValue, *curve,
                                   core::RationalTime::fromInteger(1), Vec2d{9, 9});
    refuse<SetLayerRange>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId,
                          core::RationalTime{}, core::RationalTime::fromInteger(2));
    refuse<MoveLayerBefore>(test, fixture, OperationIssueCode::InvalidValue, kFirstSlotId,
                            std::optional<LayerSlotId>{});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, kFirstPositionId,
                               ConstantValueSource{Vec2d{9, 9}});
    (void)exercise<SetLayerLocked>(test, fixture, kFirstLayerId, false);
}

// TEMPORAL-1: SetLayerRange is the only operation that opts into a finite changed-time footprint.
// Its footprint is the symmetric difference of the old and new half-open activity spans, snapped to
// composition frames. Trimming yields one interval, extending yields one, a shifted equal-length
// move yields two, and a no-op yields noChange (hence no footprint).
void testLayerRangeTimeFootprint(TestContext& test) {
    using document::CompositionId;
    Fixture fixture;
    const auto setRange = [&fixture](const core::RationalTime in, const core::RationalTime out) {
        return apply<SetLayerRange>(fixture, kFirstLayerId, in, out);
    };
    const auto expectSpans =
        [&test](const std::optional<AffectedTimeFootprint>& footprint,
                const std::vector<std::pair<core::RationalTime, core::RationalTime>>& spans,
                const std::string_view message) {
            test.expect(footprint.has_value() && footprint->compositionId == kCompositionId &&
                            footprint->intervals.size() == spans.size(),
                        message);
            if (!footprint.has_value() || footprint->intervals.size() != spans.size())
                return;
            for (std::size_t index = 0; index < spans.size(); ++index) {
                test.expect(footprint->intervals[index].start == spans[index].first &&
                                footprint->intervals[index].end == spans[index].second,
                            message);
            }
        };

    // The fixture layer starts active for the whole [0,10) composition.
    // Trim the right edge to [0,4): the departing [4,10) is the only change.
    expectSpans(setRange(core::RationalTime::fromInteger(0), core::RationalTime::fromInteger(4))
                    .affectedTimes,
                {{core::RationalTime::fromInteger(4), core::RationalTime::fromInteger(10)}},
                "trim right has one changed interval");

    // Extend the right edge back to [0,7): the entering [4,7) is the only change.
    expectSpans(setRange(core::RationalTime::fromInteger(0), core::RationalTime::fromInteger(7))
                    .affectedTimes,
                {{core::RationalTime::fromInteger(4), core::RationalTime::fromInteger(7)}},
                "extend right has one changed interval");

    // Trim the left edge to [2,7): the departing [0,2) is the only change.
    expectSpans(setRange(core::RationalTime::fromInteger(2), core::RationalTime::fromInteger(7))
                    .affectedTimes,
                {{core::RationalTime::fromInteger(0), core::RationalTime::fromInteger(2)}},
                "trim left has one changed interval");

    // Shift equal length [2,7) -> [4,9): both [2,4) and [7,9) change.
    expectSpans(setRange(core::RationalTime::fromInteger(4), core::RationalTime::fromInteger(9))
                    .affectedTimes,
                {{core::RationalTime::fromInteger(2), core::RationalTime::fromInteger(4)},
                 {core::RationalTime::fromInteger(7), core::RationalTime::fromInteger(9)}},
                "an equal-length shift has two changed intervals");

    // Disjoint move [4,9) -> [1,2): old span and new span do not overlap.
    expectSpans(setRange(core::RationalTime::fromInteger(1), core::RationalTime::fromInteger(2))
                    .affectedTimes,
                {{core::RationalTime::fromInteger(1), core::RationalTime::fromInteger(2)},
                 {core::RationalTime::fromInteger(4), core::RationalTime::fromInteger(9)}},
                "a disjoint move reports both spans normalized");

    // A full-duration layer with an explicit outPoint equal to duration is the same as absent: from
    // [1,2) to [0,10) changes [0,1) and [2,10).
    const auto full = setRange(core::RationalTime{}, core::RationalTime::fromInteger(10));
    expectSpans(full.affectedTimes,
                {{core::RationalTime::fromInteger(0), core::RationalTime::fromInteger(1)},
                 {core::RationalTime::fromInteger(2), core::RationalTime::fromInteger(10)}},
                "restoring full duration reports the whole changed span");
    const auto fullAgain = setRange(core::RationalTime{}, core::RationalTime::fromInteger(10));
    test.expect(fullAgain.status == CommandStatus::NoChange && !fullAgain.affectedTimes.has_value(),
                "a no-op range edit advertises no footprint");

    // An unknown pixel operation alongside a finite footprint dominates to whole render (no
    // footprint); a NoChange pixel operation leaves the finite footprint intact.
    {
        const auto before = fixture.document.snapshot();
        Transaction mixed("Range plus unknown pixel", before.revision());
        mixed.emplace<SetLayerRange>(kCompositionId, kFirstLayerId,
                                     core::RationalTime::fromInteger(1),
                                     core::RationalTime::fromInteger(2));
        mixed.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                          document::ParameterSource{ConstantValueSource{0.5}});
        const auto mixedResult = fixture.stack.execute(std::move(mixed));
        test.expect(mixedResult.changed() && mixedResult.renderAffecting &&
                        !mixedResult.affectedTimes.has_value(),
                    "a finite footprint beside an unknown pixel op falls back to whole render");
    }
    {
        // Set the range to [1,2) first, then a transaction whose pixel op is a NoChange.
        (void)setRange(core::RationalTime::fromInteger(1), core::RationalTime::fromInteger(2));
        const auto before = fixture.document.snapshot();
        Transaction mixed("Range plus pixel no-change", before.revision());
        mixed.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                          document::ParameterSource{ConstantValueSource{0.5}});
        mixed.emplace<SetLayerRange>(kCompositionId, kFirstLayerId,
                                     core::RationalTime::fromInteger(1),
                                     core::RationalTime::fromInteger(3));
        const auto mixedResult = fixture.stack.execute(std::move(mixed));
        test.expect(mixedResult.changed() && mixedResult.renderAffecting &&
                        mixedResult.affectedTimes.has_value() &&
                        mixedResult.affectedTimes->intervals.size() == 1 &&
                        mixedResult.affectedTimes->intervals.front().start ==
                            core::RationalTime::fromInteger(2) &&
                        mixedResult.affectedTimes->intervals.front().end ==
                            core::RationalTime::fromInteger(3),
                    "a NoChange pixel op leaves the finite range footprint intact");
    }

    // A render-neutral range edit (work area) beside a finite layer range: work area contributes
    // nothing, so the footprint stays finite.
    {
        const auto before = fixture.document.snapshot();
        // The layer is at [1,3) here; trim to [1,2) so the change is exactly [2,3).
        Transaction mixed("Range plus work area", before.revision());
        mixed.emplace<SetLayerRange>(kCompositionId, kFirstLayerId,
                                     core::RationalTime::fromInteger(1),
                                     core::RationalTime::fromInteger(2));
        mixed.emplace<SetWorkArea>(kCompositionId, core::RationalTime::fromInteger(1),
                                   core::RationalTime::fromInteger(5));
        const auto mixedResult = fixture.stack.execute(std::move(mixed));
        test.expect(mixedResult.changed() && mixedResult.renderAffecting &&
                        mixedResult.affectedTimes.has_value() &&
                        mixedResult.affectedTimes->intervals.size() == 1 &&
                        mixedResult.affectedTimes->intervals.front().start ==
                            core::RationalTime::fromInteger(2) &&
                        mixedResult.affectedTimes->intervals.front().end ==
                            core::RationalTime::fromInteger(3),
                    "a neutral work-area edit does not widen the finite range footprint");
    }

    // Undo/redo replay the stored symmetric footprint.
    {
        const auto undo = fixture.stack.undo();
        test.expect(undo.changed() && undo.affectedTimes.has_value() &&
                        undo.affectedTimes->intervals.size() == 1,
                    "undo replays the stored finite footprint");
        const auto redo = fixture.stack.redo();
        test.expect(redo.changed() && redo.affectedTimes.has_value() &&
                        redo.affectedTimes->intervals.size() == 1,
                    "redo replays the stored finite footprint");
    }
}

void testLayerRanges(TestContext& test) {
    Fixture fixture;
    (void)exercise<CreateAnimationForParameter>(test, fixture, kFirstPositionId,
                                                core::RationalTime{});
    const auto in = core::RationalTime::fromInteger(1), out = core::RationalTime::fromInteger(4);
    (void)exercise<SetLayerRange>(test, fixture, kFirstLayerId, in, out);
    refuse<SetLayerRange>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId, out, in);
    refuse<SplitLayerAtTime>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId, in);
    refuse<SplitLayerAtTime>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId,
                             core::RationalTime::fromInteger(100));
    const auto split = exercise<SplitLayerAtTime>(test, fixture, kFirstLayerId,
                                                  core::RationalTime::fromInteger(2));
    const auto copy = split.outputId<LayerId>("layer");
    const auto snapshot = fixture.document.snapshot();
    const auto& graph = snapshot.project().findComposition(kCompositionId)->graph();
    test.expect(copy && graph.findLayer(*copy)->inPoint == core::RationalTime::fromInteger(2) &&
                    graph.findLayer(*copy)->outPoint == out &&
                    graph.findLayer(kFirstLayerId)->outPoint == core::RationalTime::fromInteger(2),
                "split keeps adjacent half-open ranges and exact undo/redo IDs");
}

// SPLIT-1: an ordinary merge-connected Layer Output split proves output equivalence and publishes a
// deliberately EMPTY pixel footprint plus an original->tail identity remap. Conservative cases
// (non-merge consumer, surviving child, unsupported boundary) publish no footprint and no remaps.
void testSplitEquivalenceEvidence(TestContext& test) {
    using document::CompositionId;
    const auto t = [](const std::int64_t value) { return core::RationalTime::fromInteger(value); };
    {
        Fixture fixture;
        // Animate the layer's position so the proof must accept exact curve duplication.
        (void)exercise<CreateAnimationForParameter>(test, fixture, kFirstPositionId,
                                                    core::RationalTime{});
        (void)apply<SetLayerRange>(fixture, kFirstLayerId, t(1), t(8));
        const auto split = apply<SplitLayerAtTime>(fixture, kFirstLayerId, t(4));
        const auto copy = split.outputId<LayerId>("layer");
        test.expect(split.changed() && split.renderAffecting && copy.has_value(),
                    "an ordinary split publishes");
        if (!copy.has_value())
            throw std::logic_error("split equivalence fixture");
        test.expect(split.affectedTimes.has_value() && split.affectedTimes->intervals.empty(),
                    "an equivalent split publishes a deliberately empty pixel footprint");
        test.expect(split.layerIdentityRemaps.has_value() &&
                        split.layerIdentityRemaps->size() == 1 &&
                        split.layerIdentityRemaps->front().compositionId == kCompositionId &&
                        split.layerIdentityRemaps->front().start == t(4) &&
                        split.layerIdentityRemaps->front().end == t(8) &&
                        split.layerIdentityRemaps->front().beforeLayerId == kFirstLayerId &&
                        split.layerIdentityRemaps->front().afterLayerId == *copy,
                    "an equivalent split publishes the original->tail remap over the tail span");
    }
    {
        // F1 regression: with the Layer Stack muted only the first slot compiles. Splitting that
        // first-slot layer leaves the head inactive over the tail span while the tail copy is
        // pruned, so live output changes there and the split cannot claim equivalence.
        Fixture fixture;
        (void)apply<SetLayerRange>(fixture, kFirstLayerId, t(0), t(5));
        (void)apply<SetNodeMuted>(fixture, kLayerStackNodeId, true);
        const auto split = apply<SplitLayerAtTime>(fixture, kFirstLayerId, t(2));
        test.expect(split.changed() && split.renderAffecting && !split.affectedTimes.has_value() &&
                        !split.layerIdentityRemaps.has_value(),
                    "splitting the first layer of a muted layer stack keeps whole-render");
    }
    {
        // A non-merge downstream consumer of the original output is conservative. The second Layer
        // Output's image input is a node input, not a Merge slot.
        Fixture fixture;
        if (!apply<ConnectPorts>(fixture, OutputPortRef{kFirstLayerNodeId, "image"},
                                 InputPortRef{NodeInputRef{kSecondLayerNodeId, "image"}})
                 .changed())
            throw std::logic_error("split consumer fixture");
        const auto split = apply<SplitLayerAtTime>(fixture, kFirstLayerId, t(2));
        test.expect(split.changed() && !split.affectedTimes.has_value() &&
                        !split.layerIdentityRemaps.has_value(),
                    "a non-merge consumer keeps the split conservative (whole render)");
    }
    {
        // A surviving child parented to the split layer is conservative.
        Fixture fixture;
        (void)apply<SetLayerParent>(fixture, kSecondLayerId, kFirstLayerId);
        const auto split = apply<SplitLayerAtTime>(fixture, kFirstLayerId, t(2));
        test.expect(split.changed() && !split.affectedTimes.has_value() &&
                        !split.layerIdentityRemaps.has_value(),
                    "a surviving child keeps the split conservative (whole render)");
    }

    {
        // Locked/invalid splits keep their existing refusal semantics and publish no evidence.
        Fixture fixture;
        (void)apply<SetLayerLocked>(fixture, kFirstLayerId, true);
        const auto locked = apply<SplitLayerAtTime>(fixture, kFirstLayerId, t(2));
        test.expect(locked.status == CommandStatus::Rejected && !locked.affectedTimes.has_value() &&
                        !locked.layerIdentityRemaps.has_value(),
                    "a locked layer split is rejected with no evidence");
        (void)apply<SetLayerLocked>(fixture, kFirstLayerId, false);
        const auto invalid = apply<SplitLayerAtTime>(fixture, kFirstLayerId, t(100));
        test.expect(invalid.status == CommandStatus::Rejected &&
                        !invalid.layerIdentityRemaps.has_value(),
                    "an out-of-range split is rejected with no evidence");
    }
}

void testValidityQuery(TestContext& test) {
    Fixture fixture;
    const auto before = fixture.document.snapshot();
    const auto history = fixture.stack.size();
    test.expect(canApplyNodeOperation(
                    before, DuplicateNodes(kCompositionId, {kFirstLayerNodeId}, {24, 24})),
                "menu query accepts a valid duplicate without issuing IDs");
    test.expect(canApplyNodeOperation(before, RemoveNodes(kCompositionId, {kLayerStackNodeId})),
                "#716: menu query accepts removing the composition's Merge");
    test.expect(canApplyNodeOperation(before, DissolveNode(kCompositionId, kFirstLayerNodeId)),
                "#704: menu query accepts a Layer Output dissolve with nothing to reconnect "
                "through; it deletes the node");
    test.expect(canApplyNodeOperation(before, DissolveNode(kCompositionId, kLayerStackNodeId)),
                "#716: menu query accepts dissolving the composition's Merge");
    const auto after = fixture.document.snapshot();
    test.expect(sameTruth(before, after) && before.revision() == after.revision() &&
                    before.ids().highWater() == after.ids().highWater() &&
                    fixture.stack.size() == history,
                "queries leave all live truth, allocator, revision and history untouched");
}

void testResizeFitParameters(TestContext& test) {
    for (const auto* type : {"bloom.resize", "bloom.fit"}) {
        Fixture fixture;
        const auto result = exercise<AddNode>(test, fixture, type, Vec2d{});
        const auto width = result.outputId<ParameterId>("parameter.width");
        const auto aspect = result.outputId<ParameterId>("parameter.pixelAspectNumerator");
        const auto filter = result.outputId<ParameterId>("parameter.filter");
        if (!width || !aspect || !filter)
            throw std::logic_error("Resize/Fit exposes registered control identities");
        (void)exercise<SetParameterSource>(test, fixture, *width,
                                           ConstantValueSource{std::int64_t{2048}});
        (void)exercise<SetParameterSource>(test, fixture, *aspect,
                                           ConstantValueSource{std::int64_t{2}});
        refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *width,
                                   ConstantValueSource{std::int64_t{0}});
        refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aspect,
                                   ConstantValueSource{std::int64_t{0}});
        // #480: Nearest (1) is the second filter; nothing beyond it is.
        (void)exercise<SetParameterSource>(test, fixture, *filter,
                                           ConstantValueSource{std::int64_t{1}});
        refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *filter,
                                   ConstantValueSource{std::int64_t{2}});
        if (const auto policy = result.outputId<ParameterId>("parameter.policy")) {
            (void)exercise<SetParameterSource>(test, fixture, *policy,
                                               ConstantValueSource{std::int64_t{1}});
            refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *policy,
                                       ConstantValueSource{std::int64_t{2}});
        }
    }
}

// #336: Transform 2D is added, edited, undone and redone through the generic node commands, and
// its schema refuses values the renderer could not honour.
void testTransform2DParameters(TestContext& test) {
    Fixture fixture;
    const auto result = exercise<AddNode>(test, fixture, "bloom.transform-2d", Vec2d{});
    const auto translate = result.outputId<ParameterId>("parameter.translate");
    const auto scale = result.outputId<ParameterId>("parameter.scale");
    const auto rotation = result.outputId<ParameterId>("parameter.rotation");
    const auto extent = result.outputId<ParameterId>("parameter.extent");
    const auto filter = result.outputId<ParameterId>("parameter.filter");
    if (!translate || !scale || !rotation || !extent || !filter)
        throw std::logic_error("Transform 2D exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *translate,
                                       ConstantValueSource{Vec2d{-12.5, 40.0}});
    (void)exercise<SetParameterSource>(test, fixture, *scale, ConstantValueSource{Vec2d{0.0, -2.0}});
    (void)exercise<SetParameterSource>(test, fixture, *rotation, ConstantValueSource{-725.0});
    (void)exercise<SetParameterSource>(test, fixture, *extent,
                                       ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *translate,
                               ConstantValueSource{Vec2d{std::numeric_limits<double>::infinity(), 0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *rotation,
                               ConstantValueSource{std::numeric_limits<double>::quiet_NaN()});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *extent,
                               ConstantValueSource{std::int64_t{3}});
    (void)exercise<SetParameterSource>(test, fixture, *filter,
                                       ConstantValueSource{std::int64_t{1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *filter,
                               ConstantValueSource{std::int64_t{2}});
}

// #337: Matte Blur is added, wired Mask to Mask, edited, undone and redone through the generic node
// commands. An Image never connects to its Mask input, and its schema refuses unrenderable values.
// #338 Matte Erode and Matte Dilate follow exactly the same contract.
void testMatteBlurCommands(TestContext& test, const char* type = "bloom.matte-blur") {
    Fixture fixture;
    const auto blur = exercise<AddNode>(test, fixture, type, Vec2d{});
    const auto blurId = blur.outputId<NodeId>(kAddNodeOutput);
    const auto radius = blur.outputId<ParameterId>("parameter.radius");
    const auto edge = blur.outputId<ParameterId>("parameter.edge");
    const auto mask =
        apply<AddNode>(fixture, "bloom.mask", Vec2d{40, 0}).outputId<NodeId>(kAddNodeOutput);
    const auto mix =
        apply<AddNode>(fixture, "bloom.image-mix", Vec2d{80, 0}).outputId<NodeId>(kAddNodeOutput);
    if (!blurId || !radius || !edge || !mask || !mix)
        throw std::logic_error("Matte Blur exposes registered control identities");
    const InputPortRef input = NodeInputRef{*blurId, "input"};
    exercise<ConnectPorts>(test, fixture, OutputPortRef{*mask, "mask"}, input);
    exercise<ConnectPorts>(test, fixture, OutputPortRef{*blurId, "mask"},
                           InputPortRef{NodeInputRef{*mix, "mask"}});
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{addSource(fixture), "image"}, input);
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{*blurId, "mask"},
                         InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}});
    (void)exercise<SetParameterSource>(test, fixture, *radius, ConstantValueSource{12.75});
    (void)exercise<SetParameterSource>(test, fixture, *edge, ConstantValueSource{std::int64_t{1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *radius,
                               ConstantValueSource{257.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *radius,
                               ConstantValueSource{std::numeric_limits<double>::quiet_NaN()});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *edge,
                               ConstantValueSource{std::int64_t{2}});
}

// #479: Separate Channels and Combine Channels are added between an Image and a Layer, wired
// channel by channel, edited, undone and redone through the generic node commands; the source and
// alpha mode dropdowns refuse values outside their enumerations and accept their entry names.
void testImageChannelCommands(TestContext& test) {
    Fixture fixture;
    const auto separate = exercise<AddNode>(test, fixture, "bloom.separate-channels", Vec2d{});
    const auto separateId = separate.outputId<NodeId>(kAddNodeOutput);
    const auto combine = exercise<AddNode>(test, fixture, "bloom.combine-channels", Vec2d{40, 0});
    const auto combineId = combine.outputId<NodeId>(kAddNodeOutput);
    const auto alphaSource = combine.outputId<ParameterId>("parameter.alphaSource");
    const auto alphaMode = combine.outputId<ParameterId>("parameter.alphaMode");
    if (!separateId || !combineId || !alphaSource || !alphaMode)
        throw std::logic_error("channel nodes expose registered control identities");
    exercise<ConnectPorts>(test, fixture, OutputPortRef{addSource(fixture), "image"},
                           InputPortRef{NodeInputRef{*separateId, "input"}});
    // Swap red and blue, keep green and carry alpha.
    for (const auto& [output, input] : {std::pair{"blue", "red"}, std::pair{"green", "green"},
                                        std::pair{"red", "blue"}, std::pair{"alpha", "alpha"}})
        exercise<ConnectPorts>(test, fixture, OutputPortRef{*separateId, output},
                               InputPortRef{NodeInputRef{*combineId, input}});
    (void)exercise<SetParameterSource>(test, fixture, *alphaSource,
                                       ConstantValueSource{std::int64_t{4}});
    (void)exercise<SetParameterSource>(test, fixture, *alphaMode,
                                       ConstantValueSource{std::int64_t{1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *alphaSource,
                               ConstantValueSource{std::int64_t{5}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *alphaSource,
                               ConstantValueSource{std::int64_t{-1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *alphaMode,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *alphaMode,
                               ConstantValueSource{std::string{"Unassociated"}});
    // A dropdown entry is also authorable by its name (a script or the MCP surface).
    (void)exercise<SetParameterSource>(test, fixture, *alphaMode,
                                       ConstantValueSource{std::string{"Premultiplied"}});
}

// #340: Levels, Curves and Hue/Saturation are added between an Image and a Layer, edited, undone
// and redone through the generic node commands; their schemas refuse unrenderable values and keep
// HDR and Raw-data ranges authorable.
void testColorGradeCommands(TestContext& test) {
    Fixture fixture;
    const auto levels = exercise<AddNode>(test, fixture, "bloom.levels", Vec2d{});
    const auto levelsId = levels.outputId<NodeId>(kAddNodeOutput);
    const auto inputWhite = levels.outputId<ParameterId>("parameter.masterInputWhite");
    const auto gamma = levels.outputId<ParameterId>("parameter.redGamma");
    const auto clamp = levels.outputId<ParameterId>("parameter.clamp");
    const auto curves = exercise<AddNode>(test, fixture, "bloom.curves", Vec2d{40, 0});
    const auto curvesId = curves.outputId<NodeId>(kAddNodeOutput);
    const auto point = curves.outputId<ParameterId>("parameter.alpha");
    const auto hueShift = curves.outputId<ParameterId>("parameter.hueVsHue");
    const auto hue = exercise<AddNode>(test, fixture, "bloom.hue-saturation", Vec2d{80, 0});
    const auto hueId = hue.outputId<NodeId>(kAddNodeOutput);
    const auto saturation = hue.outputId<ParameterId>("parameter.saturation");
    if (!levelsId || !inputWhite || !gamma || !clamp || !curvesId || !point || !hueShift ||
        !hueId || !saturation)
        throw std::logic_error("grades expose registered control identities");
    exercise<ConnectPorts>(test, fixture, OutputPortRef{addSource(fixture), "image"},
                           InputPortRef{NodeInputRef{*levelsId, "input"}});
    exercise<ConnectPorts>(test, fixture, OutputPortRef{*levelsId, "image"},
                           InputPortRef{NodeInputRef{*curvesId, "input"}});
    exercise<ConnectPorts>(test, fixture, OutputPortRef{*curvesId, "image"},
                           InputPortRef{NodeInputRef{*hueId, "input"}});
    // A Raw depth range and an HDR curve point stay authorable.
    (void)exercise<SetParameterSource>(test, fixture, *inputWhite, ConstantValueSource{1000.0});
    (void)exercise<SetParameterSource>(test, fixture, *gamma, ConstantValueSource{2.2});
    (void)exercise<SetParameterSource>(test, fixture, *clamp, ConstantValueSource{true});
    // #493: a curve of any number of points with an HDR point, and a hue curve.
    (void)exercise<SetParameterSource>(
        test, fixture, *point,
        ConstantValueSource{gradeCurvePath({{0.0, 0.0}, {0.3, 0.5}, {0.6, 0.7}, {4.0, 6.0}})});
    (void)exercise<SetParameterSource>(test, fixture, *hueShift,
                                       ConstantValueSource{gradeCurvePath({{0.5, -0.25}})});
    (void)exercise<SetParameterSource>(test, fixture, *saturation, ConstantValueSource{2.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *gamma,
                               ConstantValueSource{0.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *inputWhite,
                               ConstantValueSource{std::numeric_limits<double>::infinity()});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *point,
                               ConstantValueSource{gradeCurvePath({{2.0e6, 0.0}})});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *point,
                               ConstantValueSource{Vec2d{0.5, 0.5}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *hueShift,
                               ConstantValueSource{gradeCurvePath({{0.5, 0.75}})});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *hueShift,
                               ConstantValueSource{gradeCurvePath({{1.25, 0.0}})});
    // #493: a curve is animatable as a whole value: keyed at one time, keyed again at another
    // (each one undoable command), refused out of its domain, and returned to a constant.
    const auto keyed =
        exercise<CreateAnimationForParameter>(test, fixture, *hueShift, core::RationalTime{});
    const auto pathCurve = keyed.outputId<AnimationCurveId>(kAnimationCurveOutput);
    test.expect(pathCurve.has_value(), "a curve creates a whole-value Path curve");
    if (pathCurve) {
        const auto later = gradeCurvePath({{0.25, 0.125}, {0.75, -0.125}});
        (void)exercise<SetKeyframeAtTime>(test, fixture, *pathCurve,
                                          core::RationalTime::fromInteger(2), later);
        const auto* stored = composition(fixture.document.snapshot())
                                 .animationCurves()
                                 .findPath(*pathCurve);
        test.expect(stored && stored->keyframes.size() == 2 &&
                        stored->keyframes.front().value == gradeCurvePath({{0.5, -0.25}}) &&
                        stored->keyframes.back().value == later,
                    "a curve key holds the whole point list at its time");
        refuse<SetKeyframeAtTime>(test, fixture, OperationIssueCode::InvalidValue, *pathCurve,
                                  core::RationalTime::fromInteger(3),
                                  gradeCurvePath({{0.5, 0.9}}));
        (void)exercise<ConvertAnimationToConstant>(test, fixture, *hueShift, later);
        const auto* record = composition(fixture.document.snapshot()).parameters().find(*hueShift);
        test.expect(record &&
                        std::get<ConstantValueSource>(record->source).value ==
                            ParameterValue{later} &&
                        !composition(fixture.document.snapshot()).animationCurves().find(
                            *pathCurve),
                    "a keyed curve returns to a constant");
    }
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *saturation,
                               ConstantValueSource{-0.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *clamp,
                               ConstantValueSource{1.0});
}

// #338: Luma Key is added, keyed from an Image into Image Mix's or a matte filter's Mask input,
// edited, undone and redone through the generic node commands. Its Mask never connects to an
// Image input, a Mask never connects to its Image input, and its schema refuses unrenderable
// thresholds.
void testLumaKeyCommands(TestContext& test) {
    Fixture fixture;
    const auto key = exercise<AddNode>(test, fixture, "bloom.luma-key", Vec2d{});
    const auto keyId = key.outputId<NodeId>(kAddNodeOutput);
    const auto low = key.outputId<ParameterId>("parameter.low");
    const auto high = key.outputId<ParameterId>("parameter.high");
    const auto invert = key.outputId<ParameterId>("parameter.invert");
    const auto erode =
        apply<AddNode>(fixture, "bloom.matte-erode", Vec2d{40, 0}).outputId<NodeId>(kAddNodeOutput);
    const auto mask =
        apply<AddNode>(fixture, "bloom.mask", Vec2d{60, 0}).outputId<NodeId>(kAddNodeOutput);
    const auto mix =
        apply<AddNode>(fixture, "bloom.image-mix", Vec2d{80, 0}).outputId<NodeId>(kAddNodeOutput);
    if (!keyId || !low || !high || !invert || !erode || !mask || !mix)
        throw std::logic_error("Luma Key exposes registered control identities");
    const InputPortRef input = NodeInputRef{*keyId, "input"};
    exercise<ConnectPorts>(test, fixture, OutputPortRef{addSource(fixture), "image"}, input);
    exercise<ConnectPorts>(test, fixture, OutputPortRef{*keyId, "mask"},
                           InputPortRef{NodeInputRef{*erode, "input"}});
    exercise<ConnectPorts>(test, fixture, OutputPortRef{*erode, "mask"},
                           InputPortRef{NodeInputRef{*mix, "mask"}});
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{*mask, "mask"}, input);
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{*keyId, "mask"},
                         InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}});
    (void)exercise<SetParameterSource>(test, fixture, *low, ConstantValueSource{-0.5});
    (void)exercise<SetParameterSource>(test, fixture, *high, ConstantValueSource{8.0});
    (void)exercise<SetParameterSource>(test, fixture, *invert, ConstantValueSource{true});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *low,
                               ConstantValueSource{std::numeric_limits<double>::quiet_NaN()});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *high,
                               ConstantValueSource{2.0e6});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *invert,
                               ConstantValueSource{std::int64_t{1}});
}

// #339: Drop Shadow, Glow and Light Wrap are added, wired (Glow's key pass and Mask, Light Wrap's
// background), keyed and edited, undone and redone through the generic node commands. A Mask never
// reaches Glow's key, and the schemas refuse values the recipes cannot render.
void testRecipeCommands(TestContext& test) {
    Fixture fixture;
    const auto addRecipe = [&](const char* type) {
        const auto added = exercise<AddNode>(test, fixture, type, Vec2d{});
        const auto id = added.outputId<NodeId>(kAddNodeOutput);
        if (!id)
            throw std::logic_error("recipe exposes its node identity");
        exercise<ConnectPorts>(test, fixture, OutputPortRef{addSource(fixture), "image"},
                               InputPortRef{NodeInputRef{*id, "input"}});
        return added;
    };
    const auto shadow = addRecipe("bloom.drop-shadow");
    const auto distance = shadow.outputId<ParameterId>("parameter.distance");
    const auto opacity = shadow.outputId<ParameterId>("parameter.opacity");
    const auto color = shadow.outputId<ParameterId>("parameter.color");
    const auto shadowOnly = shadow.outputId<ParameterId>("parameter.shadowOnly");
    if (!distance || !opacity || !color || !shadowOnly)
        throw std::logic_error("Drop Shadow exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *distance, ConstantValueSource{24.0});
    (void)exercise<SetParameterSource>(
        test, fixture, *color, ConstantValueSource{bloom::core::Color4d{0.1, 0.0, 0.3, 0.5}});
    (void)exercise<SetParameterSource>(test, fixture, *shadowOnly, ConstantValueSource{true});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *opacity,
                               ConstantValueSource{1.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *distance,
                               ConstantValueSource{-1.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *color,
                               ConstantValueSource{bloom::core::Color4d{0.0, 0.0, 0.0, 2.0}});

    // #528 Inner Shadow and Inner Glow: their colour, angle, distance, choke, switch and source edit
    // within their domains and refuse outside them; each edit is undoable.
    const auto inner = addRecipe("bloom.inner-shadow");
    const auto innerColor = inner.outputId<ParameterId>("parameter.color");
    const auto innerDirection = inner.outputId<ParameterId>("parameter.direction");
    const auto innerChoke = inner.outputId<ParameterId>("parameter.choke");
    const auto innerOnly = inner.outputId<ParameterId>("parameter.shadowOnly");
    const auto innerOpacity = inner.outputId<ParameterId>("parameter.opacity");
    const auto innerGlow = addRecipe("bloom.inner-glow");
    const auto innerSource = innerGlow.outputId<ParameterId>("parameter.source");
    const auto innerSize = innerGlow.outputId<ParameterId>("parameter.size");
    const auto innerGlowOnly = innerGlow.outputId<ParameterId>("parameter.glowOnly");
    const auto innerIntensity = innerGlow.outputId<ParameterId>("parameter.intensity");
    if (!innerColor || !innerDirection || !innerChoke || !innerOnly || !innerOpacity ||
        !innerSource || !innerSize || !innerGlowOnly || !innerIntensity)
        throw std::logic_error("Inner Shadow and Inner Glow expose registered control identities");
    (void)exercise<SetParameterSource>(
        test, fixture, *innerColor, ConstantValueSource{bloom::core::Color4d{0.1, 0.0, 3.0, 0.5}});
    (void)exercise<SetParameterSource>(test, fixture, *innerDirection, ConstantValueSource{-45.0});
    (void)exercise<SetParameterSource>(test, fixture, *innerChoke, ConstantValueSource{0.4});
    (void)exercise<SetParameterSource>(test, fixture, *innerOnly, ConstantValueSource{true});
    (void)exercise<SetParameterSource>(test, fixture, *innerSource,
                                       ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *innerSize, ConstantValueSource{32.0});
    (void)exercise<SetParameterSource>(test, fixture, *innerGlowOnly, ConstantValueSource{true});
    (void)exercise<SetParameterSource>(test, fixture, *innerIntensity, ConstantValueSource{12.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *innerChoke,
                               ConstantValueSource{1.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *innerOpacity,
                               ConstantValueSource{-0.1});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *innerSource,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *innerSource,
                               ConstantValueSource{true});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *innerSize,
                               ConstantValueSource{300.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *innerColor,
                               ConstantValueSource{bloom::core::Color4d{0.0, 0.0, 0.0, 2.0}});

    const auto glow = addRecipe("bloom.glow");
    const auto glowId = glow.outputId<NodeId>(kAddNodeOutput);
    const auto threshold = glow.outputId<ParameterId>("parameter.threshold");
    const auto octaves = glow.outputId<ParameterId>("parameter.octaves");
    const auto size = glow.outputId<ParameterId>("parameter.size");
    const auto mask =
        apply<AddNode>(fixture, "bloom.mask", Vec2d{60, 0}).outputId<NodeId>(kAddNodeOutput);
    if (!glowId || !threshold || !octaves || !size || !mask)
        throw std::logic_error("Glow exposes registered control identities");
    exercise<ConnectPorts>(test, fixture, OutputPortRef{addSource(fixture), "image"},
                           InputPortRef{NodeInputRef{*glowId, "key"}});
    exercise<ConnectPorts>(test, fixture, OutputPortRef{*mask, "mask"},
                           InputPortRef{NodeInputRef{*glowId, "mask"}});
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{*mask, "mask"}, InputPortRef{NodeInputRef{*glowId, "key"}});
    (void)exercise<SetParameterSource>(test, fixture, *threshold, ConstantValueSource{4.5});
    (void)exercise<SetParameterSource>(test, fixture, *octaves,
                                       ConstantValueSource{std::int64_t{8}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *octaves,
                               ConstantValueSource{std::int64_t{9}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *size,
                               ConstantValueSource{300.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *threshold,
                               ConstantValueSource{std::numeric_limits<double>::quiet_NaN()});
    // #506: the mode, ray count and shared controls edit and refuse within their domains, and
    // the lens-dirt input takes an Image but never a Mask.
    const auto mode = glow.outputId<ParameterId>("parameter.mode");
    const auto points = glow.outputId<ParameterId>("parameter.points");
    const auto radiusRed = glow.outputId<ParameterId>("parameter.radiusRed");
    const auto rollOff = glow.outputId<ParameterId>("parameter.rollOff");
    if (!mode || !points || !radiusRed || !rollOff)
        throw std::logic_error("Glow exposes its mode and shared control identities");
    (void)exercise<SetParameterSource>(test, fixture, *mode, ConstantValueSource{std::int64_t{2}});
    (void)exercise<SetParameterSource>(test, fixture, *points,
                                       ConstantValueSource{std::int64_t{16}});
    (void)exercise<SetParameterSource>(test, fixture, *rollOff, ConstantValueSource{2.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *mode,
                               ConstantValueSource{std::int64_t{3}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *points,
                               ConstantValueSource{std::int64_t{0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *radiusRed,
                               ConstantValueSource{1.5});
    exercise<ConnectPorts>(test, fixture, OutputPortRef{addSource(fixture), "image"},
                           InputPortRef{NodeInputRef{*glowId, "dirt"}});
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{*mask, "mask"}, InputPortRef{NodeInputRef{*glowId, "dirt"}});

    const auto wrap = addRecipe("bloom.light-wrap");
    const auto wrapId = wrap.outputId<NodeId>(kAddNodeOutput);
    const auto amount = wrap.outputId<ParameterId>("parameter.amount");
    if (!wrapId || !amount)
        throw std::logic_error("Light Wrap exposes registered control identities");
    exercise<ConnectPorts>(test, fixture, OutputPortRef{addSource(fixture), "image"},
                           InputPortRef{NodeInputRef{*wrapId, "background"}});
    (void)exercise<SetParameterSource>(test, fixture, *amount, ConstantValueSource{0.25});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *amount,
                               ConstantValueSource{1.25});

    // #503 Alpha Association: the mode, white switch and matte colour edit within their domains.
    const auto association = addRecipe("bloom.alpha-association");
    const auto associationMode = association.outputId<ParameterId>("parameter.mode");
    const auto white = association.outputId<ParameterId>("parameter.white");
    const auto background = association.outputId<ParameterId>("parameter.background");
    if (!associationMode || !white || !background)
        throw std::logic_error("Alpha Association exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *associationMode,
                                       ConstantValueSource{std::int64_t{3}});
    (void)exercise<SetParameterSource>(test, fixture, *white, ConstantValueSource{true});
    (void)exercise<SetParameterSource>(
        test, fixture, *background,
        ConstantValueSource{bloom::core::Color4d{2.5, -0.5, 0.25, 1.0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *associationMode,
                               ConstantValueSource{std::int64_t{4}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *white,
                               ConstantValueSource{std::int64_t{1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *background,
                               ConstantValueSource{bloom::core::Color4d{2e6, 0.0, 0.0, 1.0}});

    // #504 Colorize: the closed mode, a colour, a weight and the midpoint edit within their
    // domains and refuse outside them.
    const auto colorize = addRecipe("bloom.colorize");
    const auto colorizeMode = colorize.outputId<ParameterId>("parameter.mode");
    const auto highlights = colorize.outputId<ParameterId>("parameter.highlights");
    const auto reds = colorize.outputId<ParameterId>("parameter.reds");
    const auto midpoint = colorize.outputId<ParameterId>("parameter.midpoint");
    if (!colorizeMode || !highlights || !reds || !midpoint)
        throw std::logic_error("Colorize exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *colorizeMode,
                                       ConstantValueSource{std::int64_t{4}});
    (void)exercise<SetParameterSource>(
        test, fixture, *highlights,
        ConstantValueSource{bloom::core::Color4d{3.5, -0.5, 0.25, 1.0}});
    (void)exercise<SetParameterSource>(test, fixture, *reds, ConstantValueSource{-1.5});
    (void)exercise<SetParameterSource>(test, fixture, *midpoint, ConstantValueSource{0.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *colorizeMode,
                               ConstantValueSource{std::int64_t{5}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *highlights,
                               ConstantValueSource{bloom::core::Color4d{2e6, 0.0, 0.0, 1.0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *reds,
                               ConstantValueSource{3.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *midpoint,
                               ConstantValueSource{1.0});

    // #505 Vignette: the closed shape and mode, a strength, a share and the centre edit within
    // their domains and refuse outside them.
    const auto vignette = addRecipe("bloom.vignette");
    const auto shape = vignette.outputId<ParameterId>("parameter.shape");
    const auto exposure = vignette.outputId<ParameterId>("parameter.exposure");
    const auto feather = vignette.outputId<ParameterId>("parameter.feather");
    const auto center = vignette.outputId<ParameterId>("parameter.center");
    if (!shape || !exposure || !feather || !center)
        throw std::logic_error("Vignette exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *shape, ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *exposure, ConstantValueSource{-3.5});
    (void)exercise<SetParameterSource>(test, fixture, *feather, ConstantValueSource{0.0});
    (void)exercise<SetParameterSource>(
        test, fixture, *center, ConstantValueSource{bloom::document::Vec2d{-240.0, 135.5}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *shape,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *exposure,
                               ConstantValueSource{12.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *feather,
                               ConstantValueSource{1.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *center,
                               ConstantValueSource{bloom::document::Vec2d{2e6, 0.0}});

    // #507 Lens Flare: closed counts and choices, a share and the light edit within their domains
    // and refuse outside them.
    const auto flare = addRecipe("bloom.lens-flare");
    const auto ghosts = flare.outputId<ParameterId>("parameter.ghostCount");
    const auto chromatic = flare.outputId<ParameterId>("parameter.chromatic");
    const auto light = flare.outputId<ParameterId>("parameter.lightPosition");
    const auto lightMode = flare.outputId<ParameterId>("parameter.lightMode");
    if (!ghosts || !chromatic || !light || !lightMode)
        throw std::logic_error("Lens Flare exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *ghosts, ConstantValueSource{std::int64_t{16}});
    (void)exercise<SetParameterSource>(test, fixture, *chromatic, ConstantValueSource{0.5});
    (void)exercise<SetParameterSource>(
        test, fixture, *light, ConstantValueSource{bloom::document::Vec2d{-240.0, 135.5}});
    (void)exercise<SetParameterSource>(test, fixture, *lightMode, ConstantValueSource{std::int64_t{1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *ghosts,
                               ConstantValueSource{std::int64_t{17}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *chromatic,
                               ConstantValueSource{1.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *light,
                               ConstantValueSource{bloom::document::Vec2d{2e6, 0.0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *lightMode,
                               ConstantValueSource{std::int64_t{2}});

    // #486 Film Grain: its constant choice and seed and its values edit within their domains and
    // refuse outside them.
    const auto grain = addRecipe("bloom.film-grain");
    const auto grainSize = grain.outputId<ParameterId>("parameter.size");
    const auto grainAnimation = grain.outputId<ParameterId>("parameter.animation");
    const auto grainSeed = grain.outputId<ParameterId>("parameter.seed");
    if (!grainSize || !grainAnimation || !grainSeed)
        throw std::logic_error("Film Grain exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *grainSize, ConstantValueSource{4.0});
    (void)exercise<SetParameterSource>(test, fixture, *grainAnimation,
                                       ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *grainSeed,
                                       ConstantValueSource{std::int64_t{42}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *grainSize,
                               ConstantValueSource{0.1});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *grainAnimation,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *grainSeed,
                               ConstantValueSource{std::int64_t{-1}});
    // #565: the mode and the Sensor Noise values edit within their domains and refuse outside them.
    const auto grainMode = grain.outputId<ParameterId>("parameter.mode");
    const auto grainBlotchSize = grain.outputId<ParameterId>("parameter.blotchSize");
    if (!grainMode || !grainBlotchSize)
        throw std::logic_error("Film Grain exposes its Sensor Noise control identities");
    (void)exercise<SetParameterSource>(test, fixture, *grainMode,
                                       ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *grainBlotchSize, ConstantValueSource{48.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *grainMode,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *grainBlotchSize,
                               ConstantValueSource{0.5});

    // #563 Codec Degrade: its two values and three constants edit within their domains and refuse
    // outside them.
    const auto codec = addRecipe("bloom.codec-degrade");
    const auto codecQuality = codec.outputId<ParameterId>("parameter.quality");
    const auto codecPersistence = codec.outputId<ParameterId>("parameter.persistence");
    const auto codecBlock = codec.outputId<ParameterId>("parameter.blockSize");
    const auto codecGop = codec.outputId<ParameterId>("parameter.gopLength");
    const auto codecChroma = codec.outputId<ParameterId>("parameter.chromaBleed");
    if (!codecQuality || !codecPersistence || !codecBlock || !codecGop || !codecChroma)
        throw std::logic_error("Codec Degrade exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *codecQuality, ConstantValueSource{75.0});
    (void)exercise<SetParameterSource>(test, fixture, *codecPersistence, ConstantValueSource{0.9});
    (void)exercise<SetParameterSource>(test, fixture, *codecBlock,
                                       ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *codecGop,
                                       ConstantValueSource{std::int64_t{8}});
    (void)exercise<SetParameterSource>(test, fixture, *codecChroma, ConstantValueSource{false});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *codecQuality,
                               ConstantValueSource{101.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *codecPersistence,
                               ConstantValueSource{1.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *codecBlock,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *codecGop,
                               ConstantValueSource{std::int64_t{9}});

    // #486 Lens Distortion: its mode, four coefficients and centre edit within their domains and
    // refuse outside them.
    const auto lens = addRecipe("bloom.lens-distortion");
    const auto lensMode = lens.outputId<ParameterId>("parameter.mode");
    const auto lensK1 = lens.outputId<ParameterId>("parameter.k1");
    const auto lensK2 = lens.outputId<ParameterId>("parameter.k2");
    const auto lensP1 = lens.outputId<ParameterId>("parameter.p1");
    const auto lensP2 = lens.outputId<ParameterId>("parameter.p2");
    const auto lensCenter = lens.outputId<ParameterId>("parameter.center");
    if (!lensMode || !lensK1 || !lensK2 || !lensP1 || !lensP2 || !lensCenter)
        throw std::logic_error("Lens Distortion exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *lensMode,
                                       ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *lensK1, ConstantValueSource{-0.25});
    (void)exercise<SetParameterSource>(test, fixture, *lensK2, ConstantValueSource{0.05});
    (void)exercise<SetParameterSource>(test, fixture, *lensP1, ConstantValueSource{0.01});
    (void)exercise<SetParameterSource>(test, fixture, *lensP2, ConstantValueSource{-0.01});
    (void)exercise<SetParameterSource>(test, fixture, *lensCenter,
                                       ConstantValueSource{Vec2d{12.0, -8.0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *lensMode,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *lensK1,
                               ConstantValueSource{2.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *lensK2,
                               ConstantValueSource{-2.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *lensP1,
                               ConstantValueSource{0.6});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *lensP2,
                               ConstantValueSource{-0.6});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *lensCenter,
                               ConstantValueSource{Vec2d{2.0e6, 0.0}});

    // #486 Chromatic Aberration: its amount and centre edit within their domains and refuse
    // outside them.
    const auto aberration = addRecipe("bloom.chromatic-aberration");
    const auto aberrationAmount = aberration.outputId<ParameterId>("parameter.amount");
    const auto aberrationCenter = aberration.outputId<ParameterId>("parameter.center");
    if (!aberrationAmount || !aberrationCenter)
        throw std::logic_error("Chromatic Aberration exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *aberrationAmount, ConstantValueSource{-0.1});
    (void)exercise<SetParameterSource>(test, fixture, *aberrationCenter,
                                       ConstantValueSource{Vec2d{12.0, -8.0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aberrationAmount,
                               ConstantValueSource{0.3});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aberrationAmount,
                               ConstantValueSource{-0.3});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aberrationCenter,
                               ConstantValueSource{Vec2d{2.0e6, 0.0}});

    // #566 Auto Exposure: its values and the look-back edit within their domains and refuse outside
    // them.
    const auto autoExposure = addRecipe("bloom.auto-exposure");
    const auto aeTarget = autoExposure.outputId<ParameterId>("parameter.target");
    const auto aeAttack = autoExposure.outputId<ParameterId>("parameter.attack");
    const auto aeMinGain = autoExposure.outputId<ParameterId>("parameter.minGain");
    const auto aeMaxGain = autoExposure.outputId<ParameterId>("parameter.maxGain");
    const auto aeLookBack = autoExposure.outputId<ParameterId>("parameter.lookBack");
    if (!aeTarget || !aeAttack || !aeMinGain || !aeMaxGain || !aeLookBack)
        throw std::logic_error("Auto Exposure exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *aeTarget, ConstantValueSource{0.25});
    (void)exercise<SetParameterSource>(test, fixture, *aeAttack, ConstantValueSource{0.0});
    (void)exercise<SetParameterSource>(test, fixture, *aeMinGain, ConstantValueSource{-6.0});
    (void)exercise<SetParameterSource>(test, fixture, *aeMaxGain, ConstantValueSource{6.0});
    (void)exercise<SetParameterSource>(test, fixture, *aeLookBack,
                                       ConstantValueSource{std::int64_t{64}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aeTarget,
                               ConstantValueSource{0.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aeAttack,
                               ConstantValueSource{-1.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aeMinGain,
                               ConstantValueSource{1.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aeMaxGain,
                               ConstantValueSource{-1.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aeLookBack,
                               ConstantValueSource{std::int64_t{65}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *aeLookBack,
                               ConstantValueSource{std::int64_t{3}});

    // #518 Tone Map: its values, method and gamut switch edit within their domains and refuse
    // outside them.
    const auto toneMap = addRecipe("bloom.tone-map");
    const auto tmMethod = toneMap.outputId<ParameterId>("parameter.method");
    const auto tmKnee = toneMap.outputId<ParameterId>("parameter.knee");
    const auto tmShadows = toneMap.outputId<ParameterId>("parameter.shadows");
    const auto tmRange = toneMap.outputId<ParameterId>("parameter.shadowRange");
    const auto tmGamut = toneMap.outputId<ParameterId>("parameter.gamutCompress");
    const auto tmLimit = toneMap.outputId<ParameterId>("parameter.gamutLimit");
    if (!tmMethod || !tmKnee || !tmShadows || !tmRange || !tmGamut || !tmLimit)
        throw std::logic_error("Tone Map exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *tmMethod, ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *tmKnee, ConstantValueSource{4.0});
    (void)exercise<SetParameterSource>(test, fixture, *tmShadows, ConstantValueSource{-1.0});
    (void)exercise<SetParameterSource>(test, fixture, *tmGamut, ConstantValueSource{true});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tmMethod,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tmKnee,
                               ConstantValueSource{-0.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tmShadows,
                               ConstantValueSource{-1.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tmRange,
                               ConstantValueSource{0.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tmLimit,
                               ConstantValueSource{1.0});

    // #517 Color Matrix: its elements and choices edit within their domains and refuse outside them.
    const auto colorMatrix = addRecipe("bloom.color-matrix");
    const auto cmPreset = colorMatrix.outputId<ParameterId>("parameter.preset");
    const auto cmModel = colorMatrix.outputId<ParameterId>("parameter.outputModel");
    const auto cmElement = colorMatrix.outputId<ParameterId>("parameter.rg");
    if (!cmPreset || !cmModel || !cmElement)
        throw std::logic_error("Color Matrix exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *cmPreset, ConstantValueSource{std::int64_t{6}});
    (void)exercise<SetParameterSource>(test, fixture, *cmModel, ConstantValueSource{std::int64_t{4}});
    (void)exercise<SetParameterSource>(test, fixture, *cmElement, ConstantValueSource{-2.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *cmPreset,
                               ConstantValueSource{std::int64_t{7}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *cmModel,
                               ConstantValueSource{std::int64_t{5}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *cmElement,
                               ConstantValueSource{101.0});

    // #519 Quantize: its level, softness, levels and switches edit within their domains and refuse
    // outside them (a threshold level 0 to 16, levels 2 to 256, a mode of two).
    const auto quantize = addRecipe("bloom.quantize");
    const auto qMode = quantize.outputId<ParameterId>("parameter.mode");
    const auto qBasis = quantize.outputId<ParameterId>("parameter.basis");
    const auto qLevel = quantize.outputId<ParameterId>("parameter.level");
    const auto qSoftness = quantize.outputId<ParameterId>("parameter.softness");
    const auto qInvert = quantize.outputId<ParameterId>("parameter.invert");
    const auto qLevels = quantize.outputId<ParameterId>("parameter.levels");
    const auto qLink = quantize.outputId<ParameterId>("parameter.linkLevels");
    const auto qRed = quantize.outputId<ParameterId>("parameter.redLevels");
    const auto qAlpha = quantize.outputId<ParameterId>("parameter.posterizeAlpha");
    if (!qMode || !qBasis || !qLevel || !qSoftness || !qInvert || !qLevels || !qLink || !qRed || !qAlpha)
        throw std::logic_error("Quantize exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *qMode, ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *qBasis, ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *qLevel, ConstantValueSource{0.75});
    (void)exercise<SetParameterSource>(test, fixture, *qSoftness, ConstantValueSource{0.25});
    (void)exercise<SetParameterSource>(test, fixture, *qInvert, ConstantValueSource{true});
    (void)exercise<SetParameterSource>(test, fixture, *qLevels, ConstantValueSource{256.0});
    (void)exercise<SetParameterSource>(test, fixture, *qLink, ConstantValueSource{false});
    (void)exercise<SetParameterSource>(test, fixture, *qRed, ConstantValueSource{2.0});
    (void)exercise<SetParameterSource>(test, fixture, *qAlpha, ConstantValueSource{true});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *qMode,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *qBasis,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *qLevel,
                               ConstantValueSource{-0.1});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *qSoftness,
                               ConstantValueSource{16.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *qLevels,
                               ConstantValueSource{1.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *qLevels,
                               ConstantValueSource{257.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *qRed,
                               ConstantValueSource{0.0});

    // #527 Dither: its amplitude, choices, seed and Monochrome edit within their domains and refuse
    // outside them (an amplitude 0 to 16 LSBs, two patterns, four matrix sizes and ten bit depths).
    const auto dither = addRecipe("bloom.dither");
    const auto dPattern = dither.outputId<ParameterId>("parameter.pattern");
    const auto dSize = dither.outputId<ParameterId>("parameter.matrixSize");
    const auto dDepth = dither.outputId<ParameterId>("parameter.bitDepth");
    const auto dAmplitude = dither.outputId<ParameterId>("parameter.amplitude");
    const auto dDomain = dither.outputId<ParameterId>("parameter.domain");
    const auto dAnimation = dither.outputId<ParameterId>("parameter.animation");
    const auto dSeed = dither.outputId<ParameterId>("parameter.seed");
    const auto dMono = dither.outputId<ParameterId>("parameter.monochrome");
    if (!dPattern || !dSize || !dDepth || !dAmplitude || !dDomain || !dAnimation || !dSeed || !dMono)
        throw std::logic_error("Dither exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *dPattern, ConstantValueSource{std::int64_t{0}});
    (void)exercise<SetParameterSource>(test, fixture, *dSize, ConstantValueSource{std::int64_t{3}});
    (void)exercise<SetParameterSource>(test, fixture, *dDepth, ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *dAmplitude, ConstantValueSource{2.5});
    (void)exercise<SetParameterSource>(test, fixture, *dDomain, ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *dAnimation, ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *dSeed, ConstantValueSource{std::int64_t{123456}});
    (void)exercise<SetParameterSource>(test, fixture, *dMono, ConstantValueSource{true});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *dPattern,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *dSize,
                               ConstantValueSource{std::int64_t{4}});
    (void)exercise<SetParameterSource>(test, fixture, *dDepth, ConstantValueSource{std::int64_t{9}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *dDepth,
                               ConstantValueSource{std::int64_t{10}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *dAmplitude,
                               ConstantValueSource{-0.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *dAmplitude,
                               ConstantValueSource{16.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *dSeed,
                               ConstantValueSource{std::int64_t{-1}});

    // #348 Corner Pin, ST Map and Displace: their corners, amounts and choices edit within their
    // domains and refuse outside them.
    const auto pin = addRecipe("bloom.corner-pin");
    const auto pinCorner = pin.outputId<ParameterId>("parameter.topRight");
    const auto pinFilter = pin.outputId<ParameterId>("parameter.filter");
    const auto displace = addRecipe("bloom.displace");
    const auto displaceAmount = displace.outputId<ParameterId>("parameter.xAmount");
    const auto displaceChannel = displace.outputId<ParameterId>("parameter.yChannel");
    const auto stMap = addRecipe("bloom.st-map");
    const auto stMapEdges = stMap.outputId<ParameterId>("parameter.edges");
    if (!pinCorner || !pinFilter || !displaceAmount || !displaceChannel || !stMapEdges)
        throw std::logic_error("the warp nodes expose registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *pinCorner,
                                       ConstantValueSource{Vec2d{-120.0, 45.5}});
    (void)exercise<SetParameterSource>(test, fixture, *pinFilter, ConstantValueSource{std::int64_t{1}});
    (void)exercise<SetParameterSource>(test, fixture, *displaceAmount, ConstantValueSource{-75.0});
    (void)exercise<SetParameterSource>(test, fixture, *displaceChannel,
                                       ConstantValueSource{std::int64_t{4}});
    (void)exercise<SetParameterSource>(test, fixture, *stMapEdges, ConstantValueSource{std::int64_t{3}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *pinCorner,
                               ConstantValueSource{Vec2d{2.0e6, 0.0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *pinFilter,
                               ConstantValueSource{std::int64_t{3}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *displaceAmount,
                               ConstantValueSource{2.0e5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *displaceChannel,
                               ConstantValueSource{std::int64_t{5}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *stMapEdges,
                               ConstantValueSource{std::int64_t{4}});

    // #511 Tile & Mirror: its sizes, angles, points, switches, whole number and choices edit within
    // their domains and refuse outside them (a switch is a bool, the segment count 1 to 64, a size above
    // zero); each edit is undoable.
    const auto tile = addRecipe("bloom.tile-mirror");
    const auto tileWidth = tile.outputId<ParameterId>("parameter.tileWidth");
    const auto tileMode = tile.outputId<ParameterId>("parameter.mode");
    const auto tileLink = tile.outputId<ParameterId>("parameter.linkTileSize");
    const auto tileSegments = tile.outputId<ParameterId>("parameter.kaleidoscopeSegments");
    const auto tileAngle = tile.outputId<ParameterId>("parameter.mirrorAngle");
    const auto tileCentre = tile.outputId<ParameterId>("parameter.tileCenter");
    const auto tileSide = tile.outputId<ParameterId>("parameter.mirrorSide");
    if (!tileWidth || !tileMode || !tileLink || !tileSegments || !tileAngle || !tileCentre || !tileSide)
        throw std::logic_error("Tile & Mirror exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *tileWidth, ConstantValueSource{35.5});
    (void)exercise<SetParameterSource>(test, fixture, *tileMode, ConstantValueSource{std::int64_t{3}});
    (void)exercise<SetParameterSource>(test, fixture, *tileLink, ConstantValueSource{true});
    (void)exercise<SetParameterSource>(test, fixture, *tileSegments,
                                       ConstantValueSource{std::int64_t{12}});
    (void)exercise<SetParameterSource>(test, fixture, *tileAngle, ConstantValueSource{-45.0});
    (void)exercise<SetParameterSource>(test, fixture, *tileCentre,
                                       ConstantValueSource{Vec2d{-120.0, 45.5}});
    (void)exercise<SetParameterSource>(test, fixture, *tileSide, ConstantValueSource{std::int64_t{1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tileWidth,
                               ConstantValueSource{0.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tileMode,
                               ConstantValueSource{std::int64_t{4}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tileLink,
                               ConstantValueSource{std::int64_t{1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tileSegments,
                               ConstantValueSource{std::int64_t{0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tileSegments,
                               ConstantValueSource{std::int64_t{65}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tileSide,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *tileCentre,
                               ConstantValueSource{Vec2d{2.0e6, 0.0}});

    // #510 Procedural Warp: its mode, lengths, percentages, angles, choices and centre edit within
    // their domains and refuse outside them (a mode past Magnify, a negative radius, an amount past
    // 100%, a refraction index below 1, a wavelength below 1 pixel, a magnification below 1%, a far
    // centre); each edit is undoable.
    const auto warp = addRecipe("bloom.procedural-warp");
    const auto warpMode = warp.outputId<ParameterId>("parameter.mode");
    const auto warpRadius = warp.outputId<ParameterId>("parameter.radius");
    const auto warpAmount = warp.outputId<ParameterId>("parameter.amount");
    const auto warpRefraction = warp.outputId<ParameterId>("parameter.refraction");
    const auto warpWavelength = warp.outputId<ParameterId>("parameter.wavelength");
    const auto warpAngle = warp.outputId<ParameterId>("parameter.angle");
    const auto warpPinning = warp.outputId<ParameterId>("parameter.pinning");
    const auto warpMagnification = warp.outputId<ParameterId>("parameter.magnification");
    const auto warpLink = warp.outputId<ParameterId>("parameter.magnifyLink");
    const auto warpCentre = warp.outputId<ParameterId>("parameter.center");
    if (!warpMode || !warpRadius || !warpAmount || !warpRefraction || !warpWavelength ||
        !warpAngle || !warpPinning || !warpMagnification || !warpLink || !warpCentre)
        throw std::logic_error("Procedural Warp exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *warpMode, ConstantValueSource{std::int64_t{6}});
    (void)exercise<SetParameterSource>(test, fixture, *warpRadius, ConstantValueSource{125.5});
    (void)exercise<SetParameterSource>(test, fixture, *warpAmount, ConstantValueSource{-100.0});
    (void)exercise<SetParameterSource>(test, fixture, *warpRefraction, ConstantValueSource{2.25});
    (void)exercise<SetParameterSource>(test, fixture, *warpWavelength, ConstantValueSource{12.0});
    (void)exercise<SetParameterSource>(test, fixture, *warpAngle, ConstantValueSource{-1080.0});
    (void)exercise<SetParameterSource>(test, fixture, *warpPinning,
                                       ConstantValueSource{std::int64_t{7}});
    (void)exercise<SetParameterSource>(test, fixture, *warpMagnification, ConstantValueSource{450.0});
    (void)exercise<SetParameterSource>(test, fixture, *warpLink, ConstantValueSource{std::int64_t{2}});
    (void)exercise<SetParameterSource>(test, fixture, *warpCentre,
                                       ConstantValueSource{Vec2d{-240.0, 80.5}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpMode,
                               ConstantValueSource{std::int64_t{12}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpRadius,
                               ConstantValueSource{-1.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpAmount,
                               ConstantValueSource{100.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpRefraction,
                               ConstantValueSource{0.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpWavelength,
                               ConstantValueSource{0.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpPinning,
                               ConstantValueSource{std::int64_t{8}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpMagnification,
                               ConstantValueSource{0.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpLink,
                               ConstantValueSource{std::int64_t{3}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpCentre,
                               ConstantValueSource{Vec2d{2.0e6, 0.0}});

    // #510 the modes after Magnify: Drizzle is the last mode; Warp's style and bend, Split's
    // distance, mirror and line, Turbulent's displacement, noise size and seed, and Drizzle's drops
    // edit within their domains and refuse outside them.
    const auto warpStyle = warp.outputId<ParameterId>("parameter.warpStyle");
    const auto warpBend = warp.outputId<ParameterId>("parameter.bend");
    const auto warpSplit = warp.outputId<ParameterId>("parameter.splitDistance");
    const auto warpMirror = warp.outputId<ParameterId>("parameter.mirror");
    const auto warpAsymmetric = warp.outputId<ParameterId>("parameter.asymmetric");
    const auto warpOpposite = warp.outputId<ParameterId>("parameter.oppositeDistance");
    const auto warpLineEnd = warp.outputId<ParameterId>("parameter.lineEnd");
    const auto warpPullPoint = warp.outputId<ParameterId>("parameter.pullPoint");
    const auto warpNoiseSize = warp.outputId<ParameterId>("parameter.noiseSize");
    const auto warpSeed = warp.outputId<ParameterId>("parameter.seed");
    const auto warpDrops = warp.outputId<ParameterId>("parameter.drops");
    const auto warpPulse = warp.outputId<ParameterId>("parameter.pulseRadius");
    if (!warpStyle || !warpBend || !warpSplit || !warpMirror || !warpLineEnd || !warpPullPoint ||
        !warpNoiseSize || !warpSeed || !warpDrops || !warpPulse || !warpAsymmetric || !warpOpposite)
        throw std::logic_error("Procedural Warp exposes the identities of its later modes");
    (void)exercise<SetParameterSource>(test, fixture, *warpMode, ConstantValueSource{std::int64_t{11}});
    (void)exercise<SetParameterSource>(test, fixture, *warpStyle, ConstantValueSource{std::int64_t{12}});
    (void)exercise<SetParameterSource>(test, fixture, *warpBend, ConstantValueSource{-100.0});
    (void)exercise<SetParameterSource>(test, fixture, *warpSplit, ConstantValueSource{260.0});
    (void)exercise<SetParameterSource>(test, fixture, *warpMirror, ConstantValueSource{true});
    (void)exercise<SetParameterSource>(test, fixture, *warpAsymmetric, ConstantValueSource{true});
    (void)exercise<SetParameterSource>(test, fixture, *warpOpposite, ConstantValueSource{260.0});
    (void)exercise<SetParameterSource>(test, fixture, *warpLineEnd,
                                       ConstantValueSource{Vec2d{120.0, -60.0}});
    (void)exercise<SetParameterSource>(test, fixture, *warpPullPoint,
                                       ConstantValueSource{Vec2d{-120.0, 60.0}});
    (void)exercise<SetParameterSource>(test, fixture, *warpNoiseSize,
                                       ConstantValueSource{Vec2d{40.0, 220.0}});
    (void)exercise<SetParameterSource>(test, fixture, *warpSeed,
                                       ConstantValueSource{std::int64_t{16777215}});
    (void)exercise<SetParameterSource>(test, fixture, *warpDrops, ConstantValueSource{std::int64_t{64}});
    (void)exercise<SetParameterSource>(test, fixture, *warpPulse, ConstantValueSource{180.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpStyle,
                               ConstantValueSource{std::int64_t{13}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpBend,
                               ConstantValueSource{100.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpSplit,
                               ConstantValueSource{-1.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpOpposite,
                               ConstantValueSource{-1.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpNoiseSize,
                               ConstantValueSource{Vec2d{0.5, 100.0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpSeed,
                               ConstantValueSource{std::int64_t{16777216}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpDrops,
                               ConstantValueSource{std::int64_t{65}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpDrops,
                               ConstantValueSource{std::int64_t{0}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *warpPulse,
                               ConstantValueSource{-1.0});

    // #513 Stabilize: its choices and points edit within their domains and refuse outside them, and
    // a finished track is stored as one undo step: a Table block of the positions and the keys.
    const auto stabilize = addRecipe("bloom.stabilize");
    const auto stabilizeMode = stabilize.outputId<ParameterId>("parameter.mode");
    const auto stabilizePosition = stabilize.outputId<ParameterId>("parameter.tracked");
    const auto stabilizeNode = stabilize.outputId<NodeId>(kAddNodeOutput);
    if (!stabilizeMode || !stabilizePosition || !stabilizeNode)
        throw std::logic_error("Stabilize exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *stabilizeMode, ConstantValueSource{std::int64_t{1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *stabilizeMode,
                               ConstantValueSource{std::int64_t{2}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *stabilizePosition,
                               ConstantValueSource{Vec2d{0.0, 2.0e6}});
    {
        const std::vector<bloom::commands::TrackRow> rows{
            {0, 10.0, -4.0, 1.0}, {1, 12.5, -3.0, 0.97}, {2, 15.0, -2.25, 0.95}};
        bloom::commands::TrackStorage storage;
        storage.composition = kCompositionId;
        storage.node = *stabilizeNode;
        storage.position = *stabilizePosition;
        storage.settings = "pattern=16;search=32";
        storage.createdAt = "2026-11-09T00:00:00Z";
        const auto before = fixture.document.snapshot();
        const auto historySize = fixture.stack.size();
        auto transaction = bloom::commands::makeStoreTrackTransaction(storage, rows);
        const auto result = fixture.stack.execute(std::move(transaction));
        test.expect(result.status == CommandStatus::Succeeded && fixture.stack.size() == historySize + 1,
                    "storing a track is one history entry");
        const auto& project = fixture.document.snapshot().project();
        const auto blocks = project.dataBlocks();
        const document::DataBlockRecord* track = nullptr;
        for (const auto& block : blocks)
            if (block.typeId == bloom::commands::kTrackBlockTypeId)
                track = &block;
        const auto* table = track == nullptr ? nullptr : std::get_if<document::DataBlockTable>(&track->payload);
        test.expect(table != nullptr, "the payload is a table");
        test.expect(table != nullptr && table->columns.size() == 4 && table->columns[1].values.size() == 3,
                    "four columns of three rows");
        test.expect(track != nullptr && track->subject.has_value() &&
                        std::holds_alternative<NodeId>(*track->subject) &&
                        std::get<NodeId>(*track->subject) == *stabilizeNode,
                    "the block is about the node");
        test.expect(table != nullptr && table->columns.size() == 4 && table->columns[1].values.size() == 3 &&
                        std::get<double>(table->columns[2].values[1]) == 12.5 &&
                        std::get<std::int64_t>(table->columns[0].values[2]) == 2 &&
                        track->subject == std::optional<document::ExtensionTarget>{*stabilizeNode},
                    "the track is a Table of frame, score, x and y about the node");
        const auto* composition = fixture.document.snapshot().project().findComposition(kCompositionId);
        const auto* position = composition == nullptr ? nullptr : composition->parameters().find(*stabilizePosition);
        test.expect(position != nullptr && std::holds_alternative<document::AnimationCurveSource>(position->source),
                    "the position is keyed with the track");
        test.expect(fixture.stack.undo().changed() && sameTruth(fixture.document.snapshot(), before) &&
                        fixture.document.snapshot().project().dataBlocks().empty(),
                    "undoing the track removes the block and the keys together");
        test.expect(fixture.stack.redo().changed(), "the track redoes");
    }

    // #486 Sharpen: its values edit within their domains and refuse outside them.
    const auto sharpen = addRecipe("bloom.sharpen");
    const auto sharpenAmount = sharpen.outputId<ParameterId>("parameter.amount");
    const auto sharpenRadius = sharpen.outputId<ParameterId>("parameter.radius");
    const auto sharpenThreshold = sharpen.outputId<ParameterId>("parameter.threshold");
    if (!sharpenAmount || !sharpenRadius || !sharpenThreshold)
        throw std::logic_error("Sharpen exposes registered control identities");
    (void)exercise<SetParameterSource>(test, fixture, *sharpenAmount, ConstantValueSource{2.5});
    (void)exercise<SetParameterSource>(test, fixture, *sharpenRadius, ConstantValueSource{12.0});
    (void)exercise<SetParameterSource>(test, fixture, *sharpenThreshold, ConstantValueSource{0.04});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *sharpenAmount,
                               ConstantValueSource{-0.5});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *sharpenRadius,
                               ConstantValueSource{257.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *sharpenThreshold,
                               ConstantValueSource{-0.1});

    // #512 Camera Shake: each new node gets a seed of its own (two default nodes never shake
    // alike), stored as an ordinary constant that edits within its domain; its Seed edits and
    // refuses outside it, and its outputs offer the shake to other parameters.
    const auto shakeA = addRecipe("bloom.camera-shake");
    const auto shakeB = addRecipe("bloom.camera-shake");
    const auto seedA = shakeA.outputId<ParameterId>("parameter.seed");
    const auto seedB = shakeB.outputId<ParameterId>("parameter.seed");
    const auto shakeAmplitude = shakeA.outputId<ParameterId>("parameter.amplitudeX");
    if (!seedA || !seedB || !shakeAmplitude)
        throw std::logic_error("Camera Shake exposes registered control identities");
    const auto seedOf = [&](const ParameterId id) {
        const auto* record = composition(fixture.document.snapshot()).parameters().find(id);
        const auto* constant =
            record ? std::get_if<ConstantValueSource>(&record->source) : nullptr;
        const auto* seed = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
        if (!seed)
            throw std::logic_error("Camera Shake seed is an integer constant");
        return *seed;
    };
    test.expect(seedOf(*seedA) != seedOf(*seedB), "two new Camera Shake nodes have different seeds");
    test.expect(seedOf(*seedA) >= 0 && seedOf(*seedB) >= 0, "a new Camera Shake seed is in range");
    (void)exercise<SetParameterSource>(test, fixture, *seedA,
                                       ConstantValueSource{std::int64_t{987654}});
    test.expect(seedOf(*seedA) == 987654, "a Camera Shake seed can be set");
    (void)exercise<SetParameterSource>(test, fixture, *shakeAmplitude, ConstantValueSource{24.0});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *seedA,
                               ConstantValueSource{std::int64_t{-1}});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, *shakeAmplitude,
                               ConstantValueSource{-1.0});
}

void testAddAndLayout(TestContext& test) {
    // Every built-in definition below is added through one history entry each, so the fixture's
    // history holds more entries than the registry has definitions (the default cap is 256).
    Fixture fixture{CommandStack::HistoryLimits{4096, CommandStack::HistoryLimits{}.maxChargedBytes}};
    // #344 #530 #509 #349: the path processing and analysis nodes are among the definitions this
    // loop adds, lays out and checks.
    for (const auto* type :
         {"bloom.transform-path", "bloom.trim-path", "bloom.offset-path", "bloom.round-corners",
          "bloom.boolean-paths", "bloom.merge-paths", "bloom.path-stroke", "bloom.histogram",
          "bloom.image-compare", "bloom.image-statistics"})
        test.expect(std::ranges::any_of(builtInNodeDefinitions().definitions(),
                                        [&](const auto& definition) {
                                            return definition.key.typeId == type;
                                        }),
                    "the generic AddNode loop covers the path and analysis nodes");
    for (const auto& definition : builtInNodeDefinitions().definitions()) {
        // Task S1, item 5: a OnePerComposition type is refused once the composition already holds
        // one. The fixture's composition carries a Layer Stack from the start, so that one refuses
        // immediately; the composition output is the same rule proved from the other side -- the
        // first is accepted below and a second is refused right after it.
        if (definition.cardinality == NodeCardinality::OnePerComposition &&
            std::ranges::any_of(
                composition(fixture.document.snapshot()).graph().nodes(),
                [&](const auto& existing) { return existing.typeId == definition.key.typeId; })) {
            refuse<AddNode>(test, fixture, OperationIssueCode::Unsupported, definition.key.typeId,
                            Vec2d{17, -31});
            continue;
        }
        const auto beforeSlots =
            composition(fixture.document.snapshot()).graph().layerStack().entries().size();
        const auto result = exercise<AddNode>(test, fixture, definition.key.typeId, Vec2d{17, -31});
        const auto id = result.outputId<NodeId>(kAddNodeOutput);
        if (!(id.has_value()))
            throw std::logic_error("node ID returned");
        const auto& comp = composition(fixture.document.snapshot());
        const auto* node = comp.graph().findNode(*id);
        test.expect(
            // ADR 0024: AddNode leaves the optional Time Remap unbound (disabled).
            node &&
                node->parameters.size() ==
                    definition.parameters.size() -
                        static_cast<std::size_t>(std::ranges::count(
                            definition.parameters, document::kTimeRemapParameterSchemaKey,
                            &document::ParameterDefinition::schemaKey)) &&
                node->schemaVersion == definition.key.schemaVersion &&
                comp.nodeLayout().contains(*id) &&
                comp.graph().layerStack().entries().size() == beforeSlots,
            "generic AddNode uses schema defaults, a free requested slot, and no layer or slot");
        for (const auto& parameter : definition.parameters) {
            if (parameter.schemaKey == document::kTimeRemapParameterSchemaKey) {
                test.expect(!result.outputId<ParameterId>("parameter." + parameter.role),
                            "AddNode leaves Time Remap unbound");
                continue;
            }
            const auto parameterId = result.outputId<ParameterId>("parameter." + parameter.role);
            const auto* record = parameterId ? comp.parameters().find(*parameterId) : nullptr;
            auto expected = parameter.defaultValue;
            if (parameter.schemaKey == document::kSolidWidthParameterSchemaKey)
                expected = static_cast<double>(comp.format().width());
            if (parameter.schemaKey == document::kSolidHeightParameterSchemaKey)
                expected = static_cast<double>(comp.format().height());
            if (parameter.schemaKey == document::kGradientStartParameterSchemaKey &&
                definition.key.typeId == document::kRadialGradientNodeType)
                expected = document::Vec2d{static_cast<double>(comp.format().width()) / 2.0,
                                           static_cast<double>(comp.format().height()) / 2.0};
            if (parameter.schemaKey == document::kGradientEndParameterSchemaKey)
                expected =
                    document::Vec2d{static_cast<double>(comp.format().width()),
                                    definition.key.typeId == document::kRadialGradientNodeType
                                        ? static_cast<double>(comp.format().height()) / 2.0
                                        : 0.0};
            if (parameter.schemaKey == document::kPositionParameterSchemaKey)
                expected = document::Vec2d{static_cast<double>(comp.format().width()) / 2.0,
                                           static_cast<double>(comp.format().height()) / 2.0};
            // #524: a new Four-Color Gradient anchors its points at the composition's corners (field
            // units: pixels from the centre, y up), whatever the composition's size.
            if (document::isFourColorGradientPointSchemaKey(parameter.schemaKey)) {
                const double halfWidth = static_cast<double>(comp.format().width()) / 2.0;
                const double halfHeight = static_cast<double>(comp.format().height()) / 2.0;
                const bool right = parameter.role == "point2" || parameter.role == "point4";
                const bool top = parameter.role == "point1" || parameter.role == "point2";
                expected = document::Vec2d{right ? halfWidth : -halfWidth,
                                           top ? halfHeight : -halfHeight};
            }
            // #512: a new Camera Shake (and #564 Frame Rate Hold) gets a seed of its own, an ordinary stored
            // constant.
            if ((parameter.schemaKey == "bloom.camera-shake.seed" ||
                 parameter.schemaKey == "bloom.frame-rate-hold.seed" ||
                 ((definition.key.typeId == "bloom.light-flicker" ||
                  definition.key.typeId == "bloom.wiggle") &&
                 parameter.role == "seed")) &&
                record) {
                const auto* constant = std::get_if<ConstantValueSource>(&record->source);
                const auto* seed =
                    constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
                if (seed && *seed >= 0 && *seed < 1'000'000)
                    expected = *seed;
            }
            test.expect(record && record->schemaKey == parameter.schemaKey &&
                            record->source == ParameterSource{ConstantValueSource{expected}},
                        "composition-aware default is copied into an independent parameter: " +
                            definition.key.typeId + "." + parameter.role);
        }
        if (definition.cardinality == NodeCardinality::OnePerComposition) {
            refuse<AddNode>(test, fixture, OperationIssueCode::Unsupported, definition.key.typeId,
                            Vec2d{17, -31});
        }
    }
    // The rule lives on the definition, so it is the definition that says which types are
    // singletons
    // -- the Layer Stack operator and the composition's one evaluation endpoint, and nothing else.
    for (const auto& definition : builtInNodeDefinitions().definitions()) {
        const bool singleton = definition.key.typeId == kCompositionOutputNodeType ||
                               definition.key.typeId == kGroupInputNodeType ||
                               definition.key.typeId == kGroupOutputNodeType;
        test.expect((definition.cardinality == NodeCardinality::OnePerComposition) == singleton,
                    "only the composition output and the group boundary are one per composition: " +
                        definition.key.typeId);
    }
    const auto id = addSource(fixture);
    exercise<MoveNodes>(test, fixture,
                        std::map<NodeId, Vec2d>{{id, {12, 34}}, {kFirstLayerNodeId, {-6, 9}}});
    exercise<SetNodeCollapsed>(test, fixture, id, true);
    exercise<SetNodeMuted>(test, fixture, id, true);
    exercise<SetNodeWidth>(test, fixture, id, 239.25);
    const auto& layout = composition(fixture.document.snapshot()).nodeLayout().at(id);
    test.expect(layout.position == Vec2d{12, 34} && layout.collapsed && layout.muted &&
                    layout.width == 239.25,
                "layout fields change independently");
    const auto before = fixture.document.snapshot();
    test.expect(apply<SetNodeCollapsed>(fixture, id, true).status == CommandStatus::NoChange &&
                    apply<SetNodeMuted>(fixture, id, true).status == CommandStatus::NoChange &&
                    apply<SetNodeWidth>(fixture, id, 239.25).status == CommandStatus::NoChange &&
                    apply<MoveNodes>(fixture, std::map<NodeId, Vec2d>{{id, {12, 34}}}).status ==
                        CommandStatus::NoChange &&
                    fixture.document.snapshot().revision() == before.revision(),
                "identical layout edits do not dirty or advance revision");
    refuse<AddNode>(test, fixture, OperationIssueCode::Unsupported, "test.missing", Vec2d{});
    refuse<AddNode>(test, fixture, OperationIssueCode::InvalidValue,
                    std::string(kSolidSourceNodeType),
                    Vec2d{std::numeric_limits<double>::infinity(), 0});
    const auto missing = NodeId::fromRaw(99999);
    refuse<MoveNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                      std::map<NodeId, Vec2d>{{id, {1, 2}}, {missing, {0, 0}}});
    refuse<MoveNodes>(test, fixture, OperationIssueCode::InvalidValue,
                      std::map<NodeId, Vec2d>{{id, {0, std::numeric_limits<double>::quiet_NaN()}}});
    refuse<SetNodeCollapsed>(test, fixture, OperationIssueCode::InvalidTarget, missing, true);
    refuse<SetNodeMuted>(test, fixture, OperationIssueCode::InvalidTarget, missing, true);
    refuse<SetNodeWidth>(test, fixture, OperationIssueCode::InvalidTarget, missing, 100);
    for (const auto width : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()})
        refuse<SetNodeWidth>(test, fixture, OperationIssueCode::InvalidValue, id, width);
}

void testNewNodePlacementDoesNotMoveExistingCards(TestContext& test) {
    Fixture fixture;
    const auto before = composition(fixture.document.snapshot()).nodeLayout();
    const auto requested = before.at(kLayerStackNodeId).position;
    const auto result = apply<AddNode>(fixture, std::string(kSolidSourceNodeType), requested);
    const auto added = result.outputId<NodeId>(kAddNodeOutput);
    if (!added) {
        test.fail("collision placement returns the new node");
        return;
    }
    const auto& after = composition(fixture.document.snapshot()).nodeLayout();
    for (const auto& [id, record] : before)
        test.expect(after.at(id).position == record.position,
                    "placing a new node does not move an existing card");
    test.expect(after.at(*added).position != requested,
                "a new card moves to the nearest free slot when its requested slot is occupied");
}

void testWiringAndRename(TestContext& test) {
    Fixture fixture;
    const auto source = addSource(fixture);
    const auto replacement = addSource(fixture);
    const InputPortRef firstInput = NodeInputRef{kFirstLayerNodeId, "image"};
    exercise<ConnectPorts>(test, fixture, OutputPortRef{source, "image"}, firstInput);
    const auto initialEdge = composition(fixture.document.snapshot()).graph().edges().back().id;
    exercise<ConnectPorts>(test, fixture, OutputPortRef{replacement, "image"}, firstInput);
    const auto edges = composition(fixture.document.snapshot()).graph().edges();
    test.expect(std::ranges::count(edges, firstInput, &EdgeRecord::destination) == 1 &&
                    std::ranges::any_of(edges,
                                        [&](const auto& edge) {
                                            return edge.id == initialEdge &&
                                                   edge.source.nodeId == replacement;
                                        }),
                "rewire replaces one input edge and retains its stable identity");
    test.expect(
        apply<ConnectPorts>(fixture, OutputPortRef{replacement, "image"}, firstInput).status ==
            CommandStatus::NoChange,
        "identical connect is a no-op");
    exercise<DisconnectInput>(test, fixture, firstInput);
    test.expect(apply<DisconnectInput>(fixture, firstInput).status == CommandStatus::NoChange,
                "already-disconnected input is a no-op");
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::InvalidTarget,
                         OutputPortRef{source, "absent"}, firstInput);
    refuse<DisconnectInput>(test, fixture, OperationIssueCode::InvalidTarget,
                            InputPortRef{NodeInputRef{source, "image"}});
    const InputPortRef slotInput = LayerStackInputRef{kLayerStackNodeId, kFirstSlotId, "content"};
    exercise<ConnectPorts>(test, fixture, OutputPortRef{source, "image"}, slotInput);
    // ADAPTED (task FIX1, item B): detaching a stack slot's content REMOVES the slot, and
    // connecting a Layer output to the sentinel slot creates one. The slot and the link into it are
    // one thing to the artist, so they are one thing here -- which is what makes a stack slot
    // detachable at all.
    exercise<DisconnectInput>(test, fixture, slotInput);
    test.expect(fixture.document.snapshot()
                        .project()
                        .findComposition(kCompositionId)
                        ->graph()
                        .layerStack()
                        .find(kFirstSlotId) == nullptr,
                "disconnecting a stack slot removes the slot");
    const auto before = fixture.document.snapshot()
                            .project()
                            .findComposition(kCompositionId)
                            ->graph()
                            .layerStack()
                            .entries()
                            .size() +
                        1;
    const InputPortRef newSlot = LayerStackInputRef{kLayerStackNodeId, LayerSlotId{}, "content"};
    exercise<ConnectPorts>(test, fixture, OutputPortRef{kFirstLayerNodeId, "image"}, newSlot);
    test.expect(fixture.document.snapshot()
                        .project()
                        .findComposition(kCompositionId)
                        ->graph()
                        .layerStack()
                        .entries()
                        .size() == before,
                "connecting a Layer output to Merge creates its stack slot");
    exercise<ConnectPorts>(test, fixture, OutputPortRef{kFirstLayerNodeId, "image"},
                           InputPortRef{NodeInputRef{kSecondLayerNodeId, "image"}});
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::GraphCycle,
                         OutputPortRef{kSecondLayerNodeId, "image"}, firstInput);
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::GraphCycle,
                         OutputPortRef{kFirstLayerNodeId, "image"}, firstInput);
    NodeDefinitionRegistry registry;
    if (!(registerBuiltInNodeDefinitions(registry)))
        throw std::logic_error("kind registry");
    if (!(registry.registerDefinition({{"test.scalar", 1},
                                       NodeLoweringKind::Unsupported,
                                       {},
                                       {{"value", SocketValueKind::Scalar}},
                                       {},
                                       std::nullopt}) == NodeRegistrationStatus::Registered))
        throw std::logic_error("scalar schema");
    registry.freeze();
    const auto scalar =
        apply<AddNode>(fixture, "test.scalar", Vec2d{}, registry).outputId<NodeId>(kAddNodeOutput);
    if (!(scalar.has_value()))
        throw std::logic_error("scalar source");
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{*scalar, "value"}, firstInput, registry);
    exercise<RenameLayer>(test, fixture, kFirstLayerId, "Hero copy");
    test.expect(composition(fixture.document.snapshot()).graph().layerOutputs().front().name ==
                    "Hero copy",
                "rename changes the owning layer boundary");
    test.expect(apply<RenameLayer>(fixture, kFirstLayerId, "Hero copy").status ==
                    CommandStatus::NoChange,
                "same layer name is a no-op");
    refuse<RenameLayer>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId, "");
    refuse<RenameLayer>(test, fixture, OperationIssueCode::InvalidTarget, LayerId::fromRaw(999),
                        "Missing");
}

// ADAPTED (#502): dissolving a PARTICIPATING Layer Output keeps its Merge entry. The slot takes the
// Layer node's content source, and the reconcile pass gives the entry the Layer node's values as its
// own entry layer, so the picture does not change. A child Layer node feeding the same Merge cannot
// be parented to an entry layer (it would evaluate before its parent), so its link clears.
void testDissolveParticipatingLayer(TestContext& test) {
    Fixture fixture;
    const auto source = addSource(fixture);
    if (!(apply<ConnectPorts>(fixture, OutputPortRef{source, "image"},
                              InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}})
              .changed()))
        throw std::logic_error("connect participating layer input");
    const auto slotsBefore =
        composition(fixture.document.snapshot()).graph().layerStack().entries().size();
    (void)apply<SetLayerParent>(fixture, kSecondLayerId, kFirstLayerId);
    exercise<DissolveNode>(test, fixture, kFirstLayerNodeId);
    test.expect(!composition(fixture.document.snapshot()).graph().findLayer(kSecondLayerId)->parent,
                "dissolving a parent clears child links and restores them on undo");
    const auto& after = composition(fixture.document.snapshot());
    const auto* entry = after.graph().layerStack().find(kFirstSlotId);
    const auto* layer = entry ? after.graph().findLayer(entry->layerId) : nullptr;
    test.expect(after.graph().findNode(kFirstLayerNodeId) == nullptr &&
                    after.graph().layerStack().entries().size() == slotsBefore &&
                    after.graph().layerOutputs().size() == 2 && layer != nullptr &&
                    layer->entryOwned() && layer->name == "First",
                "dissolving a participating Layer Output keeps its entry with the Layer's values");
}

void testRemoveAndDissolve(TestContext& test) {
    Fixture fixture;
    const auto source = addSource(fixture);
    const auto loose = apply<AddNode>(fixture, std::string(kLayerOutputNodeType), Vec2d{})
                           .outputId<NodeId>(kAddNodeOutput);
    if (!(loose.has_value()))
        throw std::logic_error("loose boundary node");
    if (!(apply<ConnectPorts>(fixture, OutputPortRef{source, "image"},
                              InputPortRef{NodeInputRef{*loose, "image"}})
              .changed()))
        throw std::logic_error("connect dissolve input");
    for (const auto target : {kFirstLayerNodeId, kSecondLayerNodeId})
        if (!(apply<ConnectPorts>(fixture, OutputPortRef{*loose, "image"},
                                  InputPortRef{NodeInputRef{target, "image"}})
                  .changed()))
            throw std::logic_error("connect dissolve consumer");
    const auto count = composition(fixture.document.snapshot()).parameters().records().size();
    exercise<DissolveNode>(test, fixture, *loose);
    const auto& dissolved = composition(fixture.document.snapshot());
    test.expect(!dissolved.graph().findNode(*loose) && !dissolved.nodeLayout().contains(*loose) &&
                    // ADAPTED (task S4, then blend modes): a Layer Output node now owns six
                    // parameters, not two, so dissolving one orphans six.
                    dissolved.parameters().records().size() == count - 6 &&
                    std::ranges::count_if(
                        dissolved.graph().edges(),
                        [&](const auto& edge) { return edge.source.nodeId == source; }) == 2,
                "dissolve removes orphan parameters/layout and reconnects every consumer");
    // #704: a node with nothing to reconnect through dissolves by deleting.
    test.expect(canApplyNodeOperation(fixture.document.snapshot(), DissolveNode(kCompositionId, source)),
                "a source with no image input dissolves by deleting");
    refuse<DissolveNode>(test, fixture, OperationIssueCode::InvalidTarget, NodeId::fromRaw(999));
    refuse<RemoveNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                        std::set<NodeId>{source, NodeId::fromRaw(999)});
    const auto compositionOutput =
        apply<AddNode>(fixture, std::string(kCompositionOutputNodeType), Vec2d{})
            .outputId<NodeId>(kAddNodeOutput);
    // And a second one is refused, here as well as in testAddAndLayout: this is the fixture that
    // actually holds a composition output, so it is where the refusal is worth restating.
    refuse<AddNode>(test, fixture, OperationIssueCode::Unsupported,
                    std::string(kCompositionOutputNodeType), Vec2d{});
    if (!(compositionOutput.has_value()))
        throw std::logic_error("output fixture");
    // #716: an Output node deletes like any other node, in one undo step.
    exercise<RemoveNodes>(test, fixture, std::set<NodeId>{*compositionOutput});
    exercise<RemoveNodes>(test, fixture, std::set<NodeId>{kFirstLayerNodeId});
    const auto& removed = composition(fixture.document.snapshot());
    test.expect(
        removed.graph().findNode(source) && removed.graph().layerStack().entries().size() == 1 &&
            removed.graph().layerStack().entries().front().slotId == kSecondSlotId &&
            removed.graph().layerOutputs().size() == 1,
        "removing a Layer Output drops its boundary/slot and preserves shared upstream nodes");
    exercise<RemoveNodes>(test, fixture, std::set<NodeId>{source});
    test.expect(apply<RemoveNodes>(fixture, std::set<NodeId>{}).status == CommandStatus::NoChange,
                "empty removal is a no-op");
}

// #716: the composition's Output and the Merge feeding it delete and dissolve like any other
// node, each in one undo step. The composition then has no output until an Output is added again,
// and no Merge to hold a new layer until one is added.
void testDeleteOutputAndItsMerge(TestContext& test) {
    const auto graphOf = [](const Fixture& fixture) -> const CanonicalGraph& {
        return composition(fixture.document.snapshot()).graph();
    };
    {
        // This fixture's composition output is its Merge.
        Fixture fixture;
        test.expect(graphOf(fixture).compositionOutput().has_value() &&
                        graphOf(fixture).compositionOutput()->nodeId == kLayerStackNodeId,
                    "the fixture's output is its Merge");
        exercise<RemoveNodes>(test, fixture, std::set<NodeId>{kLayerStackNodeId});
        test.expect(!graphOf(fixture).findNode(kLayerStackNodeId) &&
                        !graphOf(fixture).compositionOutput().has_value() &&
                        graphOf(fixture).merges().empty(),
                    "#716: deleting the output Merge leaves a composition with no output");
        refuse<AddSolidLayer>(test, fixture, OperationIssueCode::Unsupported, std::string("Solid"),
                              core::Color4d{1.0, 0.0, 0.0, 1.0});
        test.expect(fixture.stack.undo().changed() &&
                        graphOf(fixture).compositionOutput().has_value() &&
                        graphOf(fixture).compositionOutput()->nodeId == kLayerStackNodeId,
                    "#716: one undo brings the Merge and the output back");
    }
    {
        Fixture fixture;
        exercise<DissolveNode>(test, fixture, kLayerStackNodeId);
        test.expect(!graphOf(fixture).findNode(kLayerStackNodeId) &&
                        !graphOf(fixture).compositionOutput().has_value(),
                    "#716: Dissolve removes the output Merge as Delete does");
    }
}

void testDeepDuplication(TestContext& test) {
    Fixture fixture;
    const auto source = addSource(fixture);
    if (!(apply<ConnectPorts>(fixture, OutputPortRef{source, "image"},
                              InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}})
              .changed()))
        throw std::logic_error("duplicate internal edge");
    if (!(apply<CreateAnimationForParameter>(fixture, kOpacityId, bloom::core::RationalTime{})
              .changed()))
        throw std::logic_error("animated duplicate fixture");
    if (!(apply<SetNodeWidth>(fixture, source, 250.0).changed() &&
          apply<SetNodeCollapsed>(fixture, source, true).changed() &&
          apply<SetNodeMuted>(fixture, source, true).changed()))
        throw std::logic_error("duplicate layout fixture");
    const auto original = fixture.document.snapshot();
    const auto result = exercise<DuplicateNodes>(
        test, fixture, std::set<NodeId>{source, kFirstLayerNodeId}, Vec2d{20, -10});
    const auto copySource = result.outputId<NodeId>("node." + std::to_string(source.value()));
    const auto copyLayer =
        result.outputId<LayerId>("layer." + std::to_string(kFirstLayerId.value()));
    const auto copyBoundary =
        result.outputId<NodeId>("node." + std::to_string(kFirstLayerNodeId.value()));
    const auto copyOpacity =
        result.outputId<ParameterId>("parameter." + std::to_string(kOpacityId.value()));
    if (!(copySource && copyLayer && copyBoundary && copyOpacity))
        throw std::logic_error("duplicate returns original-to-copy identities");
    const auto& comp = composition(fixture.document.snapshot());
    const auto slots = comp.graph().layerStack().entries();
    test.expect(slots.size() == 3 && slots[0].layerId == kFirstLayerId &&
                    slots[1].layerId == *copyLayer && slots[2].layerId == kSecondLayerId,
                "duplicate slot follows original and preserves surrounding order");
    const auto boundaries = comp.graph().layerOutputs();
    test.expect(std::ranges::any_of(boundaries,
                                    [&](const auto& boundary) {
                                        return boundary.layerId == *copyLayer &&
                                               boundary.name == "First copy";
                                    }),
                "duplicate layer name and LayerId");
    auto expectedLayout = composition(original).nodeLayout().at(source);
    expectedLayout.position.x += 20;
    expectedLayout.position.y -= 10;
    test.expect(comp.nodeLayout().at(*copySource) == expectedLayout,
                "duplicate copies width/collapse/mute and applies position offset");
    const auto* oldOpacity = composition(original).parameters().find(kOpacityId);
    const auto* newOpacity = comp.parameters().find(*copyOpacity);
    if (!(oldOpacity && newOpacity))
        throw std::logic_error("opacity records");
    const auto oldCurve = std::get<AnimationCurveSource>(oldOpacity->source).curveId;
    const auto newCurve = std::get<AnimationCurveSource>(newOpacity->source).curveId;
    const auto* oldKeys = comp.animationCurves().findScalar(oldCurve);
    const auto* newKeys = comp.animationCurves().findScalar(newCurve);
    test.expect(oldCurve != newCurve && oldKeys && newKeys &&
                    oldKeys->keyframes.size() == newKeys->keyframes.size() &&
                    oldKeys->keyframes.front().id != newKeys->keyframes.front().id &&
                    oldKeys->keyframes.front().value == newKeys->keyframes.front().value,
                "curves and keyframes are deeply copied with new identities");
    test.expect(std::ranges::any_of(comp.graph().edges(),
                                    [&](const auto& edge) {
                                        return edge.source.nodeId == *copySource &&
                                               edge.destination == InputPortRef{NodeInputRef{
                                                                       *copyBoundary, "image"}};
                                    }),
                "internal edge joins duplicated nodes");
    const auto isolated = exercise<DuplicateNodes>(test, fixture, std::set<NodeId>{source}, Vec2d{})
                              .outputId<NodeId>("node." + std::to_string(source.value()));
    if (!(isolated.has_value()))
        throw std::logic_error("isolated copy");
    test.expect(
        std::ranges::none_of(composition(fixture.document.snapshot()).graph().edges(),
                             [&](const auto& edge) { return edge.source.nodeId == *isolated; }),
        "edges to unselected nodes are not copied");
    exercise<RemoveNodes>(test, fixture, std::set<NodeId>{*copyBoundary});
    test.expect(!composition(fixture.document.snapshot()).animationCurves().find(newCurve) &&
                    composition(fixture.document.snapshot()).animationCurves().find(oldCurve),
                "removing copied animation owner cleans only its orphan curve");
    refuse<DuplicateNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                           std::set<NodeId>{NodeId::fromRaw(999)}, Vec2d{});
    refuse<DuplicateNodes>(test, fixture, OperationIssueCode::InvalidValue,
                           std::set<NodeId>{source},
                           Vec2d{std::numeric_limits<double>::infinity(), 0});
    test.expect(apply<DuplicateNodes>(fixture, std::set<NodeId>{}, Vec2d{}).status ==
                    CommandStatus::NoChange,
                "empty duplication is a no-op");
}
void testDuplicationOwnershipEdges(TestContext& test) {
    Fixture fixture;
    if (!apply<CreateAnimationForParameter>(fixture, kFirstPositionId, core::RationalTime{})
             .changed())
        throw std::logic_error("vec2 animation fixture");
    const auto original = fixture.document.snapshot();
    const auto result = exercise<DuplicateNodes>(
        test, fixture, std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId}, Vec2d{});
    const auto copiedPosition =
        result.outputId<ParameterId>("parameter." + std::to_string(kFirstPositionId.value()));
    if (!copiedPosition)
        throw std::logic_error("copied vec2 parameter");
    const auto& comp = composition(fixture.document.snapshot());
    const auto oldCurve = std::get<AnimationCurveSource>(
                              composition(original).parameters().find(kFirstPositionId)->source)
                              .curveId;
    const auto newCurve =
        std::get<AnimationCurveSource>(comp.parameters().find(*copiedPosition)->source).curveId;
    const auto* oldKeys = comp.animationCurves().findVec2(oldCurve);
    const auto* newKeys = comp.animationCurves().findVec2(newCurve);
    test.expect(oldCurve != newCurve && oldKeys && newKeys &&
                    oldKeys->components[0].keyframes.front().id !=
                        newKeys->components[0].keyframes.front().id &&
                    oldKeys->components[0].keyframes.front().value ==
                        newKeys->components[0].keyframes.front().value,
                "vec2 animation copies independently with fresh curve/key identities");
    const auto firstCopy =
        result.outputId<LayerId>("layer." + std::to_string(kFirstLayerId.value()));
    const auto secondCopy =
        result.outputId<LayerId>("layer." + std::to_string(kSecondLayerId.value()));
    const auto entries = comp.graph().layerStack().entries();
    test.expect(firstCopy && secondCopy && entries.size() == 4 &&
                    entries[0].layerId == kFirstLayerId && entries[1].layerId == *firstCopy &&
                    entries[2].layerId == kSecondLayerId && entries[3].layerId == *secondCopy,
                "multiple copied layer slots each follow their own original");
    exercise<DuplicateNodes>(test, fixture, std::set<NodeId>{kFirstLayerNodeId, kLayerStackNodeId},
                             Vec2d{});
    if (!apply<MoveNodes>(
             fixture,
             std::map<NodeId, Vec2d>{{kFirstLayerNodeId, {std::numeric_limits<double>::max(), 0}}})
             .changed())
        throw std::logic_error("position overflow fixture");
    refuse<DuplicateNodes>(test, fixture, OperationIssueCode::InvalidValue,
                           std::set<NodeId>{kFirstLayerNodeId},
                           Vec2d{std::numeric_limits<double>::max(), 0});

    // ADAPTED (task S7): a driver binding names a value node's output now, so the fixture adds the
    // Scalar node the binding points at instead of allocating a bare driver id. The contract under
    // test is unchanged -- duplicating a node whose parameter is driven is still refused, because a
    // copy would need a second driver nothing asked for.
    auto before = fixture.document.snapshot();
    auto draft = fixture.document.draft(before);
    const auto valueNodeId = draft.ids().allocateNode();
    const auto valueParameterId = draft.ids().allocateParameter();
    auto* drivenComposition = draft.project().findComposition(kCompositionId);
    if (!valueNodeId || !valueParameterId || drivenComposition == nullptr ||
        !drivenComposition->parameters().insert(
            {*valueParameterId, std::string(bloom::document::kScalarValueParameterSchemaKey),
             ConstantValueSource{0.5}}) ||
        !drivenComposition->graph().addNode(
            {*valueNodeId,
             std::string(bloom::document::kScalarValueNodeType),
             {{std::string(bloom::document::kValueParameterRole), *valueParameterId}},
             bloom::document::kValueNodeSchemaVersion}) ||
        !drivenComposition->parameters().setSource(
            kSecondOpacityId,
            DriverBindingSource{*valueNodeId, std::string(bloom::document::kValuePortName)}) ||
        !fixture.document.commit(before.revision(), std::move(draft)).committed())
        throw std::logic_error("driver fixture");
    fixture.stack.clear();
    const auto drivenCopy =
        exercise<DuplicateNodes>(test, fixture, std::set<NodeId>{kSecondLayerNodeId}, Vec2d{});
    const auto copiedOpacity =
        drivenCopy.outputId<ParameterId>("parameter." + std::to_string(kSecondOpacityId.value()));
    const auto copiedSnapshot = fixture.document.snapshot();
    const auto* copiedParameter =
        copiedOpacity ? composition(copiedSnapshot).parameters().find(*copiedOpacity) : nullptr;
    test.expect(copiedParameter &&
                    copiedParameter->source == ParameterSource{DriverBindingSource{
                                                   *valueNodeId, std::string(kValuePortName)}},
                "duplicated Layer parameters retain their upstream driver and exact undo/redo");
}

// Node groups: one transaction each, undo/redo pinned by exercise() above, and the one rule that
// spans them all -- a node belongs to exactly one group, and a frame emptied by any of these goes.
// Task S7, items 2 and 3: a link into an operand socket is the parameter's driver binding, and the
// same ConnectPorts/DisconnectInput pair that wires image transport authors it. No second gesture,
// no second command, and undo restores the exact constant the link replaced.
void testParameterSocketDrivers(TestContext& test) {
    Fixture fixture;
    const auto parameterOf = [&fixture](const NodeId nodeId, const std::string_view role) {
        const auto snapshot = fixture.document.snapshot();
        const auto& graph = composition(snapshot).graph();
        const auto* node = graph.findNode(nodeId);
        if (node == nullptr)
            throw std::logic_error("driver fixture node");
        const auto binding = std::ranges::find(node->parameters, role, &ParameterBinding::role);
        if (binding == node->parameters.end())
            throw std::logic_error("driver fixture role");
        const auto* parameter = composition(snapshot).parameters().find(binding->parameterId);
        if (parameter == nullptr)
            throw std::logic_error("driver fixture parameter");
        return *parameter;
    };

    const auto scalarNode = apply<AddNode>(fixture, std::string(kScalarValueNodeType), Vec2d{8, 9})
                                .outputId<NodeId>(kAddNodeOutput);
    if (!scalarNode.has_value())
        throw std::logic_error("scalar value node fixture");

    const InputPortRef opacitySocket =
        NodeInputRef{kFirstLayerNodeId, std::string(kOpacityParameterRole)};
    test.expect(std::holds_alternative<ConstantValueSource>(
                    parameterOf(kFirstLayerNodeId, kOpacityParameterRole).source),
                "an unlinked operand socket's parameter is a constant");

    exercise<ConnectPorts>(test, fixture, OutputPortRef{*scalarNode, std::string(kValuePortName)},
                           opacitySocket);
    const auto linked = parameterOf(kFirstLayerNodeId, kOpacityParameterRole).source;
    test.expect(std::holds_alternative<DriverBindingSource>(linked) &&
                    std::get<DriverBindingSource>(linked) ==
                        DriverBindingSource{*scalarNode, std::string(kValuePortName)},
                "linking an operand socket sets its parameter's driver binding");
    test.expect(
        std::ranges::none_of(composition(fixture.document.snapshot()).graph().edges(),
                             [&](const auto& edge) { return edge.destination == opacitySocket; }),
        "and records no edge: one authored value has one durable source");
    test.expect(apply<ConnectPorts>(
                    fixture, OutputPortRef{*scalarNode, std::string(kValuePortName)}, opacitySocket)
                        .status == CommandStatus::NoChange,
                "relinking the same output is a no-op");

    exercise<DisconnectInput>(test, fixture, opacitySocket);
    test.expect(std::holds_alternative<ConstantValueSource>(
                    parameterOf(kFirstLayerNodeId, kOpacityParameterRole).source),
                "unlinking restores a constant source");
    test.expect(apply<DisconnectInput>(fixture, opacitySocket).status == CommandStatus::NoChange,
                "an already-unlinked operand socket is a no-op");

    // The kind rule is the shared promotion whitelist: an Integer node drives a Scalar operand
    // through an explicit widening, and a String node drives nothing numeric at all.
    const auto integerNode =
        apply<AddNode>(fixture, std::string(kIntegerValueNodeType), Vec2d{1, 2})
            .outputId<NodeId>(kAddNodeOutput);
    const auto stringNode = apply<AddNode>(fixture, std::string(kStringValueNodeType), Vec2d{3, 4})
                                .outputId<NodeId>(kAddNodeOutput);
    if (!integerNode.has_value() || !stringNode.has_value())
        throw std::logic_error("promotion fixture nodes");
    exercise<ConnectPorts>(test, fixture, OutputPortRef{*integerNode, std::string(kValuePortName)},
                           opacitySocket);
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{*stringNode, std::string(kValuePortName)}, opacitySocket);
    // Nothing promotes INTO an Integer socket from a Scalar: the blend mode is a closed
    // enumeration, and a number between two modes is not a mode.
    refuse<ConnectPorts>(
        test, fixture, OperationIssueCode::SocketKindMismatch,
        OutputPortRef{*scalarNode, std::string(kValuePortName)},
        InputPortRef{NodeInputRef{kFirstLayerNodeId, std::string(kBlendModeParameterRole)}});
}

// #565: the card's estimated height follows the Film Grain mode: Sensor Noise shows one row more.
void testFilmGrainCardEstimate(TestContext& test) {
    Fixture fixture;
    const auto added = exercise<AddNode>(test, fixture, "bloom.film-grain", Vec2d{});
    const auto node = added.outputId<NodeId>(kAddNodeOutput);
    const auto mode = added.outputId<ParameterId>("parameter.mode");
    if (!node || !mode)
        throw std::logic_error("Film Grain exposes its identity and mode");
    const auto height = [&] {
        return estimatedNodeCardSize(*fixture.document.snapshot().project().findComposition(
                                         kCompositionId),
                                     *node)
            .height;
    };
    const double film = height();
    (void)exercise<SetParameterSource>(test, fixture, *mode, ConstantValueSource{std::int64_t{1}});
    const double sensor = height();
    test.expect(sensor > film && sensor - film < 60.0,
                "a Film Grain in Sensor Noise mode is estimated one row taller");
    (void)exercise<SetParameterSource>(test, fixture, *mode, ConstantValueSource{std::int64_t{0}});
    test.expect(height() == film, "back in Film mode the estimate is Film's again");
}

void testNodeGroups(TestContext& test) {
    Fixture fixture;
    const auto composition = [&fixture] {
        return fixture.document.snapshot().project().findComposition(kCompositionId);
    };
    const auto groups = [&fixture]() -> const NodeGroups& {
        return fixture.document.snapshot().project().findComposition(kCompositionId)->nodeGroups();
    };

    const auto created = exercise<GroupNodes>(
        test, fixture, std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId});
    const auto first = created.outputId<NodeGroupId>(kGroupNodesOutput);
    test.expect(first.has_value(), "GroupNodes reports the group it created");
    if (!first)
        return;
    test.expect(groups().size() == 1 &&
                    groups().at(*first).members ==
                        std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId} &&
                    groups().at(*first).name == kDefaultNodeGroupName,
                "a new group holds exactly the nodes it was given, under the default name");
    test.expect(groups().at(*first).padding.x == kDefaultNodeGroupPadding &&
                    groups().at(*first).padding.y == kDefaultNodeGroupPadding,
                "a new group takes the frozen default padding");

    // A second group over one of the first group's members takes it away from the first.
    const auto second = exercise<GroupNodes>(test, fixture, std::set<NodeId>{kSecondLayerNodeId},
                                             std::string("Lighting"))
                            .outputId<NodeGroupId>(kGroupNodesOutput);
    test.expect(second.has_value(), "a second group is created");
    if (!second)
        return;
    test.expect(groups().size() == 2 &&
                    groups().at(*first).members == std::set<NodeId>{kFirstLayerNodeId} &&
                    groups().at(*second).members == std::set<NodeId>{kSecondLayerNodeId},
                "members leave their previous group");

    // Renaming: one applies, an identical name is a no-op that writes nothing, and an empty name is
    // refused exactly as a layer name is.
    (void)exercise<RenameGroup>(test, fixture, *second, std::string("Key Light"));
    test.expect(groups().at(*second).name == "Key Light", "RenameGroup renames in place");
    const auto before = fixture.document.snapshot();
    const auto unchanged = apply<RenameGroup>(fixture, *second, std::string("Key Light"));
    test.expect(!unchanged.changed() && fixture.document.snapshot().revision() == before.revision(),
                "renaming a group to its own name changes nothing");
    refuse<RenameGroup>(test, fixture, OperationIssueCode::InvalidValue, *second, std::string{});
    refuse<GroupNodes>(test, fixture, OperationIssueCode::InvalidValue,
                       std::set<NodeId>{kFirstLayerNodeId}, std::string{});
    refuse<GroupNodes>(test, fixture, OperationIssueCode::InvalidValue, std::set<NodeId>{});
    refuse<GroupNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                       std::set<NodeId>{NodeId::fromRaw(9999)});
    refuse<RenameGroup>(test, fixture, OperationIssueCode::InvalidTarget,
                        NodeGroupId::fromRaw(9999), std::string("Nowhere"));
    refuse<UngroupNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                         NodeGroupId::fromRaw(9999));

    // Membership by command: the second frame takes both nodes, which empties the first one.
    (void)exercise<SetGroupMembers>(test, fixture, *second,
                                    std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId});
    test.expect(groups().size() == 1 && !groups().contains(*first) &&
                    groups().at(*second).members ==
                        std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId},
                "a group emptied by another group's membership change is removed");
    refuse<SetGroupMembers>(test, fixture, OperationIssueCode::InvalidTarget, *second,
                            std::set<NodeId>{NodeId::fromRaw(9999)});

    // A move that carries a membership delta is one transaction: exercise() proves the single undo.
    (void)exercise<MoveNodes>(test, fixture,
                              std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{111, 222}}},
                              NodeGroupMembershipDelta{{kFirstLayerNodeId, std::nullopt}});
    test.expect(composition()->nodeLayout().at(kFirstLayerNodeId).position == Vec2d{111, 222} &&
                    groups().at(*second).members == std::set<NodeId>{kSecondLayerNodeId},
                "a drag out of a frame moves the card and drops its membership together");
    (void)exercise<MoveNodes>(test, fixture,
                              std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{5, 6}}},
                              NodeGroupMembershipDelta{{kFirstLayerNodeId, *second}});
    test.expect(groups().at(*second).members ==
                    std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId},
                "a drop inside a frame adds the card in the same transaction");
    refuse<MoveNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                      std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{1, 1}}},
                      NodeGroupMembershipDelta{{kFirstLayerNodeId, NodeGroupId::fromRaw(9999)}});

    // Ungrouping keeps every node and every layout record; only the frame goes.
    const auto layoutBefore = composition()->nodeLayout();
    (void)exercise<UngroupNodes>(test, fixture, *second);
    test.expect(groups().empty() && composition()->nodeLayout() == layoutBefore &&
                    composition()->graph().findNode(kFirstLayerNodeId) != nullptr,
                "ungrouping removes only the frame");

    // Removing a node empties the frame that held it, and the frame goes with it.
    const auto solid = addSource(fixture);
    const auto lone = apply<GroupNodes>(fixture, std::set<NodeId>{solid})
                          .outputId<NodeGroupId>(kGroupNodesOutput);
    test.expect(lone.has_value() && groups().size() == 1, "a one-node group exists");
    (void)exercise<RemoveNodes>(test, fixture, std::set<NodeId>{solid});
    test.expect(groups().empty(), "removing a group's last node removes the group");
}

// LAYOUT-1: the card-layout and node-group commands opt out of render effect; every other node
// command keeps the conservative render-affecting default. Since #399 the command stack also
// proves the impact from the document's field classification (evaluation_inputs_tests.cpp), which
// is why SetNodeMuted -- it writes the one evaluated node-layout field -- stays render-affecting.
void testRenderAffectingClassification(TestContext& test) {
    {
        Fixture fixture;
        const auto source = addSource(fixture);
        test.expect(!apply<MoveNodes>(fixture, std::map<NodeId, Vec2d>{{source, Vec2d{12, 34}}})
                         .renderAffecting,
                    "MoveNodes is layout-only");
        test.expect(!apply<SetNodeCollapsed>(fixture, source, true).renderAffecting,
                    "SetNodeCollapsed is layout-only");
        test.expect(!apply<SetNodeWidth>(fixture, source, 256.0).renderAffecting,
                    "SetNodeWidth is layout-only");
        const auto group = apply<GroupNodes>(fixture, std::set<NodeId>{source});
        test.expect(!group.renderAffecting, "GroupNodes is layout-only");
        const auto groupId = group.outputId<NodeGroupId>(kGroupNodesOutput);
        test.expect(groupId.has_value(), "GroupNodes reports the group it created");
        if (groupId.has_value()) {
            test.expect(
                !apply<RenameGroup>(fixture, *groupId, std::string("Renamed")).renderAffecting,
                "RenameGroup is layout-only");
            test.expect(!apply<SetGroupMembers>(fixture, *groupId,
                                                std::set<NodeId>{source, kFirstLayerNodeId})
                             .renderAffecting,
                        "SetGroupMembers is layout-only");
            test.expect(!apply<UngroupNodes>(fixture, *groupId).renderAffecting,
                        "UngroupNodes is layout-only");
        }
        test.expect(apply<SetNodeMuted>(fixture, source, true).renderAffecting,
                    "SetNodeMuted is pixel-affecting despite living in node layout");
        test.expect(
            apply<AddNode>(fixture, std::string(kSolidSourceNodeType), Vec2d{3, 4}).renderAffecting,
            "an unclassified node operation defaults to render-affecting");

        // A pixel-affecting operation that reports NoChange beside an applied layout edit must not
        // make the transaction invalidating.
        const auto before = fixture.document.snapshot();
        Transaction noChange("Muted no-op and move", before.revision());
        noChange.emplace<SetNodeMuted>(kCompositionId, source, true);
        noChange.emplace<MoveNodes>(kCompositionId,
                                    std::map<NodeId, Vec2d>{{source, Vec2d{40, 41}}});
        const auto noChangeResult = fixture.stack.execute(std::move(noChange));
        test.expect(noChangeResult.status == CommandStatus::Succeeded &&
                        !noChangeResult.renderAffecting,
                    "a pixel NoChange beside an applied layout edit stays layout-only");
    }

    // A mixed applied transaction is conservatively render-affecting in both orderings, and both
    // undo and redo replay the impact stored with the history entry.
    for (const bool layoutFirst : {true, false}) {
        Fixture fixture;
        Transaction mixed("Mixed node edit", fixture.document.snapshot().revision());
        if (layoutFirst) {
            mixed.emplace<MoveNodes>(kCompositionId,
                                     std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{11, 12}}});
            mixed.emplace<SetNodeMuted>(kCompositionId, kFirstLayerNodeId, true);
        } else {
            mixed.emplace<SetNodeMuted>(kCompositionId, kFirstLayerNodeId, true);
            mixed.emplace<MoveNodes>(kCompositionId,
                                     std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{11, 12}}});
        }
        const auto mixedResult = fixture.stack.execute(std::move(mixed));
        test.expect(mixedResult.changed() && mixedResult.renderAffecting,
                    "a mixed layout+pixel transaction is conservatively render-affecting");
        const auto undo = fixture.stack.undo();
        test.expect(undo.changed() && undo.renderAffecting,
                    "undo replays the stored mixed render impact");
        const auto redo = fixture.stack.redo();
        test.expect(redo.changed() && redo.renderAffecting,
                    "redo replays the stored mixed render impact");
    }

    // A layout-only transaction replays its stored layout-only impact through undo and redo.
    {
        Fixture fixture;
        const auto source = addSource(fixture);
        if (!apply<MoveNodes>(fixture, std::map<NodeId, Vec2d>{{source, Vec2d{5, 6}}}).changed())
            throw std::logic_error("layout undo/redo fixture");
        const auto undo = fixture.stack.undo();
        test.expect(undo.changed() && !undo.renderAffecting,
                    "undo replays the stored layout-only render impact");
        const auto redo = fixture.stack.redo();
        test.expect(redo.changed() && !redo.renderAffecting,
                    "redo replays the stored layout-only render impact");
    }

    // A group edit replays its stored layout-only impact through undo and redo too.
    {
        Fixture fixture;
        if (!apply<GroupNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId}).changed())
            throw std::logic_error("group layout undo/redo fixture");
        const auto undo = fixture.stack.undo();
        test.expect(undo.changed() && !undo.renderAffecting,
                    "undo replays the stored group layout-only render impact");
        const auto redo = fixture.stack.redo();
        test.expect(redo.changed() && !redo.renderAffecting,
                    "redo replays the stored group layout-only render impact");
    }
}

// TEMPORAL-DELETE: RemoveNodes proves a finite changed-time footprint only when every removed node
// is an ordinary layer boundary whose deletion cannot influence output outside its active span.
void testRemovalTimeFootprint(TestContext& test) {
    using document::CompositionId;
    const auto expectSpan = [&test](const std::optional<AffectedTimeFootprint>& footprint,
                                    const std::int64_t start, const std::int64_t end,
                                    const std::string_view message) {
        test.expect(footprint.has_value() && footprint->compositionId == kCompositionId &&
                        footprint->intervals.size() == 1 &&
                        footprint->intervals.front().start ==
                            core::RationalTime::fromInteger(start) &&
                        footprint->intervals.front().end == core::RationalTime::fromInteger(end),
                    message);
    };
    {
        Fixture fixture;
        const auto result = apply<RemoveNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId});
        test.expect(result.changed() && result.renderAffecting, "layer removal publishes");
        expectSpan(result.affectedTimes, 0, 10,
                   "an ordinary full-span layer deletion is confined to its active span");
    }
    {
        Fixture fixture;
        (void)apply<SetLayerRange>(fixture, kFirstLayerId, core::RationalTime::fromInteger(2),
                                   core::RationalTime::fromInteger(5));
        const auto result = apply<RemoveNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId});
        expectSpan(result.affectedTimes, 2, 5, "a finite clip reports only its half-open span");
    }
    {
        Fixture fixture;
        (void)apply<SetLayerRange>(fixture, kFirstLayerId, core::RationalTime::fromInteger(1),
                                   core::RationalTime::fromInteger(4));
        (void)apply<SetLayerRange>(fixture, kSecondLayerId, core::RationalTime::fromInteger(6),
                                   core::RationalTime::fromInteger(9));
        const auto result =
            apply<RemoveNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId});
        test.expect(
            result.changed() && result.affectedTimes.has_value() &&
                result.affectedTimes->intervals.size() == 2 &&
                result.affectedTimes->intervals[0].start == core::RationalTime::fromInteger(1) &&
                result.affectedTimes->intervals[0].end == core::RationalTime::fromInteger(4) &&
                result.affectedTimes->intervals[1].start == core::RationalTime::fromInteger(6) &&
                result.affectedTimes->intervals[1].end == core::RationalTime::fromInteger(9),
            "multiple independent clips publish their normalized union");
    }
    {
        // Undo/redo replay the stored deletion footprint.
        Fixture fixture;
        (void)apply<SetLayerRange>(fixture, kFirstLayerId, core::RationalTime::fromInteger(2),
                                   core::RationalTime::fromInteger(5));
        const auto removed = apply<RemoveNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId});
        test.expect(removed.changed() && removed.affectedTimes.has_value(),
                    "the deletion publishes a footprint");
        const auto undo = fixture.stack.undo();
        test.expect(
            undo.changed() && undo.affectedTimes.has_value() &&
                undo.affectedTimes->intervals.front().start == core::RationalTime::fromInteger(2) &&
                undo.affectedTimes->intervals.front().end == core::RationalTime::fromInteger(5),
            "undo replays the deletion footprint");
        const auto redo = fixture.stack.redo();
        test.expect(redo.changed() && redo.affectedTimes.has_value() &&
                        redo.affectedTimes->intervals.size() == 1,
                    "redo replays the deletion footprint");
    }
    {
        Fixture fixture;
        const auto source = apply<AddNode>(fixture, std::string(kSolidSourceNodeType), Vec2d{})
                                .outputId<NodeId>(kAddNodeOutput);
        if (!source.has_value())
            throw std::logic_error("removal footprint source fixture");
        const auto result = apply<RemoveNodes>(fixture, std::set<NodeId>{*source});
        test.expect(result.changed() && result.renderAffecting &&
                        result.affectedTimes.has_value() && result.affectedTimes->intervals.empty(),
                    "removing an unreachable non-layer node is render-neutral (issue #355)");
    }
    {
        Fixture fixture;
        (void)apply<SetLayerParent>(fixture, kSecondLayerId, kFirstLayerId);
        const auto result = apply<RemoveNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId});
        test.expect(result.changed() && !result.affectedTimes.has_value(),
                    "a surviving child parented to the removed boundary forces whole-render");
    }
    {
        Fixture fixture;
        (void)apply<SetLayerSolo>(fixture, kFirstLayerId, true);
        const auto result = apply<RemoveNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId});
        test.expect(result.changed() && !result.affectedTimes.has_value(),
                    "removing a solo layer can unsuppress others, so it is whole-render");
    }
    {
        Fixture fixture;
        (void)apply<ConnectPorts>(fixture, OutputPortRef{kFirstLayerNodeId, "image"},
                                  InputPortRef{NodeInputRef{kSecondLayerNodeId, "image"}});
        const auto result = apply<RemoveNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId});
        test.expect(result.changed() && !result.affectedTimes.has_value(),
                    "a non-merge downstream consumer forces whole-render");
    }
    {
        // F1 regression: a muted Layer Stack compiles only its first slot, so deleting the layer
        // currently in that slot can promote a later layer into it. The change at [5,10) is not
        // inside the deleted layer's own [0,5) span, so the proof must fall back to whole-render.
        Fixture fixture;
        (void)apply<SetLayerRange>(fixture, kFirstLayerId, core::RationalTime{},
                                   core::RationalTime::fromInteger(5));
        (void)apply<SetNodeMuted>(fixture, kLayerStackNodeId, true);
        const auto result = apply<RemoveNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId});
        test.expect(result.changed() && result.renderAffecting && !result.affectedTimes.has_value(),
                    "deleting the first layer of a muted layer stack is whole-render");
    }
    {
        // The same clipped delete without the mute keeps the finite half-open span proof.
        Fixture fixture;
        (void)apply<SetLayerRange>(fixture, kFirstLayerId, core::RationalTime{},
                                   core::RationalTime::fromInteger(5));
        const auto result = apply<RemoveNodes>(fixture, std::set<NodeId>{kFirstLayerNodeId});
        expectSpan(result.affectedTimes, 0, 5,
                   "an unmuted clipped first-layer delete stays finite");
    }
    {
        Fixture fixture;
        const auto before = fixture.document.snapshot();
        const auto history = fixture.stack.size();
        Transaction transaction("Atomic refusal");
        transaction.emplace<SetProjectName>("Must roll back");
        transaction.emplace<RemoveNodes>(kCompositionId, std::set<NodeId>{NodeId::fromRaw(9999)});
        const auto result = fixture.stack.execute(std::move(transaction));
        test.expect(result.status == CommandStatus::Rejected && !result.affectedTimes.has_value() &&
                        fixture.document.snapshot().revision() == before.revision() &&
                        fixture.stack.size() == history,
                    "a rejected removal publishes no reuse evidence");
    }
}

void testParentCommands(TestContext& test) {
    Fixture mixed;
    test.expect(apply<ConnectPorts>(mixed, OutputPortRef{kFirstLayerNodeId, "image"},
                                    InputPortRef{NodeInputRef{kSecondLayerNodeId, "image"}})
                    .changed(),
                "mixed cycle fixture");
    refuse<SetLayerParent>(test, mixed, OperationIssueCode::GraphCycle, kFirstLayerId,
                           kSecondLayerId);
    Fixture fixture;
    const auto parent = [&] {
        return composition(fixture.document.snapshot()).graph().findLayer(kFirstLayerId)->parent;
    };
    (void)exercise<SetLayerParent>(test, fixture, kFirstLayerId, kSecondLayerId);
    test.expect(parent() == kSecondLayerId, "set parent survives undo and redo");
    test.expect(apply<SetLayerParent>(fixture, kFirstLayerId, kSecondLayerId).status ==
                    CommandStatus::NoChange,
                "same parent is a no-op");
    refuse<SetLayerParent>(test, fixture, OperationIssueCode::InvalidValue, kSecondLayerId,
                           kFirstLayerId);
    refuse<SetLayerParent>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId,
                           kFirstLayerId);
    refuse<SetLayerParent>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId,
                           LayerId::fromRaw(999));
    refuse<SetLayerParent>(test, fixture, OperationIssueCode::InvalidValue, LayerId::fromRaw(999),
                           std::nullopt);
    const auto single =
        exercise<DuplicateNodes>(test, fixture, std::set<NodeId>{kFirstLayerNodeId}, Vec2d{});
    const auto singleId = single.outputId<LayerId>("layer.30");
    test.expect(singleId &&
                    !composition(fixture.document.snapshot()).graph().findLayer(*singleId)->parent,
                "duplicating child alone clears parent");
    const auto pair = exercise<DuplicateNodes>(
        test, fixture, std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId}, Vec2d{});
    const auto childId = pair.outputId<LayerId>("layer.30");
    const auto parentId = pair.outputId<LayerId>("layer.31");
    test.expect(childId && parentId &&
                    composition(fixture.document.snapshot()).graph().findLayer(*childId)->parent ==
                        parentId,
                "duplicating pair remaps parent");
    (void)exercise<RemoveNodes>(test, fixture, std::set<NodeId>{kSecondLayerNodeId});
    test.expect(!parent(), "deleting parent clears child link and undoes atomically");
    (void)exercise<SetLayerParent>(test, fixture, kFirstLayerId, parentId);
    (void)exercise<SetLayerParent>(test, fixture, kFirstLayerId, std::nullopt);
    test.expect(!parent(), "unparent command survives undo and redo");
}

// #578: a refusal names what failed and on which object. Each message carries the id that was
// addressed and the place it was looked up, so "target does not exist" cannot come back.
template <typename Op, typename... Args>
std::string refusalMessage(Fixture& fixture, Args&&... args) {
    Transaction transaction("Message probe");
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    const auto result = fixture.stack.execute(std::move(transaction));
    return result.operationFailures.empty() ? std::string("(not refused)")
                                            : result.operationFailures.front().issue.message;
}

void expectNames(TestContext& test, const std::string& message,
                 const std::initializer_list<std::string_view> parts, const std::string_view what) {
    for (const auto part : parts)
        test.expect(message.find(part) != std::string::npos,
                    std::string(what) + " names \"" + std::string(part) + "\": " + message);
}

void testRefusalMessagesNameTheObject(TestContext& test) {
    Fixture fixture;
    const auto comp = std::to_string(kCompositionId.value());
    const auto missingNode = NodeId::fromRaw(9999);
    const auto missingGroup = NodeGroupId::fromRaw(8888);
    const auto missingSlot = LayerSlotId::fromRaw(7777);

    expectNames(test, refusalMessage<GroupNodes>(fixture, std::set<NodeId>{missingNode}),
                {"Node 9999", "composition " + comp}, "grouping a missing node");
    expectNames(test, refusalMessage<DuplicateNodes>(fixture, std::set<NodeId>{missingNode}, Vec2d{}),
                {"Node 9999", "composition " + comp}, "duplicating a missing node");
    expectNames(test, refusalMessage<UngroupNodes>(fixture, missingGroup),
                {"Node group 8888", "composition " + comp}, "ungrouping a missing group");
    expectNames(test,
                refusalMessage<SetMergeEnabled>(fixture, missingNode, false),
                {"Merge node 9999", "composition " + comp}, "toggling a missing Merge");
    expectNames(test, refusalMessage<ReorderMergeInput>(fixture, kLayerStackNodeId, missingSlot, 0),
                {"Slot 7777", "Merge node " + std::to_string(kLayerStackNodeId.value())},
                "reordering a missing slot");
    expectNames(test,
                refusalMessage<DisconnectInput>(
                    fixture, document::InputPortRef{document::NodeInputRef{missingNode, "in"}}),
                {"input \"in\" of node 9999", "composition " + comp},
                "disconnecting a missing input");
    expectNames(test,
                refusalMessage<ConnectPorts>(
                    fixture, document::OutputPortRef{missingNode, "out"},
                    document::InputPortRef{document::NodeInputRef{kFirstLayerNodeId, "in"}}),
                {"Output \"out\" of node 9999", "composition " + comp},
                "connecting from a missing output");
    expectNames(test,
                refusalMessage<SetNodeParameterValue>(fixture, missingNode, "x",
                                                      document::ParameterValue{1.0}),
                {"Node 9999", "composition " + comp}, "setting a missing node's parameter");
    expectNames(test,
                refusalMessage<SetNodeParameterValue>(fixture, kFirstLayerNodeId, "nope",
                                                      document::ParameterValue{1.0}),
                {"Node " + std::to_string(kFirstLayerNodeId.value()), "\"nope\""},
                "setting an unknown optional parameter");
    expectNames(test, refusalMessage<RemoveNodes>(fixture, std::set<NodeId>{missingNode}),
                {"Node 9999", "composition " + comp}, "removing a missing node");
    expectNames(test, refusalMessage<DissolveNode>(fixture, missingNode),
                {"Node 9999", "composition " + comp}, "dissolving a missing node");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testParentCommands(test);
        bloom::commands::test::testMultipleMergeCommands(test);
        bloom::commands::test::testMergeSlotSharesContentAndAudioRoles(test);
        bloom::commands::test::testLayerToggles(test);
        bloom::commands::test::testLayerRanges(test);
        bloom::commands::test::testValidityQuery(test);
        bloom::commands::test::testAddAndLayout(test);
        bloom::commands::test::testResizeFitParameters(test);
        bloom::commands::test::testTransform2DParameters(test);
        bloom::commands::test::testMatteBlurCommands(test);
        bloom::commands::test::testMatteBlurCommands(test, "bloom.matte-erode");
        bloom::commands::test::testMatteBlurCommands(test, "bloom.matte-dilate");
        bloom::commands::test::testLumaKeyCommands(test);
        bloom::commands::test::testRecipeCommands(test);
        bloom::commands::test::testColorGradeCommands(test);
        bloom::commands::test::testImageChannelCommands(test);
        bloom::commands::test::testNewNodePlacementDoesNotMoveExistingCards(test);
        bloom::commands::test::testWiringAndRename(test);
        bloom::commands::test::testRemoveAndDissolve(test);
        bloom::commands::test::testDeleteOutputAndItsMerge(test);
        bloom::commands::test::testDissolveParticipatingLayer(test);
        bloom::commands::test::testDeepDuplication(test);
        bloom::commands::test::testDuplicationOwnershipEdges(test);
        bloom::commands::test::testParameterSocketDrivers(test);
        bloom::commands::test::testFilmGrainCardEstimate(test);
        bloom::commands::test::testNodeGroups(test);
        bloom::commands::test::testRenderAffectingClassification(test);
        bloom::commands::test::testLayerRangeTimeFootprint(test);
        bloom::commands::test::testRemovalTimeFootprint(test);
        bloom::commands::test::testSplitEquivalenceEvidence(test);
        bloom::commands::test::testRefusalMessagesNameTheObject(test);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return test.failures() == 0 ? 0 : 1;
}
