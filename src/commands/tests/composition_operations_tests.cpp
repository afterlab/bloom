#include "command_test_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/group_definition_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_layout_arrange.hpp>
#include <bloom/commands/node_layout_placement.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_group_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bloom::commands::test {
namespace {

using document::CompositionFormat;
using document::FrameRate;
using document::NodeId;

void testAddCompositionBuildsTopologyAndUndoRedo(TestContext& test) {
    const auto frameRate = FrameRate::create(30, 1).value_or(FrameRate::framesPerSecond24());
    const auto format =
        CompositionFormat::create(1280, 720, core::PixelAspectRatio::square(), frameRate)
            .value_or(CompositionFormat{});

    Document document(
        document::makeNewProject("Project", "Main", core::RationalTime::fromInteger(10)).project);
    CommandStack stack(document);
    const auto before = document.snapshot();
    Transaction transaction("Add Composition", before.revision());
    transaction.emplace<AddComposition>("Second", format, frameRate,
                                        core::RationalTime::fromInteger(20));
    const auto result = stack.execute(std::move(transaction));
    const auto compositionId = result.outputId<document::CompositionId>(kAddCompositionOutput)
                                   .value_or(document::CompositionId{});
    const auto after = document.snapshot();
    const auto* added = after.project().findComposition(compositionId);

    test.expect(result.changed() && compositionId.isValid() && added != nullptr,
                "AddComposition creates and returns a composition");
    test.expect(added != nullptr && added->name() == "Second" &&
                    added->duration() == core::RationalTime::fromInteger(20) &&
                    added->format() == format && added->graph().nodes().size() == 2 &&
                    added->graph().edges().size() == 1 &&
                    added->graph().compositionOutput().has_value(),
                "AddComposition builds the Merge-to-Output topology and requested settings");
    test.expect(after.revision().value() == before.revision().value() + 1 && stack.size() == 1 &&
                    stack.trackedRevision() == after.revision(),
                "AddComposition has one dirty revision and one history entry");

    test.expect(stack.undo().changed() && document.snapshot().project().compositions().size() == 1,
                "AddComposition is one undo step");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(compositionId) != nullptr,
                "AddComposition redo restores the pinned composition");
}

// #694: a new composition's starting Merge and Output are laid out by Arrange's own rule (the
// command-side card estimate), never the compact legacy columns: Arrange leaves them where they
// are, the cards keep at least the Arrange gap and they share a row.
void testNewCompositionStartsArranged(TestContext& test) {
    Document document(document::makeNewProject("Project").project);
    CommandStack stack(document);
    Transaction transaction("Add Composition", document.snapshot().revision());
    transaction.emplace<AddComposition>("Main", CompositionFormat{},
                                        core::RationalTime::fromInteger(10));
    const auto result = stack.execute(std::move(transaction));
    const auto id = result.outputId<document::CompositionId>(kAddCompositionOutput)
                        .value_or(document::CompositionId{});
    const auto snapshot = document.snapshot();
    const auto* composition = snapshot.project().findComposition(id);
    test.expect(composition != nullptr, "the composition is created");
    if (composition == nullptr)
        return;
    const auto merge = composition->graph().outputMergeId();
    const auto output = composition->graph().compositionOutput();
    test.expect(merge.has_value() && output.has_value(), "the composition has its Merge and Output");
    if (!merge || !output)
        return;
    const auto arranged = arrangeNodes(*composition, {});
    for (const auto& [node, position] : arranged)
        test.expect(effectiveNodePosition(*composition, node) == position,
                    "a starting node sits exactly where Arrange All puts it");
    const auto sizes = nodeCardSizes(*composition, {});
    const auto mergeAt = effectiveNodePosition(*composition, *merge);
    const auto outputAt = effectiveNodePosition(*composition, output->nodeId);
    test.expect(outputAt.x - (mergeAt.x + sizes.at(*merge).width) >= document::kNodeLayoutSpacing,
                "the Output keeps the Arrange gap right of the Merge");
    test.expect(outputAt.x - (mergeAt.x + sizes.at(*merge).width) <
                    document::kNodeLayoutSpacing + document::kNodeLayoutGrid,
                "the gap is Arrange's gap on the grid, not the legacy column pitch");
    const auto mergeCenter = mergeAt.y + sizes.at(*merge).height / 2.0;
    const auto outputCenter = outputAt.y + sizes.at(output->nodeId).height / 2.0;
    test.expect(std::abs(mergeCenter - outputCenter) <= document::kNodeLayoutGrid,
                "the Output is centered on the Merge's row, as Arrange centers a node on its band");
    test.expect(std::fmod(mergeAt.x, document::kNodeLayoutGrid) == 0.0 &&
                    std::fmod(outputAt.y, document::kNodeLayoutGrid) == 0.0,
                "starting positions are on the layout grid");
}

void testDeleteCompositionAllowsLastAndUndoRedo(TestContext& test) {
    Document document(
        document::makeNewProject("Project", "Main", core::RationalTime::fromInteger(10)).project);
    CommandStack stack(document);
    const auto initialId = document::CompositionId::fromRaw(1);
    const auto format = CompositionFormat::create(640, 480).value_or(CompositionFormat{});

    Transaction add("Add Composition", document.snapshot().revision());
    add.emplace<AddComposition>("Second", format, core::RationalTime::fromInteger(8));
    const auto added = stack.execute(std::move(add));
    const auto secondId = added.outputId<document::CompositionId>(kAddCompositionOutput)
                              .value_or(document::CompositionId{});

    const auto beforeDelete = document.snapshot();
    Transaction remove("Delete Composition", beforeDelete.revision());
    remove.emplace<DeleteComposition>(initialId);
    const auto removed = stack.execute(std::move(remove));
    test.expect(removed.changed() &&
                    document.snapshot().project().findComposition(initialId) == nullptr,
                "DeleteComposition removes the requested composition");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(initialId) != nullptr,
                "DeleteComposition undo restores the composition");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(initialId) == nullptr,
                "DeleteComposition redo reapplies the deletion");

    // Deleting the FINAL composition is allowed and yields the same usable empty project a blank
    // startup begins with; undo restores it as one history step.
    const auto lastBefore = document.snapshot();
    Transaction last("Delete Last Composition", lastBefore.revision());
    last.emplace<DeleteComposition>(secondId);
    const auto lastDelete = stack.execute(std::move(last));
    test.expect(lastDelete.changed() && document.snapshot().project().compositions().empty() &&
                    document.snapshot().project().validate().ok(),
                "DeleteComposition deletes the last composition and leaves a valid empty project");
    test.expect(stack.size() == 3, "deleting the last composition is a single history entry");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(secondId) != nullptr &&
                    document.snapshot().project().validate().ok(),
                "undo restores the final composition");
    test.expect(stack.redo().changed() && document.snapshot().project().compositions().empty(),
                "redo removes the final composition again");
}

void testDuplicateCompositionRemapsDeepDocumentState(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);

    Transaction group("Group Nodes", document.snapshot().revision());
    group.emplace<GroupNodes>(kCompositionId, std::set<NodeId>{kFirstLayerNodeId}, "Frame");
    test.expect(stack.execute(std::move(group)).changed(), "duplicate fixture creates a group");
    Transaction move("Move Nodes", document.snapshot().revision());
    move.emplace<MoveNodes>(kCompositionId,
                            std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{42.0, 84.0}}});
    test.expect(stack.execute(std::move(move)).changed(), "duplicate fixture moves a node");
    Transaction animate("Animate Opacity", document.snapshot().revision());
    animate.emplace<CreateAnimationForParameter>(kCompositionId, kOpacityId,
                                                 core::RationalTime::fromInteger(0));
    test.expect(stack.execute(std::move(animate)).changed(), "duplicate fixture creates animation");

    const auto before = document.snapshot();
    const auto* source = before.project().findComposition(kCompositionId);
    requireFixture(source != nullptr, "duplicate source must exist");
    Transaction duplicate("Duplicate Composition", before.revision());
    duplicate.emplace<DuplicateComposition>(kCompositionId);
    const auto result = stack.execute(std::move(duplicate));
    const auto copyId = result.outputId<document::CompositionId>(kDuplicateCompositionOutput)
                            .value_or(document::CompositionId{});
    const auto after = document.snapshot();
    const auto* copy = after.project().findComposition(copyId);

    test.expect(result.changed() && copyId.isValid() && copy != nullptr &&
                    copy->name() == "Main copy" && copy->format() == source->format() &&
                    copy->duration() == source->duration(),
                "DuplicateComposition creates the named settings-preserving copy");
    test.expect(copy != nullptr && copy->graph().nodes().size() == source->graph().nodes().size() &&
                    copy->graph().edges().size() == source->graph().edges().size() &&
                    copy->parameters().records().size() == source->parameters().records().size() &&
                    copy->animationCurves().records().size() ==
                        source->animationCurves().records().size() &&
                    copy->nodeLayout().size() == source->nodeLayout().size() &&
                    copy->nodeGroups().size() == source->nodeGroups().size(),
                "DuplicateComposition preserves graph, values, animation, layout, and groups");

    if (copy != nullptr) {
        test.expect(std::ranges::none_of(copy->graph().nodes(),
                                         [&](const auto& node) {
                                             return source->graph().findNode(node.id) != nullptr;
                                         }) &&
                        std::ranges::none_of(copy->graph().edges(),
                                             [&](const auto& edge) {
                                                 return std::ranges::any_of(
                                                     source->graph().edges(), [&](const auto& old) {
                                                         return old.id == edge.id;
                                                     });
                                             }),
                    "DuplicateComposition gives graph records fresh IDs");
        test.expect(std::ranges::all_of(copy->nodeGroups(),
                                        [&](const auto& entry) {
                                            return std::ranges::none_of(
                                                entry.second.members, [&](const auto nodeId) {
                                                    return source->graph().findNode(nodeId) !=
                                                           nullptr;
                                                });
                                        }),
                    "DuplicateComposition remaps group membership to copied nodes");
        const auto* copiedOpacity = copy->parameters().find(
            std::ranges::find_if(copy->parameters().records(), [](const auto& parameter) {
                return parameter.schemaKey == document::kOpacityParameterSchemaKey;
            })->id);
        test.expect(
            copiedOpacity != nullptr &&
                std::holds_alternative<document::AnimationCurveSource>(copiedOpacity->source),
            "DuplicateComposition preserves animated parameter sources");
    }

    const auto copyRevision = after.revision();
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(copyId) == nullptr,
                "DuplicateComposition is one undo step");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(copyId) != nullptr &&
                    document.snapshot().revision().value() == copyRevision.value() + 2,
                "DuplicateComposition redo restores the exact copied IDs and revision parity");
}

void testSafeAreaSettingsAreUndoableAndDuplicated(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto settings = document::SafeAreaSettings{.action = 0.93, .title = 0.90};

    Transaction setSafeAreas("Set Safe Areas", document.snapshot().revision());
    setSafeAreas.emplace<SetCompositionSafeAreas>(kCompositionId, settings);
    const auto changed = stack.execute(std::move(setSafeAreas));
    const auto* authored = document.snapshot().project().findComposition(kCompositionId);
    test.expect(changed.changed() && authored != nullptr && authored->safeAreas() == settings,
                "SetCompositionSafeAreas writes the validated composition setting");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(kCompositionId)->safeAreas() ==
                        document::SafeAreaSettings{},
                "SetCompositionSafeAreas is one undoable edit");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(kCompositionId)->safeAreas() ==
                        settings,
                "SetCompositionSafeAreas redo restores the exact percentages");

    Transaction duplicate("Duplicate Composition", document.snapshot().revision());
    duplicate.emplace<DuplicateComposition>(kCompositionId);
    const auto result = stack.execute(std::move(duplicate));
    const auto copyId = result.outputId<document::CompositionId>(kDuplicateCompositionOutput)
                            .value_or(document::CompositionId{});
    const auto* copy = document.snapshot().project().findComposition(copyId);
    test.expect(copy != nullptr && copy->safeAreas() == settings,
                "DuplicateComposition carries per-composition safe-area settings");
}

void testWorkingColorSpaceOverrideIsUndoable(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    Transaction setWorkingSpace("Set Working Space", document.snapshot().revision());
    setWorkingSpace.emplace<SetCompositionWorkingColorSpace>(kCompositionId,
                                                             std::optional<std::string>{"ACEScg"});
    const auto changed = stack.execute(std::move(setWorkingSpace));
    const auto* composition = document.snapshot().project().findComposition(kCompositionId);
    test.expect(changed.changed() && composition != nullptr &&
                    composition->workingColorSpaceId() == std::optional<std::string>{"ACEScg"},
                "SetCompositionWorkingColorSpace writes the override");
    test.expect(
        stack.undo().changed() &&
            document.snapshot().project().findComposition(kCompositionId)->workingColorSpaceId() ==
                std::nullopt,
        "working color-space override is one undo step");
    test.expect(
        stack.redo().changed() &&
            document.snapshot().project().findComposition(kCompositionId)->workingColorSpaceId() ==
                std::optional<std::string>{"ACEScg"},
        "redo restores the working color-space override");
}

// #376 Composition Settings: one undoable command for every setting, with exact layer timing and
// anchored content across size, rate and duration changes.
struct SettingsFixture final {
    Document document{
        document::makeNewProject("Project", "Main", core::RationalTime::fromInteger(10),
                                 CompositionFormat::create(1000, 500).value_or(CompositionFormat{}))
            .project};
    CommandStack stack{document};
    document::CompositionId composition = document.snapshot().project().compositions().front().id();

    CommandResult run(std::unique_ptr<Operation> operation, const char* label = "Fixture") {
        Transaction transaction(label, document.snapshot().revision());
        (void)transaction.add(std::move(operation));
        return stack.execute(std::move(transaction));
    }
    template <typename Operation, typename... Arguments> CommandResult run(Arguments&&... args) {
        Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(args)...);
        return stack.execute(std::move(transaction));
    }
    [[nodiscard]] const document::Composition& current() const {
        return *document.snapshot().project().findComposition(composition);
    }
    [[nodiscard]] CompositionSettings settings() const { return compositionSettingsOf(current()); }
    struct Layer final {
        document::LayerId id;
        document::ParameterId position;
    };
    Layer solid(const char* name) {
        const auto result = run<AddSolidLayer>(composition, name, core::Color4d{1, 1, 1, 1});
        return {result.outputId<document::LayerId>(kAddSolidLayerLayerOutput)
                    .value_or(document::LayerId{}),
                result.outputId<document::ParameterId>(kAddSolidLayerPositionParameterOutput)
                    .value_or(document::ParameterId{})};
    }
    [[nodiscard]] std::optional<document::Vec2d>
    constantPosition(const document::ParameterId id) const {
        const auto* record = current().parameters().find(id);
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        const auto* value = constant ? std::get_if<document::Vec2d>(&constant->value) : nullptr;
        return value ? std::optional(*value) : std::nullopt;
    }
};

void testCompositionSettingsAreOneUndoableCommand(TestContext& test) {
    SettingsFixture fixture;
    const auto before = fixture.settings();
    auto next = before;
    next.name = "Renamed";
    next.format = CompositionFormat::create(
                      1920, 1080, core::PixelAspectRatio::square(),
                      FrameRate::create(30000, 1001).value_or(FrameRate::framesPerSecond24()))
                      .value_or(CompositionFormat{});
    next.duration = core::RationalTime::fromInteger(20);
    next.timecode = {core::RationalTime::fromInteger(3600), true};
    next.background = {0.25, 0.5, 0.75, 1.0};
    next.workingColorSpaceId = "ACEScg";
    next.safeAreas = {.action = 0.95, .title = 0.85};
    const auto history = fixture.stack.size();
    const auto result = fixture.run(
        std::make_unique<SetCompositionSettings>(fixture.composition, next), "Settings");
    test.expect(result.changed() && fixture.settings() == next &&
                    fixture.stack.size() == history + 1,
                "SetCompositionSettings writes every setting as one history entry");
    test.expect(!result.affectedTimes.has_value(),
                "a settings change publishes the whole-render footprint");
    test.expect(fixture.stack.undo().changed() && fixture.settings() == before,
                "one undo restores every setting");
    test.expect(fixture.stack.redo().changed() && fixture.settings() == next,
                "redo restores every setting");
    test.expect(
        fixture.run(std::make_unique<SetCompositionSettings>(fixture.composition, next)).status ==
            CommandStatus::NoChange,
        "unchanged settings commit nothing");
}

void testCompositionSettingsRefuseInvalidValues(TestContext& test) {
    SettingsFixture fixture;
    const auto refused = [&](CompositionSettings settings, const char* message) {
        const auto result = fixture.run(
            std::make_unique<SetCompositionSettings>(fixture.composition, std::move(settings)));
        test.expect(result.status == CommandStatus::Rejected && !result.operationFailures.empty() &&
                        !result.operationFailures.front().issue.message.empty(),
                    message);
    };
    auto empty = fixture.settings();
    empty.name.clear();
    refused(empty, "an empty name is refused with a reason");
    auto zero = fixture.settings();
    zero.duration = core::RationalTime{};
    refused(zero, "a zero duration is refused");
    auto negative = fixture.settings();
    negative.timecode.start = core::RationalTime::fromInteger(-1);
    refused(negative, "a negative start timecode is refused");
    auto safe = fixture.settings();
    safe.safeAreas = {.action = 0.8, .title = 0.9};
    refused(safe, "title safe outside action safe is refused");
    auto space = fixture.settings();
    space.workingColorSpaceId = std::string{};
    refused(space, "an empty working color space id is refused");
    test.expect(compositionSettingsIssue(empty).has_value() &&
                    !compositionSettingsIssue(fixture.settings()).has_value(),
                "the issue helper gives the dialog the same plain reason");
}

void testFrameRateAndDurationKeepLayerTiming(TestContext& test) {
    SettingsFixture fixture;
    const auto layer = fixture.solid("Timed");
    const auto start = core::RationalTime::create(1, 3).value_or(core::RationalTime{});
    const auto in = core::RationalTime::create(1, 2).value_or(core::RationalTime{});
    const auto out = core::RationalTime::fromInteger(8);
    test.expect(
        fixture.run<SetLayerTiming>(fixture.composition, layer.id, start, in, out).changed(),
        "the layer gets an exact timing");
    test.expect(fixture
                    .run<CreateAnimationForParameter>(fixture.composition, layer.position,
                                                      core::RationalTime{})
                    .changed(),
                "the layer position animates");
    const auto keyTime = core::RationalTime::create(5, 24).value_or(core::RationalTime{});
    test.expect(fixture
                    .run<SetKeyframeAtTimeForParameter>(fixture.composition, layer.position,
                                                        keyTime, document::Vec2d{100, 50})
                    .changed(),
                "a frame-aligned key at 5/24 s");
    const auto boundaryBefore = *fixture.current().graph().findLayer(layer.id);
    const auto curvesBefore = std::vector(fixture.current().animationCurves().records().begin(),
                                          fixture.current().animationCurves().records().end());
    auto next = fixture.settings();
    next.format =
        CompositionFormat::create(1000, 500, core::PixelAspectRatio::square(),
                                  FrameRate::create(30, 1).value_or(FrameRate::framesPerSecond24()))
            .value_or(CompositionFormat{});
    next.duration = core::RationalTime::fromInteger(6);
    test.expect(
        fixture.run(std::make_unique<SetCompositionSettings>(fixture.composition, next)).changed(),
        "rate and duration change in one command");
    const auto& after = *fixture.current().graph().findLayer(layer.id);
    test.expect(after.timing == boundaryBefore.timing && after.inPoint == boundaryBefore.inPoint &&
                    after.outPoint == boundaryBefore.outPoint,
                "layer start, in and out keep their exact times across a rate change");
    test.expect(std::ranges::equal(fixture.current().animationCurves().records(), curvesBefore),
                "keys keep their exact times (5/24 s now falls between 30 fps frames)");
}

void testDurationShrinkClampsWorkArea(TestContext& test) {
    SettingsFixture fixture;
    test.expect(fixture
                    .run<SetWorkArea>(fixture.composition, core::RationalTime::fromInteger(2),
                                      core::RationalTime::fromInteger(9))
                    .changed(),
                "a work area to 9 s");
    auto next = fixture.settings();
    next.duration = core::RationalTime::fromInteger(5);
    test.expect(
        fixture.run(std::make_unique<SetCompositionSettings>(fixture.composition, next)).changed(),
        "the duration shrinks to 5 s");
    const auto area = fixture.current().workArea();
    test.expect(area && area->start == core::RationalTime::fromInteger(2) &&
                    area->end == core::RationalTime::fromInteger(5),
                "the work area is clamped to the new duration");
    next.duration = core::RationalTime::fromInteger(1);
    test.expect(fixture.run(std::make_unique<SetCompositionSettings>(fixture.composition, next))
                        .changed() &&
                    !fixture.current().workArea().has_value(),
                "a work area left with nothing is cleared");
}

void testSizeChangeKeepsContentAtTheAnchor(TestContext& test) {
    const auto resize = [](SettingsFixture& fixture, const ResizeAnchor anchor) {
        auto next = fixture.settings();
        next.format = CompositionFormat::create(2000, 1100, next.format.pixelAspect(),
                                                next.format.frameRate())
                          .value_or(CompositionFormat{});
        return fixture.run(
            std::make_unique<SetCompositionSettings>(fixture.composition, next, anchor));
    };
    {
        SettingsFixture fixture;
        const auto layer = fixture.solid("Centred");
        const auto before = fixture.constantPosition(layer.position).value_or(document::Vec2d{});
        test.expect(resize(fixture, {1, 1}).changed(), "a centre-anchored resize commits");
        const auto after = fixture.constantPosition(layer.position);
        test.expect(after && after->x == before.x + 500 && after->y == before.y + 300,
                    "center anchor: content moves by half the size change");
        test.expect(fixture.stack.undo().changed() &&
                        fixture.constantPosition(layer.position) == before,
                    "one undo restores size and position");
    }
    {
        SettingsFixture fixture;
        const auto layer = fixture.solid("Top left");
        const auto before = fixture.constantPosition(layer.position);
        test.expect(resize(fixture, {0, 0}).changed() &&
                        fixture.constantPosition(layer.position) == before,
                    "top-left anchor: content stays");
    }
    {
        SettingsFixture fixture;
        const auto layer = fixture.solid("Keyed");
        const auto child = fixture.solid("Child");
        const auto locked = fixture.solid("Locked");
        const auto childBefore = fixture.constantPosition(child.position);
        const auto lockedBefore = fixture.constantPosition(locked.position);
        (void)fixture.run<SetLayerParent>(fixture.composition, child.id,
                                          std::optional<document::LayerId>{layer.id});
        (void)fixture.run<SetLayerLocked>(fixture.composition, locked.id, true);
        (void)fixture.run<CreateAnimationForParameter>(fixture.composition, layer.position,
                                                       core::RationalTime{});
        (void)fixture.run<SetKeyframeAtTimeForParameter>(fixture.composition, layer.position,
                                                         core::RationalTime::fromInteger(2),
                                                         document::Vec2d{10, 20});
        test.expect(resize(fixture, {2, 2}).changed(), "a bottom-right resize commits");
        const auto* record = fixture.current().parameters().find(layer.position);
        const auto* animated =
            record ? std::get_if<document::AnimationCurveSource>(&record->source) : nullptr;
        const auto* curve =
            animated ? fixture.current().animationCurves().findVec2(animated->curveId) : nullptr;
        bool keysMoved = curve != nullptr;
        if (curve != nullptr)
            for (const auto& key : curve->component(document::AnimationComponent::X)->keyframes)
                if (key.time == core::RationalTime::fromInteger(2))
                    keysMoved = keysMoved && key.value == 10 + 1000;
        test.expect(keysMoved, "bottom-right anchor: every key moves by the whole size change");
        test.expect(fixture.constantPosition(child.position) == childBefore,
                    "a parented layer follows its parent instead of moving itself");
        test.expect(fixture.constantPosition(locked.position) == lockedBefore,
                    "a locked layer is not moved");
    }
}

void testAddAndUpdateCompositionSettings(TestContext& test) {
    SettingsFixture fixture;
    auto settings = fixture.settings();
    settings.name = "Vertical";
    settings.format = CompositionFormat::create(1080, 1920).value_or(CompositionFormat{});
    settings.timecode = {core::RationalTime::fromInteger(10), false};
    settings.workingColorSpaceId = "ACEScg";
    const auto added =
        fixture.run(std::make_unique<AddCompositionWithSettings>(settings), "Add Composition");
    const auto id = added.outputId<document::CompositionId>(kAddCompositionOutput);
    const auto* composition =
        id ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
    test.expect(added.changed() && composition != nullptr &&
                    compositionSettingsOf(*composition) == settings,
                "AddCompositionWithSettings creates a composition with every setting");
    CompositionSettingsChange change;
    change.height = 1350U;
    change.dropFrame = true;
    change.titleSafe = 0.7;
    const auto updated = fixture.run(std::make_unique<UpdateCompositionSettings>(*id, change));
    composition = fixture.document.snapshot().project().findComposition(*id);
    test.expect(updated.changed() && composition->format().width() == 1080 &&
                    composition->format().height() == 1350 && composition->timecode().dropFrame &&
                    composition->safeAreas().title == 0.7 &&
                    composition->safeAreas().action == settings.safeAreas.action &&
                    composition->name() == "Vertical",
                "UpdateCompositionSettings changes only the named settings");
    Transaction duplicate("Duplicate Composition", fixture.document.snapshot().revision());
    duplicate.emplace<DuplicateComposition>(*id);
    const auto copied = fixture.stack.execute(std::move(duplicate));
    const auto copyId = copied.outputId<document::CompositionId>(kDuplicateCompositionOutput);
    const auto* copy =
        copyId ? fixture.document.snapshot().project().findComposition(*copyId) : nullptr;
    test.expect(copy != nullptr && copy->timecode() == composition->timecode(),
                "DuplicateComposition carries the timecode labelling");
}

// #557: a composition whose animation spans several curves (several properties on several layers)
// duplicates into an equal copy with fresh curve and keyframe ids, and the copy's curves are
// independent of the original's.
void testDuplicateCompositionWithManyAnimatedProperties(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto key = [&](const document::ParameterId parameter, const std::int64_t time,
                         const double value) {
        Transaction transaction("Key", document.snapshot().revision());
        transaction.emplace<SetKeyframeAtTimeForParameter>(
            kCompositionId, parameter, core::RationalTime::fromInteger(time), value);
        return stack.execute(std::move(transaction)).changed();
    };
    for (const auto parameter :
         {kOpacityId, kSecondOpacityId, kFirstRotationId, kSecondRotationId}) {
        test.expect(key(parameter, 0, 0.25) && key(parameter, 2, 0.75),
                    "fixture keys several parameters on several layers");
    }

    const auto before = document.snapshot();
    const auto* source = before.project().findComposition(kCompositionId);
    requireFixture(source != nullptr, "duplicate source must exist");
    test.expect(source->animationCurves().records().size() == 4,
                "the source has four animation curves");
    const auto normalized = [](const document::Composition& composition) {
        auto records = std::vector<document::AnimationCurveRecord>(
            composition.animationCurves().records().begin(),
            composition.animationCurves().records().end());
        for (auto& record : records)
            std::get<document::ScalarAnimationCurve>(record).id = document::AnimationCurveId{};
        for (auto& record : records)
            for (auto& keyframe : std::get<document::ScalarAnimationCurve>(record).keyframes)
                keyframe.id = document::KeyframeId{};
        return records;
    };
    const auto sourceCurves = normalized(*source);

    Transaction duplicate("Duplicate Composition", before.revision());
    duplicate.emplace<DuplicateComposition>(kCompositionId);
    const auto result = stack.execute(std::move(duplicate));
    const auto copyId = result.outputId<document::CompositionId>(kDuplicateCompositionOutput)
                            .value_or(document::CompositionId{});
    const auto after = document.snapshot();
    const auto* copy = after.project().findComposition(copyId);
    test.expect(result.changed() && copy != nullptr,
                "DuplicateComposition succeeds with several animated properties");
    if (copy == nullptr)
        return;
    test.expect(normalized(*copy) == sourceCurves,
                "the copy's animation equals the original's apart from ids");
    test.expect(std::ranges::none_of(copy->animationCurves().records(),
                                     [&](const auto& record) {
                                         return source->animationCurves().find(
                                                    document::animationCurveId(record)) != nullptr;
                                     }),
                "the copy's curves have fresh ids");
    std::set<document::KeyframeId> keyframeIds;
    for (const auto* composition : {source, copy})
        for (const auto& record : composition->animationCurves().records())
            for (const auto& keyframe : std::get<document::ScalarAnimationCurve>(record).keyframes)
                keyframeIds.insert(keyframe.id);
    test.expect(keyframeIds.size() == 16, "all sixteen keyframes carry distinct ids");

    // The copy's keys are its own: re-keying a copied parameter leaves the original alone.
    const auto copiedOpacity = std::ranges::find_if(
        copy->parameters().records(),
        [&](const auto& parameter) {
            return parameter.schemaKey == document::kOpacityParameterSchemaKey &&
                   std::holds_alternative<document::AnimationCurveSource>(parameter.source);
        });
    requireFixture(copiedOpacity != copy->parameters().records().end(),
                   "copy has animated opacity");
    Transaction edit("Edit Copy", after.revision());
    edit.emplace<SetKeyframeAtTimeForParameter>(copyId, copiedOpacity->id,
                                                core::RationalTime::fromInteger(2), 0.1);
    test.expect(stack.execute(std::move(edit)).changed(), "the copy's key is edited");
    const auto edited = document.snapshot();
    test.expect(normalized(*edited.project().findComposition(kCompositionId)) == sourceCurves &&
                    normalized(*edited.project().findComposition(copyId)) != sourceCurves,
                "editing the copy's keys leaves the original's curves unchanged");

    test.expect(stack.undo().changed() && stack.undo().changed() &&
                    document.snapshot().project().findComposition(copyId) == nullptr,
                "undo removes the edit and then the copy");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(copyId) != nullptr &&
                    normalized(*document.snapshot().project().findComposition(copyId)) ==
                        sourceCurves,
                "redo restores the copy with its animation");
}
// #557: every id the copy holds resolves inside the copy. Layer timing extents, parent links, track
// mattes, masks, stack entries, edges and parameter sources all point at the copy's own records,
// (compiling the copy is checked in tests/integration/layer_time_tests.cpp, which may depend on
// the runtime).
void testDuplicateCompositionRemapsEveryReference(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto run = [&](auto operation) {
        Transaction transaction("Fixture", document.snapshot().revision());
        static_cast<void>(transaction.add(std::move(operation)));
        return stack.execute(std::move(transaction)).changed();
    };
    // The first layer plays a source node of its own (a video layer); the second one plays its own
    // node too (an audio layer's source is likewise a node of the composition).
    test.expect(run(std::make_unique<SetLayerContentExtent>(
                    kCompositionId, kFirstLayerId, document::SourceLayerExtent{kFirstLayerNodeId})) &&
                    run(std::make_unique<SetLayerContentExtent>(
                        kCompositionId, kSecondLayerId,
                        document::SourceLayerExtent{kSecondLayerNodeId})),
                "fixture gives both layers a source extent");
    test.expect(run(std::make_unique<SetLayerParent>(kCompositionId, kFirstLayerId,
                                                     std::optional(kSecondLayerId))),
                "fixture links a parent");
    test.expect(run(std::make_unique<SetTrackMatte>(kCompositionId, kFirstLayerId,
                                                    document::TrackMatte{kSecondLayerId})),
                "fixture sets a track matte");
    document::PathValue path;
    path.editableContour().closed = true;
    path.editableContour().anchors = {{{0, 0}, {}, {}}, {{4, 0}, {}, {}}, {{4, 4}, {}, {}}};
    test.expect(run(std::make_unique<AddLayerMask>(kCompositionId, kFirstLayerId, path)),
                "fixture adds a mask");

    const auto before = document.snapshot();
    Transaction duplicate("Duplicate Composition", before.revision());
    duplicate.emplace<DuplicateComposition>(kCompositionId);
    const auto result = stack.execute(std::move(duplicate));
    const auto copyId = result.outputId<document::CompositionId>(kDuplicateCompositionOutput)
                            .value_or(document::CompositionId{});
    const auto after = document.snapshot();
    const auto* copy = after.project().findComposition(copyId);
    requireFixture(result.changed() && copy != nullptr, "the composition duplicates");

    std::set<document::LayerId> layers;
    std::set<document::LayerSlotId> slots;
    for (const auto& layer : copy->graph().layerOutputs())
        layers.insert(layer.layerId);
    for (const auto& stackNode : copy->graph().merges())
        for (const auto& entry : stackNode.entries())
            slots.insert(entry.slotId);
    const auto node = [&](const NodeId id) { return copy->graph().findNode(id) != nullptr; };
    const auto parameter = [&](const document::ParameterId id) {
        return copy->parameters().find(id) != nullptr;
    };
    const auto bindings = [&](const auto& list) {
        return std::ranges::all_of(
            list, [&](const auto& binding) { return parameter(binding.parameterId); });
    };

    bool resolved = true;
    for (const auto& layer : copy->graph().layerOutputs()) {
        resolved = resolved && node(layer.nodeId) && bindings(layer.parameters);
        if (layer.parent)
            resolved = resolved && layers.contains(*layer.parent);
        if (layer.trackMatte && layer.trackMatte->source.isValid())
            resolved = resolved && layers.contains(layer.trackMatte->source);
        if (layer.trackMatte && layer.trackMatte->node)
            resolved = resolved && node(layer.trackMatte->node->nodeId);
        if (const auto* extent = std::get_if<document::SourceLayerExtent>(&layer.timing.contentExtent))
            resolved = resolved && node(extent->sourceNodeId);
        if (layer.entrySlot)
            resolved = resolved && slots.contains(*layer.entrySlot);
        for (const auto& mask : layer.masks) {
            resolved = resolved && parameter(mask.path) && parameter(mask.feather) &&
                       parameter(mask.expansion) && parameter(mask.opacity);
            if (mask.matte)
                resolved = resolved && node(mask.matte->nodeId);
        }
    }
    for (const auto& record : copy->graph().nodes())
        resolved = resolved && bindings(record.parameters);
    for (const auto& edge : copy->graph().edges()) {
        resolved = resolved && node(edge.source.nodeId);
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination))
            resolved = resolved && node(input->nodeId);
        else if (const auto* stackInput = std::get_if<document::LayerStackInputRef>(&edge.destination))
            resolved = resolved && node(stackInput->stackNodeId) && slots.contains(stackInput->slotId);
    }
    for (const auto& stackNode : copy->graph().merges()) {
        resolved = resolved && node(stackNode.nodeId());
        for (const auto& entry : stackNode.entries())
            resolved = resolved && (!entry.layerId.isValid() || layers.contains(entry.layerId));
    }
    for (const auto& record : copy->parameters().records()) {
        if (const auto* curve = std::get_if<document::AnimationCurveSource>(&record.source))
            resolved = resolved && copy->animationCurves().find(curve->curveId) != nullptr;
        if (const auto* driver = std::get_if<document::DriverBindingSource>(&record.source))
            resolved = resolved && node(driver->sourceNodeId);
    }
    for (const auto& [id, layout] : copy->nodeLayout()) {
        static_cast<void>(layout);
        resolved = resolved && node(id);
    }
    test.expect(resolved, "every id in the copy resolves to a record of the copy");

    const auto* original = after.project().findComposition(kCompositionId);
    requireFixture(original != nullptr, "the original remains");
    const auto none = std::ranges::none_of(copy->graph().layerOutputs(), [&](const auto& layer) {
        const auto* extent = std::get_if<document::SourceLayerExtent>(&layer.timing.contentExtent);
        return original->graph().findNode(layer.nodeId) != nullptr ||
               original->graph().findLayer(layer.layerId) != nullptr ||
               (extent != nullptr && original->graph().findNode(extent->sourceNodeId) != nullptr) ||
               (layer.parent && original->graph().findLayer(*layer.parent) != nullptr) ||
               (layer.trackMatte && original->graph().findLayer(layer.trackMatte->source) != nullptr);
    });
    test.expect(none, "no layer id in the copy points into the original");
}

// #342: a group is made, edited and pointed at in single undo steps, and a group that would contain
// itself is refused with a message.
void testGroupDefinitionIsUndoableAndRefusesCycles(TestContext& test) {
    Document document(
        document::makeNewProject("Project", "Main", core::RationalTime::fromInteger(10)).project);
    CommandStack stack(document);
    const auto main = document::CompositionId::fromRaw(1);

    Transaction addInstance("Add Node Group", document.snapshot().revision());
    addInstance.emplace<AddNode>(main, std::string(document::kNodeGroupNodeType), Vec2d{0.0, 0.0});
    const auto added = stack.execute(std::move(addInstance));
    const auto instance =
        added.outputId<NodeId>(kAddNodeOutput).value_or(NodeId{});
    test.expect(added.changed() && instance.isValid(), "a Node Group can be added to a composition");

    Transaction create("Create Group", document.snapshot().revision());
    create.emplace<CreateGroupDefinition>(main, "Brighten", instance);
    const auto created = stack.execute(std::move(create));
    const auto group = created.outputId<document::CompositionId>(kCreateGroupDefinitionOutput)
                           .value_or(document::CompositionId{});
    const auto after = document.snapshot();
    const auto* definition = after.project().findComposition(group);
    test.expect(created.changed() && definition != nullptr &&
                    document::isNodeGroupDefinition(*definition) &&
                    document::findGroupInput(*definition) != nullptr && definition->name() == "Brighten",
                "creating a group adds a composition holding a Group Input and a Group Output");
    const auto* host = after.project().findComposition(main);
    const auto* node = host != nullptr ? host->graph().findNode(instance) : nullptr;
    test.expect(node != nullptr &&
                    document::nodeGroupReference(*node, host->parameters()) == group,
                "creating a group points the Node Group at it in the same step");
    test.expect(after.project().validate().ok(), "a project with a group is valid document truth");

    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(group) == nullptr,
                "creating a group is one undo step");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(group) != nullptr,
                "redo restores the group");

    // Editing the definition is an ordinary node edit on its composition: one step.
    const auto beforeEdit = document.snapshot();
    Transaction edit("Add Exposure", beforeEdit.revision());
    edit.emplace<AddNode>(group, "bloom.exposure", Vec2d{240.0, 0.0});
    test.expect(stack.execute(std::move(edit)).changed() &&
                    document.snapshot().project().findComposition(group)->graph().nodes().size() ==
                        definition->graph().nodes().size() + 1,
                "an edit inside a group is an ordinary node edit");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(group)->graph().nodes().size() ==
                        definition->graph().nodes().size(),
                "an edit inside a group is one undo step");

    // A Node Group inside the group naming the group would contain itself.
    Transaction inner("Add inner Node Group", document.snapshot().revision());
    inner.emplace<AddNode>(group, std::string(document::kNodeGroupNodeType), Vec2d{480.0, 240.0});
    const auto innerAdded = stack.execute(std::move(inner));
    const auto innerNode = innerAdded.outputId<NodeId>(kAddNodeOutput).value_or(NodeId{});
    const auto before = document.snapshot().revision();
    Transaction cycle("Name the group inside itself", before);
    cycle.emplace<SetNodeParameterValue>(group, innerNode, std::string(document::kNodeGroupReferenceRole),
                                         static_cast<std::int64_t>(group.value()));
    const auto refused = stack.execute(std::move(cycle));
    test.expect(!refused.changed() && document.snapshot().revision() == before,
                "a group cannot be made to contain itself");
}
} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testAddCompositionBuildsTopologyAndUndoRedo(test);
        bloom::commands::test::testNewCompositionStartsArranged(test);
        bloom::commands::test::testDeleteCompositionAllowsLastAndUndoRedo(test);
        bloom::commands::test::testDuplicateCompositionRemapsDeepDocumentState(test);
        bloom::commands::test::testDuplicateCompositionWithManyAnimatedProperties(test);
        bloom::commands::test::testDuplicateCompositionRemapsEveryReference(test);
        bloom::commands::test::testSafeAreaSettingsAreUndoableAndDuplicated(test);
        bloom::commands::test::testWorkingColorSpaceOverrideIsUndoable(test);
        bloom::commands::test::testCompositionSettingsAreOneUndoableCommand(test);
        bloom::commands::test::testCompositionSettingsRefuseInvalidValues(test);
        bloom::commands::test::testFrameRateAndDurationKeepLayerTiming(test);
        bloom::commands::test::testDurationShrinkClampsWorkArea(test);
        bloom::commands::test::testSizeChangeKeepsContentAtTheAnchor(test);
        bloom::commands::test::testAddAndUpdateCompositionSettings(test);
        bloom::commands::test::testGroupDefinitionIsUndoableAndRefusesCycles(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
