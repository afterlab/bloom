// Issue #744: composition content is shared per record. An edit copies the records it touches,
// publication freezes, charges and indexes only those, and validation of a draft checks only the
// records it does not share with the snapshot it was drafted from.
#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/render_reachability.hpp>
#include <bloom/document/value_nodes.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
using namespace bloom::document;
using bloom::core::RationalTime;

int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::logic_error(message);
}

constexpr std::uint64_t kCurves = 64;
constexpr std::uint64_t kKeysPerCurve = 50;
constexpr std::uint64_t kParameterBase = 10'000;
constexpr std::uint64_t kCurveBase = 20'000;
constexpr std::uint64_t kKeyBase = 100'000;

[[nodiscard]] ScalarKeyframe key(const std::uint64_t id, const std::int64_t frame,
                                 const double value) {
    return {KeyframeId::fromRaw(id), RationalTime::fromInteger(frame), value};
}

// Output <- Merge, `fillers` unconnected reroutes, and kCurves rotation parameters, each animated
// by a scalar curve of kKeysPerCurve keys.
[[nodiscard]] Composition makeComposition(const std::uint64_t base, const std::size_t fillers) {
    const auto stack = NodeId::fromRaw(base + 1);
    const auto output = NodeId::fromRaw(base + 2);
    CanonicalGraph graph(stack);
    require(
        graph.addNode({stack, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}),
        "stack");
    require(graph.addNode({output,
                           std::string(kCompositionOutputNodeType),
                           {},
                           kCompositionOutputNodeSchemaVersion}),
            "output");
    require(graph.addEdge({EdgeId::fromRaw(base + 1),
                           {stack, std::string(kLayerStackOutputPort)},
                           NodeInputRef{output, std::string(kCompositionOutputInputPort)}}),
            "edge");
    graph.setCompositionOutput({output, std::string(kCompositionOutputOutputPort)});
    for (std::size_t index = 0; index < fillers; ++index)
        require(graph.addNode({NodeId::fromRaw(base + 100 + index),
                               std::string(kRerouteNodeType),
                               {},
                               kValueNodeSchemaVersion}),
                "filler");
    Composition composition(CompositionId::fromRaw(base), "Comp " + std::to_string(base),
                            RationalTime::fromInteger(100), std::move(graph));
    for (std::uint64_t curve = 0; curve < kCurves; ++curve) {
        const auto curveId = AnimationCurveId::fromRaw(base + kCurveBase + curve);
        require(composition.parameters().insert(
                    {ParameterId::fromRaw(base + kParameterBase + curve),
                     std::string(kRotationParameterSchemaKey), AnimationCurveSource{curveId, 0.0}}),
                "parameter");
        std::vector<ScalarKeyframe> keys;
        for (std::uint64_t index = 0; index < kKeysPerCurve; ++index)
            keys.push_back(key(base + kKeyBase + curve * kKeysPerCurve + index,
                               static_cast<std::int64_t>(index), static_cast<double>(index)));
        require(
            composition.animationCurves().insert(ScalarAnimationCurve{curveId, std::move(keys)}),
            "curve");
    }
    return composition;
}

[[nodiscard]] Project makeProject(const std::size_t compositions) {
    Project project(ProjectId::fromRaw(1), "Shared records");
    for (std::size_t index = 0; index < compositions; ++index)
        require(project.addComposition(makeComposition(1'000'000 * (index + 1), 8)), "composition");
    require(project.validate().ok(), "fixture validates");
    return project;
}

[[nodiscard]] CompositionId firstId() { return CompositionId::fromRaw(1'000'000); }
[[nodiscard]] AnimationCurveId curveOf(const std::uint64_t curve) {
    return AnimationCurveId::fromRaw(1'000'000 + kCurveBase + curve);
}
[[nodiscard]] ParameterId parameterOf(const std::uint64_t curve) {
    return ParameterId::fromRaw(1'000'000 + kParameterBase + curve);
}

// The identities of every record a composition holds.
[[nodiscard]] std::unordered_set<const void*> recordIdentities(const Composition& composition) {
    std::unordered_set<const void*> identities;
    composition.forEachRecord(
        [&](const void* identity, const auto&) { identities.insert(identity); });
    return identities;
}

[[nodiscard]] std::size_t fullCharge(const Composition& composition) {
    std::size_t bytes = composition.contentShellByteCharge();
    composition.forEachRecord(
        [&](const void*, const auto& record) { bytes += recordByteCharge(record); });
    return bytes;
}

// Adds one key to curve `curve` of the first composition and commits.
Snapshot addOneKey(Document& document, const std::uint64_t curve, const std::int64_t frame) {
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    auto* composition = draft.project().findComposition(firstId());
    const auto id = draft.ids().allocateKeyframe();
    require(id.has_value(), "allocate key");
    require(
        composition->animationCurves().insertKeyframe(curveOf(curve), key(id->value(), frame, 0.5)),
        "insert key");
    const auto result = document.commit(snapshot.revision(), std::move(draft));
    require(result.committed() && result.snapshot.has_value(), "commit one key");
    return *result.snapshot;
}

void testOneKeyEditCopiesOnlyItsCurve() {
    Document document(makeProject(1));
    const auto before = document.snapshot();
    const auto work = Composition::copyWork();
    const auto after = addOneKey(document, 7, 1000);
    const auto& was = before.project().compositions().front();
    const auto& now = after.project().compositions().front();

    expect(was.contentIdentity() != now.contentIdentity(), "the edited content is new");
    const auto wasRecords = recordIdentities(was);
    const auto nowRecords = recordIdentities(now);
    std::size_t fresh = 0;
    for (const auto* identity : nowRecords)
        fresh += wasRecords.contains(identity) ? 0 : 1;
    expect(fresh == 1, "a one-key edit holds one new record: the edited curve");
    const auto* oldCurve = was.animationCurves().find(curveOf(7));
    const auto* newCurve = now.animationCurves().find(curveOf(7));
    expect(oldCurve != newCurve && !nowRecords.contains(oldCurve),
           "the edited curve is a copy, the published one is untouched");
    expect(was.animationCurves().find(curveOf(8)) == now.animationCurves().find(curveOf(8)),
           "an untouched curve is the same allocation in both snapshots");
    expect(was.parameters().storageIdentity() == now.parameters().storageIdentity() &&
               was.graph().nodeStorageIdentity() == now.graph().nodeStorageIdentity(),
           "untouched record lists are shared whole");

    const auto& wasKeys = was.animationCurves().keyframeIds();
    const auto& nowKeys = now.animationCurves().keyframeIds();
    expect(nowKeys.size() == wasKeys.size() + 1 &&
               nowKeys.contains(KeyframeId::fromRaw(after.ids().highWater().keyframe)),
           "the keyframe-id index holds the new key");
    expect(nowKeys.sharedChunks(wasKeys) + 1 >= nowKeys.chunkCount(),
           "the keyframe-id index copies at most the one chunk the new id falls in");

    const auto done = Composition::copyWork();
    expect(done.incrementalFreezes - work.incrementalFreezes == 1 &&
               done.lockIndexReuses - work.lockIndexReuses == 1 &&
               done.reachabilityReuses - work.reachabilityReuses == 1,
           "publication reuses the lock index and reachability a key edit cannot change");
    expect(done.recordsValidated - work.recordsValidated == 1,
           "validation checks the one changed curve");
    expect(now.contentByteCharge() == fullCharge(now) && was.contentByteCharge() == fullCharge(was),
           "the incremental content charge equals the whole recount");
    expect(now.contentByteCharge() > was.contentByteCharge(), "the new key is charged");
}

void testRetainedDraftReferenceCannotReachSnapshot() {
    Document document(makeProject(1));
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    auto* composition = draft.project().findComposition(firstId());
    auto* node = composition->graph().findNode(NodeId::fromRaw(1'000'000 + 100));
    require(node != nullptr, "filler node");
    node->schemaVersion = kValueNodeSchemaVersion;
    const auto result = document.commit(snapshot.revision(), std::move(draft));
    require(result.committed(), "commit");
    // The reference the draft handed out still points into the draft's own record.
    node->typeId = "bloom.test.mutated-after-commit";
    const auto* published = result.snapshot->project().findComposition(firstId())->graph().findNode(
        NodeId::fromRaw(1'000'000 + 100));
    expect(published != nullptr && published != node &&
               published->typeId == std::string(kRerouteNodeType),
           "a record reference kept from the draft cannot change the published snapshot");
}

// Commits `edit`, reports whether the document accepted it, and checks that the incremental proof
// and the whole validation agree on the draft.
template <typename Edit> bool commitEdit(Document& document, Edit&& edit) {
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    edit(draft);
    const auto& base = *snapshot.project().findComposition(firstId());
    const auto& drafted = *std::as_const(draft).project().findComposition(firstId());
    const bool proven = drafted.validatesAgainst(base);
    const bool valid = drafted.validate().ok();
    expect(proven == valid, "the incremental proof agrees with the whole validation");
    return document.commit(snapshot.revision(), std::move(draft)).committed();
}

void testIncrementalValidationRefusesWhatWholeValidationRefuses() {
    Document document(makeProject(2));
    // A key outside its parameter's domain: opacity keys are confined to [0, 1].
    expect(!commitEdit(document,
                       [](Draft& draft) {
                           auto& composition = *draft.project().findComposition(firstId());
                           const auto opacity = ParameterId::fromRaw(1'000'000 + 9'000);
                           const auto curve = AnimationCurveId::fromRaw(1'000'000 + 9'000);
                           require(composition.parameters().insert(
                                       {opacity, std::string(kOpacityParameterSchemaKey),
                                        AnimationCurveSource{curve, 1.0}}),
                                   "opacity");
                           require(composition.animationCurves().insert(ScalarAnimationCurve{
                                       curve, {key(1'000'000 + 900'000, 0, 2.0)}}),
                                   "curve");
                       }),
           "a key outside its schema's domain is refused");
    // A parameter that names a curve that does not exist.
    expect(!commitEdit(document,
                       [](Draft& draft) {
                           require(
                               draft.project().findComposition(firstId())->parameters().setSource(
                                   parameterOf(3),
                                   AnimationCurveSource{AnimationCurveId::fromRaw(424242), 0.0}),
                               "set source");
                       }),
           "a parameter naming a missing curve is refused");
    // A curve still named by an unchanged parameter is removed.
    expect(!commitEdit(document,
                       [](Draft& draft) {
                           require(
                               draft.project().findComposition(firstId())->animationCurves().erase(
                                   curveOf(4)),
                               "erase");
                       }),
           "removing a curve an unchanged parameter names is refused");
    // A parameter returned to a constant leaves its curve without an owner.
    expect(!commitEdit(document,
                       [](Draft& draft) {
                           require(
                               draft.project().findComposition(firstId())->parameters().setSource(
                                   parameterOf(5), ConstantValueSource{0.0}),
                               "constant");
                       }),
           "a curve left without its owner is refused");
    // A key id another composition declares.
    expect(!commitEdit(
               document,
               [](Draft& draft) {
                   require(
                       draft.project().findComposition(firstId())->animationCurves().insertKeyframe(
                           curveOf(6), key(2'000'000 + kKeyBase, 999, 0.0)),
                       "foreign key");
               }),
           "a key id held by another composition is refused");
    // A layer range is checked against a changed duration.
    expect(commitEdit(document,
                      [](Draft& draft) {
                          require(draft.project().findComposition(firstId())->setDuration(
                                      RationalTime::fromInteger(50)),
                                  "duration");
                      }),
           "a valid duration edit is accepted");
    // Valid edits: a parameter returned to a constant with its curve, a moved key, a new curve.
    expect(commitEdit(document,
                      [](Draft& draft) {
                          auto& composition = *draft.project().findComposition(firstId());
                          require(composition.parameters().setSource(parameterOf(5),
                                                                     ConstantValueSource{0.0}),
                                  "constant");
                          require(composition.animationCurves().erase(curveOf(5)), "erase");
                      }),
           "a parameter returned to a constant with its curve removed is accepted");
    expect(commitEdit(document,
                      [](Draft& draft) {
                          auto& composition = *draft.project().findComposition(firstId());
                          const auto curve = AnimationCurveId::fromRaw(1'000'000 + 9'001);
                          require(composition.parameters().setSource(
                                      parameterOf(5), AnimationCurveSource{curve, 0.0}),
                                  "animate");
                          require(composition.animationCurves().insert(ScalarAnimationCurve{
                                      curve, {key(1'000'000 + 900'001, 0, 3.0)}}),
                                  "curve");
                      }),
           "a constant parameter animated with a new curve is accepted");
    const auto& published = document.snapshot().project();
    expect(published.validate().ok(), "every accepted edit leaves the project valid");
}

void testDeclaredIdIndexStaysExact() {
    Document document(makeProject(2));
    for (std::uint64_t edit = 0; edit < 20; ++edit)
        static_cast<void>(
            addOneKey(document, edit % kCurves, 2000 + static_cast<std::int64_t>(edit)));
    // A new composition built from scratch next to the edited one must agree on every id.
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    auto* composition = draft.project().findComposition(firstId());
    const auto taken = composition->animationCurves().keyframeIds().size();
    expect(taken == kCurves * kKeysPerCurve + 20, "every key edit's id is indexed");
    // Re-using an id the edits allocated in another composition is caught by the published index.
    auto* other = draft.project().findComposition(CompositionId::fromRaw(2'000'000));
    require(
        other->animationCurves().insertKeyframe(AnimationCurveId::fromRaw(2'000'000 + kCurveBase),
                                                key(snapshot.ids().highWater().keyframe, 500, 0.0)),
        "foreign insert");
    expect(!document.commit(snapshot.revision(), std::move(draft)).committed(),
           "an id an earlier edit declared is still held project-wide");
}

void testUndoRestoresExactRecords() {
    Document document(makeProject(1));
    std::vector<Snapshot> states{document.snapshot()};
    for (std::uint64_t edit = 0; edit < 12; ++edit)
        states.push_back(addOneKey(document, edit % 3, 3000 + static_cast<std::int64_t>(edit)));
    // Restoring an older state brings back exactly its records; a new edit drafted from the
    // restored state shares them and validates incrementally.
    const auto restored = document.restore(document.snapshot().revision(), states[4]);
    require(restored.committed(), "restore");
    const auto& now = restored.snapshot->project().compositions().front();
    const auto& then = states[4].project().compositions().front();
    expect(now.contentIdentity() == then.contentIdentity(),
           "a restored state shares its content with the history state");
    const auto branched = addOneKey(document, 9, 4000);
    const auto& branch = branched.project().compositions().front();
    expect(branch.animationCurves().find(curveOf(0)) == then.animationCurves().find(curveOf(0)),
           "a new edit after undo shares the restored records");
    std::size_t keys = 0;
    for (const auto& record : branch.animationCurves().records())
        keys += std::get<ScalarAnimationCurve>(record).keyframes.size();
    expect(keys == branch.animationCurves().keyframeIds().size() &&
               keys == kCurves * kKeysPerCurve + 4 + 1,
           "the branch holds the restored keys and its own, indexed exactly");
    expect(branched.project().validate().ok(), "the branch validates whole");
}

} // namespace

int main() {
    try {
        testOneKeyEditCopiesOnlyItsCurve();
        testRetainedDraftReferenceCannotReachSnapshot();
        testIncrementalValidationRefusesWhatWholeValidationRefuses();
        testDeclaredIdIndexStaysExact();
        testUndoRestoresExactRecords();
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected exception: " << exception.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
