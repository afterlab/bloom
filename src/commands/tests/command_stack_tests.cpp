#include "command_test_support.hpp"

#include <bloom/commands/animation_operations.hpp>

#include <algorithm>
#include <cstddef>
#include <future>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bloom::commands::test {
namespace {

class BreakProject final : public Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.break-project";
    }

    [[nodiscard]] OperationResult apply(document::Draft& draft) const override {
        auto& graph = draft.project().findComposition(kCompositionId)->graph();
        graph.setCompositionOutput({NodeId::fromRaw(9999), "image"});
        return OperationResult::applied();
    }
};

class ExhaustNodeIds final : public Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.exhaust-node-ids";
    }

    [[nodiscard]] OperationResult apply(document::Draft& draft) const override {
        draft.ids().reserveExisting(NodeId::fromRaw(std::numeric_limits<std::uint64_t>::max()));
        draft.project().setName("Exhausted");
        return OperationResult::applied();
    }
};

class ReplaceExtensionPayload final : public Operation {
  public:
    explicit ReplaceExtensionPayload(document::OpaqueExtensionPayload payload)
        : payload_(std::move(payload)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.replace-extension-payload";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override {
        auto record = draft.project().extensionRecords().front();
        if (record.payload == payload_)
            return OperationResult::noChange();
        if (!draft.project().removeExtensionRecord(record.id))
            return OperationResult::rejected(OperationIssueCode::InvalidValue, "Missing record");
        record.payload = payload_;
        if (!draft.project().addExtensionRecord(std::move(record)))
            return OperationResult::rejected(OperationIssueCode::InvalidValue, "Invalid record");
        return OperationResult::applied();
    }

  private:
    document::OpaqueExtensionPayload payload_;
};

// A deliberately unclassified operation: it leaves renderAffecting() at its default, so it proves
// the conservative CommandStack behavior without depending on any node command class.
class UnclassifiedRename final : public Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.unclassified-rename";
    }

    [[nodiscard]] OperationResult apply(document::Draft& draft) const override {
        draft.project().setName("Unclassified");
        return OperationResult::applied();
    }
};

// #399: changes an evaluation input (the first layer's opacity) while declaring `neutral`.
class FadeOp final : public Operation {
  public:
    explicit FadeOp(const double opacity, const bool declaredNeutral = false)
        : opacity_(opacity), declaredNeutral_(declaredNeutral) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.test.fade"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override {
        auto& parameters = draft.project().findComposition(kCompositionId)->parameters();
        return parameters.setSource(kOpacityId, ConstantValueSource{opacity_})
                   ? OperationResult::applied()
                   : OperationResult::rejected(OperationIssueCode::InvalidValue, "No opacity");
    }
    [[nodiscard]] bool renderAffecting() const noexcept override { return !declaredNeutral_; }

  private:
    double opacity_;
    bool declaredNeutral_;
};

// TEMPORAL-1: an operation that proves a finite footprint but does not change the document (so it
// can be layered many times in one transaction) without depending on a real layer command.
class FootprintOp final : public Operation {
  public:
    FootprintOp(AffectedTimeFootprint footprint, bool renderAffecting = true)
        : footprint_(std::move(footprint)), renderAffecting_(renderAffecting) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.footprint";
    }
    [[nodiscard]] OperationResult apply(document::Draft&) const override {
        auto result = OperationResult::applied();
        result.affectedTimes = footprint_;
        return result;
    }
    [[nodiscard]] bool renderAffecting() const noexcept override { return renderAffecting_; }

  private:
    AffectedTimeFootprint footprint_;
    bool renderAffecting_;
};

// Applies (or reports no change) while reporting one non-fatal issue, like a partial import.
class WarningOp final : public Operation {
  public:
    explicit WarningOp(std::string name) : name_(std::move(name)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.test.warning"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override {
        auto result = OperationResult::noChange();
        if (!name_.empty()) {
            draft.project().setName(name_);
            result = OperationResult::applied();
        }
        result.issues.push_back({OperationIssueCode::InvalidValue, "skipped.png: unreadable"});
        return result;
    }

  private:
    std::string name_;
};

// A render-neutral operation (renderAffecting false) that also carries no footprint.
class NeutralOp final : public Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.test.neutral"; }
    [[nodiscard]] OperationResult apply(document::Draft&) const override {
        return OperationResult::applied();
    }
    [[nodiscard]] bool renderAffecting() const noexcept override { return false; }
};

// SPLIT-1: a finite footprint plus ordered remaps, to exercise CommandStack aggregation, inversion,
// and forward replay without depending on a real layer command.
class RemapOp final : public Operation {
  public:
    RemapOp(AffectedTimeFootprint footprint, std::vector<LayerIdentityRemap> remaps)
        : footprint_(std::move(footprint)), remaps_(std::move(remaps)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.test.remap"; }
    [[nodiscard]] OperationResult apply(document::Draft&) const override {
        auto result = OperationResult::applied();
        result.affectedTimes = footprint_;
        result.layerIdentityRemaps = remaps_;
        return result;
    }

  private:
    AffectedTimeFootprint footprint_;
    std::vector<LayerIdentityRemap> remaps_;
};

[[nodiscard]] LayerIdentityRemap remap(const std::uint64_t before, const std::uint64_t after) {
    return LayerIdentityRemap{.compositionId = kCompositionId,
                              .start = core::RationalTime::fromInteger(1),
                              .end = core::RationalTime::fromInteger(4),
                              .beforeLayerId = LayerId::fromRaw(before),
                              .afterLayerId = LayerId::fromRaw(after),
                              .beforeNodeId = NodeId::fromRaw(before + 100),
                              .afterNodeId = NodeId::fromRaw(after + 100)};
}

[[nodiscard]] AffectedTimeFootprint
footprint(const std::vector<std::pair<core::RationalTime, core::RationalTime>>& spans) {
    AffectedTimeFootprint result;
    result.compositionId = kCompositionId;
    for (const auto& [start, end] : spans)
        result.intervals.push_back({start, end});
    return result;
}

void testLayerIdentityRemapAggregation(TestContext& test) {
    const auto t = [](const std::int64_t value) { return core::RationalTime::fromInteger(value); };

    // normalize: malformed descriptor, mixed composition, and cap overflow all fail closed.
    {
        auto malformed = remap(30, 31);
        malformed.start = t(4);
        malformed.end = t(1);
        test.expect(!normalizeLayerIdentityRemaps({malformed}).has_value(),
                    "an inverted remap range is not provable");
    }
    {
        auto foreign = remap(30, 31);
        foreign.compositionId = CompositionId::fromRaw(999);
        test.expect(!normalizeLayerIdentityRemaps({remap(30, 31), foreign}).has_value(),
                    "a mixed-composition remap list is not provable");
    }
    {
        std::vector<LayerIdentityRemap> many;
        many.reserve(10);
        for (std::uint64_t index = 0; index < 10; ++index)
            many.push_back(remap(30 + index * 2, 31 + index * 2));
        test.expect(!normalizeLayerIdentityRemaps(many).has_value(),
                    "remap cap overflow falls back to whole render");
    }

    Document document(makeProject());
    CommandStack stack(document);
    // Forward aggregation preserves order; undo reverses and swaps; redo replays forward.
    {
        Transaction transaction("Two ordered remaps", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(30, 31)});
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(31, 32)});
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.affectedTimes.has_value() &&
                        result.layerIdentityRemaps.has_value() &&
                        result.layerIdentityRemaps->size() == 2 &&
                        result.layerIdentityRemaps->at(0).beforeLayerId == LayerId::fromRaw(30) &&
                        result.layerIdentityRemaps->at(1).beforeLayerId == LayerId::fromRaw(31),
                    "forward execute preserves remap order");
        const auto undo = stack.undo();
        // Forward [30->31, 31->32] inverted: reverse order then swap each => [32->31, 31->30].
        test.expect(undo.changed() && undo.layerIdentityRemaps.has_value() &&
                        undo.layerIdentityRemaps->size() == 2 &&
                        undo.layerIdentityRemaps->at(0).beforeLayerId == LayerId::fromRaw(32) &&
                        undo.layerIdentityRemaps->at(0).afterLayerId == LayerId::fromRaw(31) &&
                        undo.layerIdentityRemaps->at(1).beforeLayerId == LayerId::fromRaw(31) &&
                        undo.layerIdentityRemaps->at(1).afterLayerId == LayerId::fromRaw(30),
                    "undo reverses order and swaps before/after IDs");
        const auto redo = stack.redo();
        test.expect(redo.changed() && redo.layerIdentityRemaps.has_value() &&
                        redo.layerIdentityRemaps->at(0).beforeLayerId == LayerId::fromRaw(30) &&
                        redo.layerIdentityRemaps->at(1).beforeLayerId == LayerId::fromRaw(31),
                    "redo replays the stored forward ordered list");
    }
    // An unclassified op in the same transaction clears remaps and forces whole render.
    {
        Transaction transaction("Remap plus unclassified", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(40, 41)});
        transaction.emplace<SetProjectName>("Whole");
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && !result.affectedTimes.has_value() &&
                        !result.layerIdentityRemaps.has_value(),
                    "an unclassified op clears remaps and forces whole render");
    }
    // A neutral op beside a remap leaves the remap intact.
    {
        Transaction transaction("Remap plus neutral", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(50, 51)});
        transaction.emplace<NeutralOp>();
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.affectedTimes.has_value() &&
                        result.layerIdentityRemaps.has_value() &&
                        result.layerIdentityRemaps->size() == 1,
                    "a neutral op leaves a remap intact");
    }
    // A rejected transaction advertises no remap evidence.
    {
        Transaction transaction("Rejected remap", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(60, 61)});
        transaction.emplace<SetCompositionName>(CompositionId::fromRaw(9999), "Missing");
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.status == CommandStatus::Rejected &&
                        !result.layerIdentityRemaps.has_value(),
                    "a rejected transaction advertises no remap evidence");
    }
    // FAIL-CLOSED at the CommandStack level (not merely the normalize helper): any remap
    // normalization, composition, or cap failure must make the whole transaction whole-render and
    // publish NEITHER affectedTimes nor remaps, and undo/redo must stay whole too.
    const auto expectWhole = [&test](const CommandResult& result, const std::string_view message) {
        test.expect(result.changed() && result.renderAffecting &&
                        !result.affectedTimes.has_value() &&
                        !result.layerIdentityRemaps.has_value(),
                    message);
    };
    {
        // More than kMaxLayerIdentityRemaps remaps spread across applied ops.
        Transaction transaction("Cap overflow remaps", document.snapshot().revision());
        for (std::uint64_t index = 0; index < kMaxLayerIdentityRemaps + 1; ++index) {
            transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}),
                                         std::vector{remap(70 + index * 2, 71 + index * 2)});
        }
        const auto result = stack.execute(std::move(transaction));
        expectWhole(result, "a cap-overflow remap transaction is whole-render with no metadata");
        const auto undo = stack.undo();
        expectWhole(undo, "undo of a cap-overflow remap transaction stays whole-render");
        const auto redo = stack.redo();
        expectWhole(redo, "redo of a cap-overflow remap transaction stays whole-render");
    }
    {
        // A malformed descriptor (inverted range) inside an applied op.
        auto malformed = remap(90, 91);
        malformed.start = t(4);
        malformed.end = t(1);
        Transaction transaction("Malformed remap", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{malformed});
        const auto result = stack.execute(std::move(transaction));
        expectWhole(result, "a malformed remap descriptor is whole-render with no metadata");
    }
    {
        // Footprint composition A but remap composition B.
        auto foreign = remap(92, 93);
        foreign.compositionId = CompositionId::fromRaw(999);
        Transaction transaction("Footprint/remap composition mismatch",
                                document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{foreign});
        const auto result = stack.execute(std::move(transaction));
        expectWhole(result,
                    "a footprint/remap composition mismatch is whole-render with no metadata");
        const auto undo = stack.undo();
        expectWhole(undo, "undo of a composition-mismatch remap transaction stays whole-render");
        const auto redo = stack.redo();
        expectWhole(redo, "redo of a composition-mismatch remap transaction stays whole-render");
    }
}

void testOperationWarningsArePublished(TestContext& test) {
    document::Document document(makeProject());
    CommandStack stack(document);
    Transaction applied("Partial", document.snapshot().revision());
    applied.emplace<WarningOp>("Renamed");
    const auto result = stack.execute(std::move(applied));
    test.expect(result.status == CommandStatus::Succeeded && result.warnings.size() == 1 &&
                    result.warnings.front().operationIndex == 0 &&
                    result.warnings.front().operationType == "bloom.test.warning" &&
                    result.warnings.front().issue.message == "skipped.png: unreadable" &&
                    result.operationFailures.empty(),
                "an applied operation's issues are published as warnings");
    test.expect(stack.undo().warnings.empty(), "undo does not replay execution warnings");

    Transaction unchanged("Nothing imported", document.snapshot().revision());
    unchanged.emplace<WarningOp>("");
    const auto none = stack.execute(std::move(unchanged));
    test.expect(none.status == CommandStatus::NoChange && none.warnings.size() == 1,
                "a no-change operation's issues are still published as warnings");

    Transaction rejected("Rejected", document.snapshot().revision());
    rejected.emplace<WarningOp>("Renamed again");
    rejected.emplace<SetProjectName>("");
    const auto refused = stack.execute(std::move(rejected));
    test.expect(refused.status == CommandStatus::Rejected && refused.warnings.empty(),
                "a rejected transaction publishes failures, not warnings");
}

void testFiniteTimeFootprintAggregation(TestContext& test) {
    const auto t = [](const std::int64_t value) { return core::RationalTime::fromInteger(value); };

    // normalize: overlapping and exactly-adjacent intervals merge deterministically.
    {
        const auto normalized =
            normalizeAffectedTimeFootprint(footprint({{t(5), t(7)}, {t(1), t(3)}, {t(3), t(5)}}));
        test.expect(normalized.has_value() && normalized->intervals.size() == 1 &&
                        normalized->intervals.front().start == t(1) &&
                        normalized->intervals.front().end == t(7),
                    "overlapping/adjacent intervals normalize to one");
    }
    // An empty footprint is deliberate and stays empty.
    {
        const auto normalized = normalizeAffectedTimeFootprint(footprint({}));
        test.expect(normalized.has_value() && normalized->intervals.empty(),
                    "an explicitly empty footprint stays empty");
    }
    // An inverted interval is not provable and falls back to whole render.
    {
        test.expect(!normalizeAffectedTimeFootprint(footprint({{t(4), t(2)}})).has_value(),
                    "an inverted interval is not provable");
    }
    // A different composition fails the merge closed.
    {
        auto other = footprint({{t(1), t(2)}});
        other.compositionId = CompositionId::fromRaw(999);
        test.expect(!mergeAffectedTimeFootprints(footprint({{t(0), t(1)}}), other).has_value(),
                    "a mixed-composition union is not provable");
    }
    // Too many disjoint intervals exceed the cap and fall back to whole render.
    {
        std::vector<std::pair<core::RationalTime, core::RationalTime>> many;
        many.reserve(10);
        for (std::int64_t index = 0; index < 10; ++index)
            many.emplace_back(t(index * 4), t(index * 4 + 1));
        test.expect(!normalizeAffectedTimeFootprint(footprint(many)).has_value(),
                    "pathological interval growth falls back to whole render");
    }

    // CommandStack aggregation: two finite ops union and normalize; an unclassified op dominates.
    Document document(makeProject());
    CommandStack stack(document);
    {
        Transaction transaction("Two finite ops", document.snapshot().revision());
        transaction.emplace<FootprintOp>(footprint({{t(4), t(10)}}));
        transaction.emplace<FootprintOp>(footprint({{t(2), t(4)}}));
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.renderAffecting &&
                        result.affectedTimes.has_value() &&
                        result.affectedTimes->intervals.size() == 1 &&
                        result.affectedTimes->intervals.front().start == t(2) &&
                        result.affectedTimes->intervals.front().end == t(10),
                    "two finite ops union into one normalized interval");
        const auto undo = stack.undo();
        test.expect(undo.changed() && undo.affectedTimes.has_value() &&
                        undo.affectedTimes->intervals.front().start == t(2) &&
                        undo.affectedTimes->intervals.front().end == t(10),
                    "undo replays the aggregated finite footprint");
        const auto redo = stack.redo();
        test.expect(redo.changed() && redo.affectedTimes.has_value() &&
                        redo.affectedTimes->intervals.size() == 1,
                    "redo replays the aggregated finite footprint");
    }
    {
        Transaction transaction("Finite plus unclassified", document.snapshot().revision());
        transaction.emplace<FootprintOp>(footprint({{t(1), t(2)}}));
        transaction.emplace<SetProjectName>("Whole render");
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.renderAffecting && !result.affectedTimes.has_value(),
                    "a finite footprint beside an unclassified op falls back to whole render");
    }
    {
        Transaction transaction("Finite plus neutral", document.snapshot().revision());
        transaction.emplace<FootprintOp>(footprint({{t(1), t(2)}}));
        transaction.emplace<NeutralOp>();
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.renderAffecting &&
                        result.affectedTimes.has_value() &&
                        result.affectedTimes->intervals.size() == 1,
                    "a neutral op does not widen a finite footprint");
    }
    {
        // A rejected transaction never advertises reuse evidence.
        Transaction transaction("Rejected finite", document.snapshot().revision());
        transaction.emplace<FootprintOp>(footprint({{t(1), t(2)}}));
        transaction.emplace<SetCompositionName>(CompositionId::fromRaw(9999), "Missing");
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.status == CommandStatus::Rejected && !result.affectedTimes.has_value(),
                    "a rejected transaction advertises no footprint");
    }
}

void testRenderAffectingPublication(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    std::vector<CommandEventKind> events;
    (void)stack.addObserver([&events](const CommandEvent& event) { events.push_back(event.kind); });
    const auto published = [&events](const CommandEventKind kind) {
        return std::ranges::find(events, kind) != events.end();
    };

    events.clear();
    Transaction rejected("Rejected", document.snapshot().revision());
    rejected.emplace<SetCompositionName>(CompositionId::fromRaw(9999), "Missing");
    const auto rejectedResult = stack.execute(std::move(rejected));
    test.expect(rejectedResult.status == CommandStatus::Rejected &&
                    !published(CommandEventKind::RevisionChanged) && rejectedResult.renderAffecting,
                "a rejected transaction publishes no RevisionChanged and keeps the conservative "
                "render-affecting default");

    events.clear();
    Transaction rename("Rename", document.snapshot().revision());
    rename.emplace<SetProjectName>("Renamed");
    const auto renameResult = stack.execute(std::move(rename));
    test.expect(renameResult.changed() && published(CommandEventKind::RevisionChanged),
                "a successful unclassified transaction publishes RevisionChanged");
    test.expect(!renameResult.renderAffecting && !renameResult.affectedTimes.has_value(),
                "#399: a project rename is proven render-neutral without declaring it");

    events.clear();
    Transaction repeat("Repeat rename", document.snapshot().revision());
    repeat.emplace<SetProjectName>("Renamed");
    const auto repeatResult = stack.execute(std::move(repeat));
    test.expect(repeatResult.status == CommandStatus::NoChange &&
                    !published(CommandEventKind::RevisionChanged) && repeatResult.renderAffecting,
                "a no-change transaction publishes no RevisionChanged and keeps the conservative "
                "render-affecting default");

    events.clear();
    Transaction unclassified("Unclassified custom", document.snapshot().revision());
    unclassified.emplace<UnclassifiedRename>();
    const auto unclassifiedResult = stack.execute(std::move(unclassified));
    test.expect(unclassifiedResult.changed() && !unclassifiedResult.renderAffecting,
                "#399: an unclassified operation that changed only presentation is proven "
                "render-neutral");

    events.clear();
    Transaction fade("Unclassified fade", document.snapshot().revision());
    fade.emplace<FadeOp>(0.25);
    const auto fadeResult = stack.execute(std::move(fade));
    test.expect(fadeResult.changed() && fadeResult.renderAffecting,
                "an unclassified operation that changed an evaluation input is render-affecting");

    Transaction mislabelled("Mislabelled fade", document.snapshot().revision());
    mislabelled.emplace<FadeOp>(0.75, true);
    const auto mislabelledResult = stack.execute(std::move(mislabelled));
    test.expect(mislabelledResult.changed() && mislabelledResult.renderAffecting &&
                    !mislabelledResult.affectedTimes.has_value() &&
                    !mislabelledResult.affectedComposition.has_value(),
                "#399: a render-neutral declaration that changed an evaluation input fails "
                "closed to a whole render");
    const auto mislabelledUndo = stack.undo();
    test.expect(mislabelledUndo.changed() && mislabelledUndo.renderAffecting,
                "undo of a failed-closed declaration stays render-affecting");
    test.expect(stack.work().evaluationInputProofs >= 4,
                "every applied transaction ran the evaluation-input proof");
}

void testAtomicTransactionUndoAndRedo(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();

    Transaction transaction("Rename and fade", original.revision());
    transaction.emplace<SetProjectName>("Renamed Project");
    transaction.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                            document::ParameterSource{ConstantValueSource{0.5}});
    const auto execute = stack.execute(std::move(transaction));
    test.expect(execute.status == CommandStatus::Succeeded,
                "two-operation transaction should commit");
    test.expect(stack.size() == 1, "transaction should create exactly one history entry");
    test.expect(stack.canUndo(), "committed transaction should be undoable");
    test.expect(!stack.canRedo(), "new transaction should not create redo history");

    const auto changed = document.snapshot();
    test.expect(changed.revision() > original.revision(),
                "execute should advance the document revision");
    test.expect(changed.project().name() == "Renamed Project",
                "execute should publish the new project name");
    test.expect(std::abs(opacity(changed) - 0.5) < 0.0001,
                "execute should publish the new opacity");
    test.expect(original.project().name() == "Original Project",
                "original snapshot should remain immutable after execute");
    test.expect(std::abs(opacity(original) - 1.0) < 0.0001,
                "original snapshot should retain its exact parameter source");

    const auto undo = stack.undo();
    const auto restored = document.snapshot();
    test.expect(undo.status == CommandStatus::Succeeded, "undo should restore history");
    test.expect(restored.revision() > changed.revision(),
                "undo should publish at a new monotonic revision");
    test.expect(restored.project().name() == original.project().name(),
                "undo should restore the exact project name");
    test.expect(std::abs(opacity(restored) - opacity(original)) < 0.0001,
                "undo should restore the exact parameter source");
    test.expect(layerOrder(restored) == layerOrder(original),
                "undo should preserve exact stable layer order and IDs");
    test.expect(stack.canRedo(), "successful undo should make redo available");

    const auto redo = stack.redo();
    const auto redone = document.snapshot();
    test.expect(redo.status == CommandStatus::Succeeded, "redo should restore history");
    test.expect(redone.revision() > restored.revision(),
                "redo should publish at a new monotonic revision");
    test.expect(redone.project().name() == changed.project().name(),
                "redo should restore the exact changed project name");
    test.expect(std::abs(opacity(redone) - opacity(changed)) < 0.0001,
                "redo should restore the exact changed parameter source");
    test.expect(layerOrder(redone) == layerOrder(changed),
                "redo should preserve exact stable layer order and IDs");
}

void testRejectedAndInvalidTransactionsAreAtomic(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();

    Transaction rejected("Rejected", original.revision());
    rejected.emplace<SetProjectName>("Must Not Publish");
    rejected.emplace<SetCompositionName>(CompositionId::fromRaw(9999), "Missing");
    const auto rejectedResult = stack.execute(std::move(rejected));
    const auto afterRejected = document.snapshot();
    test.expect(rejectedResult.status == CommandStatus::Rejected,
                "invalid operation target should reject the whole transaction");
    test.expect(rejectedResult.operationFailures.size() == 1,
                "rejected transaction should identify one failing operation");
    if (!rejectedResult.operationFailures.empty()) {
        test.expect(rejectedResult.operationFailures.front().operationIndex == 1,
                    "failure should identify the second operation");
    }
    test.expect(afterRejected.revision() == original.revision(),
                "rejected transaction should not publish a revision");
    test.expect(afterRejected.project().name() == original.project().name(),
                "earlier draft mutations should not leak from a rejected transaction");
    test.expect(stack.size() == 0, "rejected transaction should not create history");

    Transaction invalid("Invalid draft", original.revision());
    invalid.emplace<SetProjectName>("Must Also Not Publish");
    invalid.emplace<BreakProject>();
    const auto invalidResult = stack.execute(std::move(invalid));
    const auto afterInvalid = document.snapshot();
    test.expect(invalidResult.status == CommandStatus::ValidationFailed,
                "invalid final draft should fail document validation");
    test.expect(!invalidResult.validation.ok(),
                "validation failure should carry structured document diagnostics");
    test.expect(afterInvalid.revision() == original.revision(),
                "invalid final draft should not publish a revision");
    test.expect(afterInvalid.project().name() == original.project().name(),
                "invalid final draft should preserve exact prior state");
    test.expect(stack.size() == 0, "invalid final draft should not create history");
}

void testKnownParameterSchemaRejectionsAreAtomic(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);

    Transaction valid("Set valid opacity", document.snapshot().revision());
    valid.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                      document::ParameterSource{ConstantValueSource{0.5}});
    test.expect(stack.execute(std::move(valid)).changed(),
                "known-schema rejection fixture should publish one valid edit");
    const auto baseline = document.snapshot();
    const auto baselineHistorySize = stack.size();

    Transaction wrongType("Reject wrong opacity type", baseline.revision());
    wrongType.emplace<SetParameterSource>(
        kCompositionId, kOpacityId,
        document::ParameterSource{ConstantValueSource{std::string("opaque")}});
    const auto wrongTypeResult = stack.execute(std::move(wrongType));
    test.expect(wrongTypeResult.status == CommandStatus::Rejected,
                "opacity command should reject a wrong-type constant");
    test.expect(document.snapshot().revision() == baseline.revision() &&
                    std::abs(opacity(document.snapshot()) - 0.5) < 0.0001 &&
                    stack.size() == baselineHistorySize && stack.canUndo() && !stack.canRedo(),
                "wrong-type rejection should preserve revision, value, and history cursor");

    Transaction wrongRange("Reject out-of-range opacity", baseline.revision());
    wrongRange.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                           document::ParameterSource{ConstantValueSource{1.5}});
    const auto wrongRangeResult = stack.execute(std::move(wrongRange));
    test.expect(wrongRangeResult.status == CommandStatus::Rejected,
                "opacity command should reject an out-of-range constant");
    test.expect(document.snapshot().revision() == baseline.revision() &&
                    std::abs(opacity(document.snapshot()) - 0.5) < 0.0001 &&
                    stack.size() == baselineHistorySize && stack.canUndo() && !stack.canRedo(),
                "range rejection should preserve revision, value, and history cursor");
    test.expect(stack.undo().changed() && std::abs(opacity(document.snapshot()) - 1.0) < 0.0001,
                "prior valid history should remain exactly undoable after rejections");
}

void testStaleExecuteUndoAndRedoAreNoOps(TestContext& test) {
    {
        Document document(makeProject());
        CommandStack stack(document);
        const auto stale = document.snapshot();
        auto externalDraft = document.draft(stale);
        externalDraft.project().setName("External");
        test.expect(document.commit(stale.revision(), std::move(externalDraft)).committed(),
                    "stale-execute setup commit should succeed");

        Transaction transaction("Stale execute", stale.revision());
        transaction.emplace<SetProjectName>("Must Not Publish");
        const auto result = stack.execute(std::move(transaction));
        const auto after = document.snapshot();
        test.expect(result.status == CommandStatus::StaleRevision,
                    "execute should reject an obsolete base revision");
        test.expect(result.expectedRevision == stale.revision(),
                    "stale execute should report the expected revision");
        test.expect(after.project().name() == "External",
                    "stale execute should preserve newer document state");
        test.expect(stack.size() == 0, "stale execute should not create history");
    }

    {
        Document document(makeProject());
        CommandStack stack(document);
        Transaction transaction("Tracked edit", document.snapshot().revision());
        transaction.emplace<SetProjectName>("Tracked");
        test.expect(stack.execute(std::move(transaction)).changed(),
                    "stale-undo setup command should commit");

        const auto beforeExternal = document.snapshot();
        auto externalDraft = document.draft(beforeExternal);
        externalDraft.project().findComposition(kCompositionId)->setName("External");
        test.expect(
            document.commit(beforeExternal.revision(), std::move(externalDraft)).committed(),
            "stale-undo setup external commit should succeed");
        const auto external = document.snapshot();

        const auto result = stack.undo();
        const auto after = document.snapshot();
        test.expect(result.status == CommandStatus::StaleRevision,
                    "undo should reject history after an external commit");
        test.expect(stack.canUndo(), "stale undo should retain its history cursor");
        test.expect(after.revision() == external.revision(),
                    "stale undo should not publish a revision");
        test.expect(after.project().name() == external.project().name(),
                    "stale undo should preserve the external project state");
        test.expect(composition(after).name() == "External",
                    "stale undo should preserve the external composition state");
    }

    {
        Document document(makeProject());
        CommandStack stack(document);
        Transaction transaction("Tracked edit", document.snapshot().revision());
        transaction.emplace<SetProjectName>("Tracked");
        test.expect(stack.execute(std::move(transaction)).changed(),
                    "stale-redo setup command should commit");
        test.expect(stack.undo().changed(), "stale-redo setup undo should commit");

        const auto beforeExternal = document.snapshot();
        auto externalDraft = document.draft(beforeExternal);
        externalDraft.project().findComposition(kCompositionId)->setName("External");
        test.expect(
            document.commit(beforeExternal.revision(), std::move(externalDraft)).committed(),
            "stale-redo setup external commit should succeed");
        const auto external = document.snapshot();

        const auto result = stack.redo();
        const auto after = document.snapshot();
        test.expect(result.status == CommandStatus::StaleRevision,
                    "redo should reject history after an external commit");
        test.expect(stack.canRedo(), "stale redo should retain its history cursor");
        test.expect(after.revision() == external.revision(),
                    "stale redo should not publish a revision");
        test.expect(after.project().name() == external.project().name(),
                    "stale redo should preserve the external project state");
        test.expect(composition(after).name() == "External",
                    "stale redo should preserve the external composition state");
    }
}

void testLayerOrderNoChangeAndRedoInvalidation(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();

    Transaction move("Move second layer", original.revision());
    move.emplace<MoveLayerBefore>(kCompositionId, kSecondSlotId, kFirstSlotId);
    test.expect(stack.execute(std::move(move)).changed(), "layer move should commit");
    test.expect(layerOrder(document.snapshot()) ==
                    std::vector<LayerSlotId>{kSecondSlotId, kFirstSlotId},
                "layer move should change only stable slot order");

    test.expect(stack.undo().changed(), "layer move should be undoable");
    test.expect(layerOrder(document.snapshot()) == layerOrder(original),
                "undo should restore exact stable slot order");
    test.expect(stack.canRedo(), "undo should make the layer move redoable");

    Transaction replacement("Replace redo", document.snapshot().revision());
    replacement.emplace<SetProjectName>("Replacement");
    test.expect(stack.execute(std::move(replacement)).changed(),
                "new edit after undo should commit");
    test.expect(!stack.canRedo(), "new edit after undo should invalidate redo history");
    test.expect(stack.size() == 1, "invalidated redo branch should be discarded");

    const auto beforeNoChange = document.snapshot();
    Transaction noChange("No change", beforeNoChange.revision());
    noChange.emplace<SetProjectName>("Replacement");
    const auto noChangeResult = stack.execute(std::move(noChange));
    test.expect(noChangeResult.status == CommandStatus::NoChange,
                "idempotent operation should produce a no-change result");
    test.expect(document.snapshot().revision() == beforeNoChange.revision(),
                "no-change transaction should not advance revision");
    test.expect(stack.size() == 1, "no-change transaction should not create history");
}

void testDiscardRedoForgetsWithdrawnEdits(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto chargedBefore = stack.historyChargedBytes();
    Transaction rename("Rename", document.snapshot().revision());
    rename.emplace<SetProjectName>("Withdrawn");
    test.expect(stack.execute(std::move(rename)).changed(), "the edit commits");
    test.expect(stack.undo().changed(), "the edit undoes");
    const auto undone = document.snapshot().revision();
    stack.discardRedo();
    test.expect(!stack.canRedo() && !stack.canUndo() && stack.size() == 0,
                "discarding redo forgets the withdrawn edit");
    test.expect(stack.historyChargedBytes() == chargedBefore,
                "the withdrawn entry no longer charges history bytes");
    test.expect(document.snapshot().revision() == undone,
                "discarding redo does not change the document");
    test.expect(stack.redo().status == CommandStatus::NothingToRedo,
                "redo cannot bring the withdrawn edit back");
    stack.discardRedo();
    test.expect(stack.size() == 0, "discarding with no redo is a no-op");
}

void testExhaustionSurvivesUndoAndRedo(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    Transaction exhaust("Exhaust node IDs", document.snapshot().revision());
    exhaust.emplace<ExhaustNodeIds>();
    test.expect(stack.execute(std::move(exhaust)).changed(),
                "allocator exhaustion fixture should publish");
    test.expect(stack.undo().changed(), "allocator exhaustion should be undoable");
    auto afterUndo = document.draft(document.snapshot());
    test.expect(!afterUndo.ids().allocateNode().has_value(),
                "undo must retain a previously published exhausted sentinel");
    test.expect(stack.redo().changed(), "allocator exhaustion should be redoable");
    auto afterRedo = document.draft(document.snapshot());
    test.expect(!afterRedo.ids().allocateNode().has_value(),
                "redo must retain the exhausted sentinel");
}

void testBoundedUndoHistoryAndByteAccounting(TestContext& test) {
    const auto rename = [](CommandStack& stack, Document& document, std::string name) {
        Transaction transaction("Rename", document.snapshot().revision());
        transaction.emplace<SetProjectName>(std::move(name));
        return stack.execute(std::move(transaction));
    };

    Document counted(makeProject());
    CommandStack countStack(counted);
    const auto original = counted.snapshot();
    const auto contentWorkBefore = document::Composition::copyWork();
    const auto first = rename(countStack, counted, "Project name 001");
    const auto afterFirst = counted.snapshot();
    const auto contentWorkAfter = document::Composition::copyWork();
    const auto rootCharge = original.project().rootByteCharge() + sizeof(document::IdAllocator);
    const auto afterRootCharge =
        afterFirst.project().rootByteCharge() + sizeof(document::IdAllocator);
    test.expect(
        first.changed() &&
            original.project().compositions().front().contentIdentity() ==
                afterFirst.project().compositions().front().contentIdentity() &&
            contentWorkAfter.contentChargeWalks == contentWorkBefore.contentChargeWalks &&
            countStack.historyChargedBytes() == rootCharge,
        "history excludes the active root and shared content without walking its graph again");

    auto extensionProject = makeProject();
    const document::OpaqueExtensionPayload sharedPayload(std::vector<std::byte>(1024));
    for (std::uint64_t id = 1; id <= 2; ++id) {
        const auto added = extensionProject.addExtensionRecord(
            {.id = document::ExtensionRecordId::fromRaw(id),
             .ownerId = "vendor.module",
             .typeId = "vendor.module.record-type",
             .schemaVersion = document::SchemaVersion{1, 0},
             .subject = std::nullopt,
             .mediaType = "application/octet-stream",
             .referencePolicy = document::NoExtensionReferences{},
             .payload = sharedPayload});
        test.expect(added, "the shared extension payload fixture is valid");
    }
    Document extensionDocument(std::move(extensionProject));
    CommandStack extensionStack(extensionDocument);
    const auto extensionBefore = extensionDocument.snapshot();
    const auto extensionEdit = rename(extensionStack, extensionDocument, "Project name 001");
    test.expect(extensionEdit.changed() &&
                    extensionStack.historyChargedBytes() ==
                        extensionBefore.project().rootByteCharge() + sizeof(document::IdAllocator),
                "history excludes opaque storage shared with the active project");

    Document bounded(makeProject());
    CommandStack boundedStack(bounded, {.maxEntries = 2, .maxChargedBytes = 512ULL * 1024 * 1024});
    test.expect(rename(boundedStack, bounded, "Project name 001").changed() &&
                    rename(boundedStack, bounded, "Project name 002").changed(),
                "the first two edits fit the two-entry undo policy");
    const auto third = rename(boundedStack, bounded, "Project name 003");
    test.expect(third.changed() && third.historyEntriesDropped == 1 && boundedStack.size() == 2 &&
                    boundedStack.work().historyEntriesRetired == 1 &&
                    boundedStack.historyChargedBytes() <=
                        boundedStack.historyLimits().maxChargedBytes,
                "the oldest entry is explicitly retired while the newest stays undoable");
    test.expect(third.historyEntryLimitReached && !third.historyMemoryLimitReached,
                "the retirement names the entry-count bound, not the memory bound (#744)");
    test.expect(boundedStack.undo().changed() &&
                    bounded.snapshot().project().name() == "Project name 002" &&
                    boundedStack.undo().changed() &&
                    bounded.snapshot().project().name() == "Project name 001" &&
                    boundedStack.undo().status == CommandStatus::NothingToUndo,
                "retained undo steps restore exact state and stop at the announced floor");
    test.expect(boundedStack.redo().changed() && boundedStack.redo().changed() &&
                    bounded.snapshot().project().name() == "Project name 003",
                "redo restores exact state after bounded history pruning");

    const auto oneStepBytes = rootCharge + afterRootCharge;
    Document byteBounded(makeProject());
    CommandStack byteStack(byteBounded, {.maxEntries = 10, .maxChargedBytes = oneStepBytes});
    const auto byteFirst = rename(byteStack, byteBounded, "Project name 001");
    const auto byteSecond = rename(byteStack, byteBounded, "Project name 002");
    test.expect(byteFirst.changed() && byteSecond.changed() &&
                    byteSecond.historyEntriesDropped == 1 && byteStack.size() == 1 &&
                    byteStack.historyChargedBytes() <= oneStepBytes,
                "a retained byte cap retires old snapshots without refusing the newest step");
    test.expect(byteSecond.historyMemoryLimitReached && !byteSecond.historyEntryLimitReached &&
                    !byteFirst.historyMemoryLimitReached && !byteFirst.historyEntryLimitReached,
                "the retirement names the memory bound, and a step that retires nothing names none");

    Document refused(makeProject());
    CommandStack refusedStack(refused, {.maxEntries = 10, .maxChargedBytes = oneStepBytes - 1});
    const auto revision = refused.snapshot().revision();
    const auto denied = rename(refusedStack, refused, "Project name 001");
    test.expect(denied.status == CommandStatus::HistoryLimit && refusedStack.size() == 0 &&
                    refused.snapshot().revision() == revision &&
                    refused.snapshot().project().name() == "Original Project",
                "an oversized single undo step rejects before publication without losing truth");
}

// #746: an agent session's commits group into one undo step labelled with the session. The merged
// entry keeps only the state before the session and the newest state, so the history holds no more
// than one step for the whole session; the artist's own edit seals the group's entry and stays its
// own step, so nothing is folded into the agent's step or lost.
void testAgentSessionGroupsIntoOneUndoStep(TestContext& test) {
    const auto rename = [](CommandStack& stack, Document& document, std::string name,
                           const CommandOrigin origin) {
        Transaction transaction(origin == CommandOrigin::Agent ? "Agent edit" : "Rename",
                                document.snapshot().revision());
        transaction.setOrigin(origin);
        transaction.emplace<SetProjectName>(std::move(name));
        return stack.execute(std::move(transaction));
    };
    Document document(makeProject());
    CommandStack stack(document);
    test.expect(rename(stack, document, "Before", CommandOrigin::User).changed(),
                "the artist's edit before the session is its own step");
    const auto chargeOfOneStep = stack.historyChargedBytes();

    stack.beginGroup("Agent session \u201cShot 12\u201d", CommandOrigin::Agent);
    for (int index = 0; index < 50; ++index) {
        const auto result =
            rename(stack, document, "Agent " + std::to_string(index), CommandOrigin::Agent);
        test.expect(result.changed() && result.historyGrouped && result.label == "Agent edit",
                    "every session commit lands, grouped, with its own label in its result");
    }
    test.expect(stack.size() == 2 && stack.undoLabel() == "Agent session \u201cShot 12\u201d",
                "fifty commits make one undo step labelled with the session");
    test.expect(stack.historyChargedBytes() <= 2 * chargeOfOneStep,
                "the grouped step holds the states before and after the session, not the fifty");
    test.expect(stack.groupStats() ==
                    HistoryGroupStats{.commits = 50, .entries = 1, .interruptions = 0},
                "the group counts its commits");

    // The artist's own edit inside the session seals the group's entry and is its own step.
    test.expect(rename(stack, document, "Artist", CommandOrigin::User).changed() &&
                    stack.groupStats().interruptions == 1,
                "the artist's edit applies inside the session and seals the group's step");
    test.expect(stack.size() == 3 && stack.undoLabel() == "Rename",
                "the artist's edit is a step of its own, after the session's");
    test.expect(rename(stack, document, "Agent again", CommandOrigin::Agent).historyGrouped &&
                    stack.size() == 4 && stack.undoLabel() == "Agent session \u201cShot 12\u201d",
                "the session's next commit starts a new group step after the artist's");
    const auto stats = stack.endGroup();
    test.expect(stats == HistoryGroupStats{.commits = 51, .entries = 2, .interruptions = 1},
                "ending the group reports its commits, steps and the interruption");
    test.expect(!stack.groupOpen() &&
                    !rename(stack, document, "After", CommandOrigin::Agent).historyGrouped &&
                    stack.size() == 5,
                "after the session every commit is its own step again");

    // Undo walks back exactly: the commit after the session, the second group step, the artist's
    // edit, then the whole first group step at once.
    test.expect(stack.undo().changed() && document.snapshot().project().name() == "Agent again",
                "undo the edit after the session");
    test.expect(stack.undo().changed() && document.snapshot().project().name() == "Artist",
                "undo the session's second step restores the artist's edit");
    test.expect(stack.undo().changed() && document.snapshot().project().name() == "Agent 49",
                "undo the artist's edit restores the session's newest state");
    test.expect(stack.undo().changed() && document.snapshot().project().name() == "Before",
                "one undo takes back all fifty session commits");
    test.expect(stack.redo().changed() && document.snapshot().project().name() == "Agent 49",
                "redo brings the whole session step back");

    // An agent's undo inside a session seals the entry too: the next commit is a new step.
    Document other(makeProject());
    CommandStack otherStack(other);
    otherStack.beginGroup("Agent session", CommandOrigin::Agent);
    static_cast<void>(rename(otherStack, other, "One", CommandOrigin::Agent));
    static_cast<void>(rename(otherStack, other, "Two", CommandOrigin::Agent));
    test.expect(otherStack.undo(CommandOrigin::Agent).changed() &&
                    other.snapshot().project().name() == "Original Project",
                "an agent's undo inside the session takes back the session so far");
    static_cast<void>(rename(otherStack, other, "Three", CommandOrigin::Agent));
    test.expect(otherStack.size() == 1 && !otherStack.canRedo() &&
                    otherStack.groupStats().entries == 2 &&
                    otherStack.groupStats().interruptions == 0,
                "the next commit starts a new group step and replaces the redo branch");

    // Grouping keeps the history bounded: a two-entry history never retires the group's step to
    // make room for it.
    Document bounded(makeProject());
    CommandStack boundedStack(bounded, {.maxEntries = 2, .maxChargedBytes = 512ULL * 1024 * 1024});
    static_cast<void>(rename(boundedStack, bounded, "A", CommandOrigin::User));
    static_cast<void>(rename(boundedStack, bounded, "B", CommandOrigin::User));
    boundedStack.beginGroup("Agent session", CommandOrigin::Agent);
    const auto firstGrouped = rename(boundedStack, bounded, "C", CommandOrigin::Agent);
    std::size_t retiredLater = 0;
    for (int index = 0; index < 10; ++index)
        retiredLater +=
            rename(boundedStack, bounded, "D" + std::to_string(index), CommandOrigin::Agent)
                .historyEntriesDropped;
    test.expect(firstGrouped.historyEntriesDropped == 1 && retiredLater == 0 &&
                    boundedStack.size() == 2,
                "only the group's first commit retires a step; merging retires nothing");
    test.expect(boundedStack.undo().changed() && bounded.snapshot().project().name() == "B",
                "the bounded history still undoes the session in one step");
}

void testSharedPayloadRetentionAndReplacementPressure(TestContext& test) {
    constexpr std::size_t largeBytes = 2 * 1024 * 1024;
    auto project = makeProject();
    const document::OpaqueExtensionPayload originalPayload{std::vector<std::byte>(largeBytes)};
    const auto added =
        project.addExtensionRecord({.id = document::ExtensionRecordId::fromRaw(1),
                                    .ownerId = "vendor.module",
                                    .typeId = "vendor.module.record-type",
                                    .schemaVersion = document::SchemaVersion{1, 0},
                                    .subject = std::nullopt,
                                    .mediaType = "application/octet-stream",
                                    .referencePolicy = document::NoExtensionReferences{},
                                    .payload = originalPayload});
    test.expect(added, "large payload fixture is valid");

    const auto rename = [](CommandStack& stack, Document& document, std::string name) {
        Transaction transaction("Rename", document.snapshot().revision());
        transaction.emplace<SetProjectName>(std::move(name));
        return stack.execute(std::move(transaction));
    };
    Document shared(project);
    CommandStack sharedStack(shared, {.maxEntries = 3, .maxChargedBytes = 32 * 1024});
    const auto first = rename(sharedStack, shared, "First");
    const auto second = rename(sharedStack, shared, "Second");
    test.expect(
        first.changed() && second.changed() && sharedStack.size() == 2 &&
            sharedStack.historyChargedBytes() < largeBytes &&
            shared.snapshot().project().extensionRecords().front().payload.storageIdentity() ==
                originalPayload.storageIdentity(),
        "small renames retain undo despite a much larger shared active payload");
    for (int cycle = 0; cycle < 3; ++cycle) {
        const auto undone = sharedStack.undo();
        test.expect(undone.changed() && shared.snapshot().project().name() == "First" &&
                        sharedStack.historyChargedBytes() <=
                            sharedStack.historyLimits().maxChargedBytes,
                    "undo preserves shared-payload charge and exact project truth");
        const auto redone = sharedStack.redo();
        test.expect(redone.changed() && shared.snapshot().project().name() == "Second" &&
                        sharedStack.historyChargedBytes() <=
                            sharedStack.historyLimits().maxChargedBytes,
                    "redo preserves shared-payload charge and exact project truth");
    }
    test.expect(sharedStack.undo().changed() && rename(sharedStack, shared, "Branch").changed() &&
                    !sharedStack.canRedo() &&
                    sharedStack.historyChargedBytes() <=
                        sharedStack.historyLimits().maxChargedBytes,
                "branching retires redo and keeps only retained-history bytes charged");

    Document growing(project);
    CommandStack growingStack(growing, {.maxEntries = 32, .maxChargedBytes = 512 * 1024});
    for (int index = 0; index < 16; ++index)
        test.expect(rename(growingStack, growing, "Rename " + std::to_string(index)).changed(),
                    "the growing history fixture retains every rename");
    const auto visitsBefore = growingStack.work().chargedSnapshotsVisited;
    test.expect(rename(growingStack, growing, "Final rename").changed() &&
                    growingStack.work().chargedSnapshotsVisited - visitsBefore == 4,
                "a new edit visits only its two new states per preflight and projection");

    const document::OpaqueExtensionPayload replacement(
        std::vector<std::byte>(largeBytes, std::byte{1}));
    Document pressured(project);
    CommandStack pressuredStack(pressured, {.maxEntries = 3, .maxChargedBytes = largeBytes - 1});
    Transaction replace("Replace payload", pressured.snapshot().revision());
    replace.emplace<ReplaceExtensionPayload>(replacement);
    const auto revision = pressured.snapshot().revision();
    const auto denied = pressuredStack.execute(std::move(replace));
    test.expect(
        denied.status == CommandStatus::HistoryLimit && pressuredStack.size() == 0 &&
            pressured.snapshot().revision() == revision &&
            pressured.snapshot().project().extensionRecords().front().payload.storageIdentity() ==
                originalPayload.storageIdentity(),
        "a real replaced payload over the cap rejects atomically");

    Document allowed(project);
    CommandStack allowedStack(allowed,
                              {.maxEntries = 3, .maxChargedBytes = largeBytes + 32 * 1024});
    Transaction acceptedReplace("Replace payload", allowed.snapshot().revision());
    acceptedReplace.emplace<ReplaceExtensionPayload>(replacement);
    const auto accepted = allowedStack.execute(std::move(acceptedReplace));
    test.expect(accepted.changed() && allowedStack.historyChargedBytes() >= largeBytes &&
                    allowedStack.historyChargedBytes() <=
                        allowedStack.historyLimits().maxChargedBytes,
                "replaced payload is charged while the new payload is active");
    test.expect(
        allowedStack.undo().changed() &&
            allowed.snapshot().project().extensionRecords().front().payload.storageIdentity() ==
                originalPayload.storageIdentity() &&
            allowedStack.historyChargedBytes() >= largeBytes &&
            allowedStack.historyChargedBytes() <= allowedStack.historyLimits().maxChargedBytes &&
            allowedStack.redo().changed() &&
            allowed.snapshot().project().extensionRecords().front().payload.storageIdentity() ==
                replacement.storageIdentity(),
        "undo and redo charge the inactive replacement while preserving payload identity");
}

} // namespace
} // namespace bloom::commands::test

namespace bloom::commands::test {
namespace {

// #744: an independent oracle for the undo allowance. Every root, content allocation, record and
// payload the retained states hold is counted once by identity; whatever the active state holds
// is not charged (its root only when it is shared with the newest history state).
[[nodiscard]] std::size_t expectedHistoryCharge(const std::vector<document::Snapshot>& retained,
                                                const document::Snapshot& active,
                                                const bool sharedRoot) {
    std::unordered_map<const void*, std::size_t> chunks;
    const auto collect = [](const document::Snapshot& state,
                            std::unordered_map<const void*, std::size_t>& into) {
        into.emplace(state.stateIdentity(), state.rootByteCharge() + sizeof(document::IdAllocator));
        for (const auto& composition : state.project().compositions()) {
            into.emplace(composition.contentIdentity(), composition.contentShellByteCharge());
            composition.forEachRecord([&](const void* identity, const auto& record) {
                into.emplace(identity, document::recordByteCharge(record));
            });
        }
        for (const auto& record : state.project().extensionRecords())
            if (record.payload.storageIdentity() != nullptr)
                into.emplace(record.payload.storageIdentity(), record.payload.size());
    };
    for (const auto& state : retained)
        collect(state, chunks);
    std::unordered_map<const void*, std::size_t> current;
    collect(active, current);
    if (!sharedRoot)
        current.erase(active.stateIdentity());
    std::size_t bytes = 0;
    for (const auto& [identity, size] : chunks)
        if (!current.contains(identity))
            bytes += size;
    return bytes;
}

// #744: content is shared per record, so after a bulk key bake every one-key edit charges the undo
// allowance for what it changed, the history charge matches the oracle through undo, redo, a
// branch and an agent group, and undo and redo restore every record exactly.
void testUndoChargesSharedRecordsOnce(TestContext& test) {
    constexpr std::uint64_t kParameters = 120;
    constexpr std::size_t kKeysPerParameter = 100;
    auto project = makeProject();
    auto* main = project.findComposition(kCompositionId);
    const auto parameterOf = [](const std::uint64_t index) {
        return ParameterId::fromRaw(5'000 + index);
    };
    for (std::uint64_t index = 0; index < kParameters; ++index)
        requireFixture(main->parameters().insert(
                           {parameterOf(index), std::string(document::kRotationParameterSchemaKey),
                            ConstantValueSource{0.0}}),
                       "bulk rotation parameter must be accepted");
    Document document(std::move(project));
    CommandStack stack(document);
    std::vector<document::Snapshot> states{document.snapshot()};
    const auto keysFor = [](const double offset, const std::size_t count) {
        std::vector<ParameterKey> keys;
        for (std::size_t frame = 0; frame < count; ++frame)
            keys.push_back({*core::RationalTime::create(static_cast<std::int64_t>(frame), 24),
                            offset + static_cast<double>(frame)});
        return keys;
    };
    const auto checkCharge = [&](const std::vector<document::Snapshot>& retained,
                                 const bool sharedRoot, const char* message) {
        test.expect(stack.historyChargedBytes() ==
                        expectedHistoryCharge(retained, document.snapshot(), sharedRoot),
                    message);
    };

    // Bulk bake: four transactions of 30 parameters x 100 keys.
    for (std::uint64_t batch = 0; batch < 4; ++batch) {
        Transaction transaction("Bake", document.snapshot().revision());
        for (std::uint64_t index = batch * 30; index < (batch + 1) * 30; ++index)
            transaction.emplace<SetParameterKeys>(
                kCompositionId, parameterOf(index),
                keysFor(static_cast<double>(index), kKeysPerParameter), true);
        test.expect(stack.execute(std::move(transaction)).changed(), "a bulk bake applies");
        states.push_back(document.snapshot());
        checkCharge(states, true, "the charge after a bulk bake matches the oracle");
    }
    const auto afterBake = stack.historyChargedBytes();
    const auto contentCharge =
        document.snapshot().project().findComposition(kCompositionId)->contentByteCharge();

    // One-key edits: each costs its curve and the content's own allocation, not the composition.
    constexpr int kEdits = 20;
    for (int edit = 0; edit < kEdits; ++edit) {
        Transaction transaction("Key", document.snapshot().revision());
        transaction.emplace<SetParameterKeys>(
            kCompositionId, parameterOf(static_cast<std::uint64_t>(edit) * 5),
            std::vector<ParameterKey>{{*core::RationalTime::create(500 + edit, 24), 1.0 + edit}},
            false);
        test.expect(stack.execute(std::move(transaction)).changed(), "a one-key edit applies");
        states.push_back(document.snapshot());
        checkCharge(states, true, "the charge after a one-key edit matches the oracle");
    }
    const auto perEdit = (stack.historyChargedBytes() - afterBake) / kEdits;
    test.expect(perEdit * 10 < contentCharge,
                "a one-key edit charges a small part of the composition, not a copy of it");

    // Undo every step: each restores the exact records of the state before it.
    const auto sameRecords = [](const document::Composition& left,
                                const document::Composition& right) {
        return std::ranges::equal(left.parameters().records(), right.parameters().records()) &&
               std::ranges::equal(left.animationCurves().records(),
                                  right.animationCurves().records()) &&
               std::ranges::equal(left.graph().nodes(), right.graph().nodes()) &&
               std::ranges::equal(left.graph().edges(), right.graph().edges()) &&
               std::ranges::equal(left.graph().layerOutputs(), right.graph().layerOutputs()) &&
               left.animationCurves().keyframeIds().size() ==
                   right.animationCurves().keyframeIds().size();
    };
    for (auto index = states.size() - 1; index > 0; --index) {
        test.expect(stack.undo().changed(), "undo applies");
        const auto& now = *document.snapshot().project().findComposition(kCompositionId);
        const auto& then = *states[index - 1].project().findComposition(kCompositionId);
        test.expect(sameRecords(now, then) && now.contentIdentity() == then.contentIdentity(),
                    "undo restores the exact records of the state before the step");
        checkCharge(states, false, "the charge after undo matches the oracle");
    }
    for (std::size_t index = 1; index < states.size(); ++index) {
        test.expect(stack.redo().changed(), "redo applies");
        const auto& now = *document.snapshot().project().findComposition(kCompositionId);
        const auto& then = *states[index].project().findComposition(kCompositionId);
        test.expect(sameRecords(now, then), "redo restores the exact records of the later state");
        checkCharge(states, false, "the charge after redo matches the oracle");
    }

    // Branch: undo three steps and edit; the redone states leave the history and its charge.
    for (int step = 0; step < 3; ++step)
        test.expect(stack.undo().changed(), "undo before the branch applies");
    states.erase(states.end() - 3, states.end());
    // The branch's step starts from the restored state, a new root sharing the undone-to content.
    states.push_back(document.snapshot());
    {
        Transaction transaction("Branch", document.snapshot().revision());
        transaction.emplace<SetParameterKeys>(
            kCompositionId, parameterOf(1),
            std::vector<ParameterKey>{{*core::RationalTime::create(900, 24), 7.0}}, false);
        test.expect(stack.execute(std::move(transaction)).changed(), "the branch edit applies");
        states.push_back(document.snapshot());
        checkCharge(states, true, "the charge after a branch matches the oracle");
    }

    // An agent group keeps the state before the group and its newest state.
    stack.beginGroup("Agent", CommandOrigin::Agent);
    const auto beforeGroup = states;
    for (int edit = 0; edit < 5; ++edit) {
        Transaction transaction("Agent key", document.snapshot().revision());
        transaction.setOrigin(CommandOrigin::Agent);
        transaction.emplace<SetParameterKeys>(
            kCompositionId, parameterOf(static_cast<std::uint64_t>(edit) + 60),
            std::vector<ParameterKey>{{*core::RationalTime::create(700 + edit, 24), 2.0}}, false);
        test.expect(stack.execute(std::move(transaction)).historyGrouped, "a group commit lands");
        auto retained = beforeGroup;
        retained.push_back(document.snapshot());
        checkCharge(retained, true, "the charge of a grouped entry matches the oracle");
    }
    static_cast<void>(stack.endGroup());
    test.expect(document.snapshot().project().validate().ok(), "the final project validates");
}

// #744 (A2): prepare() on another thread then publish() on the stack's own is execute(): the same
// result, history step and events; preparation touches neither the stack nor the document; a
// cancelled preparation publishes nothing; a base the document moved past is re-based when the
// transaction still applies on the live revision and refused as stale otherwise.
void testPrepareOffThreadThenPublish(TestContext& test) {
    {
        Document document(makeProject());
        CommandStack stack(document);
        std::size_t events = 0;
        const auto observer = stack.addObserver([&](const CommandEvent&) { ++events; });
        const auto base = document.snapshot();
        const auto workBefore = document.work();
        Transaction transaction("Prepared rename", base.revision());
        transaction.setOrigin(CommandOrigin::Agent);
        transaction.emplace<SetProjectName>("Prepared");
        auto prepared = std::async(std::launch::async, [&base, &transaction] {
                            return CommandStack::prepare(base, std::move(transaction));
                        }).get();
        test.expect(document.snapshot().revision() == base.revision() && stack.size() == 0 &&
                        events == 0 && document.work().publications == workBefore.publications,
                    "preparation publishes nothing and sends no event");
        test.expect(!prepared.cancelled() && prepared.baseRevision() == base.revision() &&
                        prepared.label() == "Prepared rename" &&
                        prepared.origin() == CommandOrigin::Agent && !stack.rebaseNeeded(prepared),
                    "a prepared transaction names its base, label and origin");
        const auto result = stack.publish(std::move(prepared));
        test.expect(result.succeeded() && result.origin == CommandOrigin::Agent &&
                        document.snapshot().project().name() == "Prepared" && stack.size() == 1 &&
                        stack.undoLabel() == std::optional<std::string_view>("Prepared rename"),
                    "publish lands the prepared edit as one undo step");
        test.expect(events == 2, "publish sends RevisionChanged and HistoryChanged");
        test.expect(stack.undo().succeeded() &&
                        document.snapshot().project().name() == "Original Project",
                    "the published step undoes");
        stack.removeObserver(observer);
    }
    {
        Document document(makeProject());
        CommandStack stack(document);
        Transaction transaction("Cancelled", document.snapshot().revision());
        transaction.emplace<SetProjectName>("Cancelled");
        auto prepared =
            CommandStack::prepare(document.snapshot(), std::move(transaction), [] { return true; });
        test.expect(prepared.cancelled(), "a cancellation check that fires stops preparation");
        const auto result = stack.publish(std::move(prepared));
        test.expect(!result.succeeded() &&
                        document.snapshot().project().name() == "Original Project" &&
                        stack.size() == 0,
                    "a cancelled preparation publishes nothing");
    }
    {
        // The artist edits between preparation and publication.
        Document document(makeProject());
        CommandStack stack(document);
        const auto base = document.snapshot();
        Transaction expecting("Agent", base.revision());
        expecting.emplace<SetProjectName>("Agent");
        auto stale = CommandStack::prepare(base, std::move(expecting));
        Transaction open("Agent");
        open.emplace<SetProjectName>("Agent");
        auto rebased = CommandStack::prepare(base, std::move(open));
        Transaction artist("Artist", base.revision());
        artist.emplace<SetCompositionName>(kCompositionId, "Artist");
        test.expect(stack.execute(std::move(artist)).succeeded(), "the artist's edit lands");
        test.expect(!stack.rebaseNeeded(stale),
                    "an edit expecting the old revision is not rebased");
        const auto refused = stack.publish(std::move(stale));
        test.expect(refused.status == CommandStatus::StaleRevision &&
                        refused.expectedRevision == base.revision() &&
                        document.snapshot().project().name() == "Original Project",
                    "and is refused as stale without publishing");
        test.expect(stack.rebaseNeeded(rebased), "an edit expecting no revision is rebased");
        const auto live = document.snapshot();
        auto again = CommandStack::prepare(live, std::move(rebased).takeTransaction());
        test.expect(!stack.rebaseNeeded(again), "prepared again on the live snapshot");
        const auto published = stack.publish(std::move(again));
        test.expect(published.succeeded() && document.snapshot().project().name() == "Agent" &&
                        document.snapshot().project().findComposition(kCompositionId)->name() ==
                            "Artist" &&
                        stack.historyLabels() == std::vector<std::string>{"Artist", "Agent"},
                    "the re-based edit lands after the artist's, keeping both");
    }
    {
        // A refused operation and an invalid draft are reported as execute() reports them.
        Document document(makeProject());
        CommandStack stack(document);
        Transaction invalid("Break", document.snapshot().revision());
        invalid.emplace<SetProjectName>("Fine");
        invalid.emplace<BreakProject>();
        auto prepared = CommandStack::prepare(document.snapshot(), std::move(invalid));
        const auto result = stack.publish(std::move(prepared));
        test.expect(result.status == CommandStatus::ValidationFailed &&
                        result.validationOperationIndex == std::optional<std::size_t>(1) &&
                        document.snapshot().project().name() == "Original Project",
                    "an invalid prepared draft names its first invalid operation");
    }
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testFiniteTimeFootprintAggregation(test);
        bloom::commands::test::testOperationWarningsArePublished(test);
        bloom::commands::test::testLayerIdentityRemapAggregation(test);
        bloom::commands::test::testRenderAffectingPublication(test);
        bloom::commands::test::testAtomicTransactionUndoAndRedo(test);
        bloom::commands::test::testRejectedAndInvalidTransactionsAreAtomic(test);
        bloom::commands::test::testKnownParameterSchemaRejectionsAreAtomic(test);
        bloom::commands::test::testStaleExecuteUndoAndRedoAreNoOps(test);
        bloom::commands::test::testLayerOrderNoChangeAndRedoInvalidation(test);
        bloom::commands::test::testDiscardRedoForgetsWithdrawnEdits(test);
        bloom::commands::test::testExhaustionSurvivesUndoAndRedo(test);
        bloom::commands::test::testBoundedUndoHistoryAndByteAccounting(test);
        bloom::commands::test::testSharedPayloadRetentionAndReplacementPressure(test);
        bloom::commands::test::testAgentSessionGroupsIntoOneUndoStep(test);
        bloom::commands::test::testUndoChargesSharedRecordsOnce(test);
        bloom::commands::test::testPrepareOffThreadThenPublish(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
