// #752: the cue-list edits (subtitles_operations.hpp). Each is one undo step that shares every cue
// it did not touch (asserted by pointer), keeps cue ids, proves its footprint from the changed
// cues' spans, and refuses a linked or connected list with its message.
#include "command_test_support.hpp"

#include <bloom/commands/subtitles_operations.hpp>
#include <bloom/document/subtitles_nodes.hpp>

#include <iostream>

namespace bloom::commands::test {
namespace {
using namespace document;
using core::RationalTime;

[[nodiscard]] RationalTime t(const std::int64_t seconds) { return RationalTime::fromInteger(seconds); }
[[nodiscard]] RationalTime ms(const std::int64_t value) { return *RationalTime::create(value, 1000); }

struct Fixture {
    Document document{makeProject()};
    CommandStack stack{document};
};

template <typename Op, typename... Args> CommandResult apply(Fixture& fixture, Args&&... args) {
    Transaction transaction("Edit", fixture.document.snapshot().revision());
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    return fixture.stack.execute(std::move(transaction));
}

[[nodiscard]] Cue cue(const std::uint64_t id, const RationalTime start, const RationalTime end,
                      std::string text) {
    Cue made;
    made.id = id;
    made.start = start;
    made.end = end;
    made.text = std::move(text);
    return made;
}

[[nodiscard]] CueListValue threeCues() {
    CueListValue list;
    list.cues = {std::make_shared<Cue>(cue(1, t(1), t(2), "one")),
                 std::make_shared<Cue>(cue(2, t(3), t(4), "two")),
                 std::make_shared<Cue>(cue(3, t(5), t(6), "three"))};
    list.nextCueId = 4;
    return list;
}

[[nodiscard]] const CueListValue& listOf(const Fixture& fixture, const NodeId node) {
    const auto& composition = test::composition(fixture.document.snapshot());
    auto located = locateCueList(composition, node, true);
    requireFixture(std::holds_alternative<CueListLocation>(located), "the node has a cue list");
    return *std::get<CueListLocation>(located).list;
}

// The ids of the outputs named `name`.
[[nodiscard]] std::vector<std::uint64_t> outputIds(const CommandResult& result,
                                                   const std::string_view name) {
    std::vector<std::uint64_t> ids;
    for (const auto& output : result.outputs)
        if (output.output.name == name)
            ids.push_back(std::visit([](const auto& id) { return id.value(); }, output.output.id));
    return ids;
}

[[nodiscard]] std::string issueOf(const CommandResult& result) {
    return result.operationFailures.empty() ? std::string{}
                                            : result.operationFailures.front().issue.message;
}

void describe(const CommandResult& result) {
    std::cerr << "  status=" << static_cast<int>(result.status) << " footprint=";
    if (!result.affectedTimes)
        std::cerr << "whole";
    else
        for (const auto& interval : result.affectedTimes->intervals)
            std::cerr << '[' << interval.start.toSeconds() << ',' << interval.end.toSeconds()
                      << ')';
    std::cerr << " issue=" << issueOf(result) << '\n';
}

[[nodiscard]] bool spans(const CommandResult& result,
                         const std::vector<std::pair<RationalTime, RationalTime>>& expected) {
    bool ok = result.changed() && result.affectedTimes.has_value() &&
              result.affectedTimes->intervals.size() == expected.size();
    for (std::size_t index = 0; ok && index < expected.size(); ++index)
        ok = result.affectedTimes->intervals[index].start == expected[index].first &&
             result.affectedTimes->intervals[index].end == expected[index].second;
    if (!ok)
        describe(result);
    return ok;
}

// Undo restores exactly the records the list held before the step; redo returns to `after`.
[[nodiscard]] bool undoRedo(Fixture& fixture, const NodeId node, const CueListValue& before) {
    const auto after = listOf(fixture, node);
    if (!fixture.stack.undo().changed())
        return false;
    const auto& undone = listOf(fixture, node);
    bool same = undone == before && undone.cues.size() == before.cues.size() &&
                undone.nextCueId == before.nextCueId && undone.provenance == before.provenance;
    for (std::size_t index = 0; same && index < before.cues.size(); ++index)
        same = undone.cues[index] == before.cues[index];
    return same && fixture.stack.redo().changed() && listOf(fixture, node) == after;
}

[[nodiscard]] NodeId addLayer(Fixture& fixture, CueListValue cues) {
    const auto added = apply<AddSubtitlesLayer>(fixture, std::string("Subtitles"), std::move(cues));
    const auto node = added.outputId<NodeId>("subtitlesNode");
    if (!node)
        describe(added);
    requireFixture(node.has_value(), "the subtitles layer adds");
    return *node;
}

void testAddLayer(TestContext& test) {
    Fixture fixture;
    const auto node = addLayer(fixture, threeCues());
    const auto& composition = test::composition(fixture.document.snapshot());
    const auto* record = composition.graph().findNode(node);
    test.expect(record != nullptr && record->typeId == kSubtitlesNodeType,
                "add-subtitles adds a Subtitles node");
    test.expect(composition.graph().layerStack().entries().size() == 3,
                "as a third layer on the stack");
    test.expect(listOf(fixture, node) == threeCues(), "holding the given cues");
    test.expect(fixture.stack.undo().changed() &&
                    test::composition(fixture.document.snapshot()).graph().findNode(node) == nullptr,
                "one undo step removes the layer");
    auto bad = threeCues();
    bad.cues.push_back(std::make_shared<Cue>(cue(1, t(7), t(8), "dup")));
    bad.nextCueId = 5;
    const auto refused = apply<AddSubtitlesLayer>(fixture, std::string("Bad"), bad);
    test.expect(!refused.changed() && issueOf(refused) ==
                                          "The cues do not form a valid list: two cues share an id",
                "an invalid list is refused with its problem");
}

void testEditsShareAndUndo(TestContext& test) {
    Fixture fixture;
    const auto node = addLayer(fixture, threeCues());
    const auto before = listOf(fixture, node); // a copy: its handles are the stored records

    // edit-cue: one record replaced, the others shared.
    CueEdit edit;
    edit.text = "TWO";
    const auto edited = apply<EditCue>(fixture, node, CueSelector{CueById{2}}, edit);
    test.expect(spans(edited, {{t(3), t(4)}}), "an edited cue's footprint is its span");
    test.expect(outputIds(edited, kCueOutput) == std::vector<std::uint64_t>{2} &&
                    outputIds(edited, kRemovedCueOutput).empty(),
                "an edit names the changed cue");
    const auto& after = listOf(fixture, node);
    test.expect(after.cues.size() == 3 && after.cues[0] == before.cues[0] &&
                    after.cues[2] == before.cues[2] && after.cues[1] != before.cues[1] &&
                    after.cues[1]->id == 2 && after.cues[1]->text == "TWO",
                "edit-cue keeps the id and shares every untouched cue (pointer equality)");
    test.expect(spans(fixture.stack.undo(), {{t(3), t(4)}}), "undo replays the footprint");
    const auto& undone = listOf(fixture, node);
    test.expect(undone.cues[0] == before.cues[0] && undone.cues[1] == before.cues[1] &&
                    undone.cues[2] == before.cues[2],
                "undo restores the very records the list held");
    test.expect(fixture.stack.redo().changed() && listOf(fixture, node).cues[1]->text == "TWO",
                "redo reapplies the edit");

    // Moving a cue changes its old and its new span.
    CueEdit move;
    move.start = t(7);
    move.end = t(8);
    test.expect(spans(apply<EditCue>(fixture, node, CueSelector{CueByIndex{0}}, move),
                      {{t(1), t(2)}, {t(7), t(8)}}),
                "a moved cue's footprint is its old and its new span");

    // insert-cue takes the next id and reports it.
    const auto inserted =
        apply<InsertCue>(fixture, node, cue(99, ms(8500), ms(9500), "four <i>4</i>"));
    test.expect(spans(inserted, {{ms(8500), ms(9500)}}), "an inserted cue's footprint is its span");
    test.expect(inserted.outputId<CueId>(kInsertCueOutput) == CueId::fromRaw(4) &&
                    listOf(fixture, node).nextCueId == 5,
                "insert-cue gives the list's next id (the given id is ignored) and reports it");
    test.expect(fixture.stack.undo().changed() && listOf(fixture, node).cues.size() == 3 &&
                    fixture.stack.redo().changed(),
                "insert-cue is one undo step");

    // remove-cues by id and by index.
    const auto beforeRemove = listOf(fixture, node);
    const auto removed = apply<RemoveCues>(
        fixture, node, std::vector<CueSelector>{CueById{4}, CueByIndex{0}});
    const auto& left = listOf(fixture, node);
    test.expect(left.cues.size() == 2 && left.cues[0]->id == 3 && left.cues[1]->id == 1 &&
                    left.nextCueId == 5,
                "remove-cues removes by id and by index and never reuses an id");
    test.expect(spans(removed, {{t(3), t(4)}, {ms(8500), ms(9500)}}),
                "the removed cues' spans are the footprint");
    test.expect(outputIds(removed, kRemovedCueOutput) == std::vector<std::uint64_t>{2, 4} &&
                    outputIds(removed, kCueOutput).empty(),
                "a removal names the removed cues");

    test.expect(undoRedo(fixture, node, beforeRemove), "remove-cues is one undo step");
    // shift-cues: every cue, exactly.
    const auto beforeShift = listOf(fixture, node);
    const auto shifted = apply<ShiftCues>(fixture, node, std::nullopt, ms(-500));
    const auto& moved = listOf(fixture, node);
    test.expect(moved.cues[0]->start == ms(4500) && moved.cues[1]->end == ms(7500),
                "shift-cues moves every cue by the exact amount");
    test.expect(spans(shifted, {{ms(4500), t(6)}, {ms(6500), t(8)}}),
                "the footprint is the union of old and new spans");
    test.expect(undoRedo(fixture, node, beforeShift), "shift-cues is one undo step");
    const auto early = apply<ShiftCues>(fixture, node, std::nullopt, t(-5));
    test.expect(!early.changed() &&
                    issueOf(early) == "Shifting by -00:00:05.000 would start cue 3 before 0 (it "
                                      "starts at 00:00:04.500)",
                "a shift past 0 is refused naming the cue");

    // scale-cues: an exact ratio about an origin.
    const auto beforeScale = listOf(fixture, node);
    const auto scaledResult = apply<ScaleCues>(fixture, node, std::nullopt,
                                               *RationalTime::create(1001, 1000), t(0));
    test.expect(scaledResult.changed() &&
                    listOf(fixture, node).cues[0]->start == *RationalTime::create(4504500, 1000000),
                "scale-cues multiplies times exactly (4.5 s x 1001/1000)");
    test.expect(undoRedo(fixture, node, beforeScale), "scale-cues is one undo step");
    const auto zero = apply<ScaleCues>(fixture, node, std::nullopt, RationalTime{}, t(0));
    test.expect(!zero.changed() &&
                    issueOf(zero) == "The scale factor is a positive ratio, such as [1001, 1000]",
                "a factor of zero is refused");
}

void testSetCuesAndIds(TestContext& test) {
    Fixture fixture;
    const auto node = addLayer(fixture, threeCues());
    const auto before = listOf(fixture, node);
    auto kept = cue(3, t(5), t(6), "three");
    auto fresh = cue(0, t(8), t(9), "new");
    const auto set = apply<SetCues>(fixture, node, std::vector<Cue>{kept, fresh}, std::nullopt,
                                    std::optional<std::string>("de"), std::nullopt);
    const auto& list = listOf(fixture, node);
    test.expect(set.changed() && list.cues.size() == 2 && list.cues[0] == before.cues[2] &&
                    list.cues[1]->id == 4 && list.language == "de",
                "set-cues keeps a named id (sharing an unchanged record) and numbers new cues");
    test.expect(spans(set, {{t(0), t(10)}}), "a language change can reach every frame");
    test.expect(undoRedo(fixture, node, before), "set-cues is one undo step");
    const auto duplicate =
        apply<SetCues>(fixture, node, std::vector<Cue>{cue(7, t(1), t(2), "a"), cue(7, t(3), t(4), "b")},
                       std::nullopt, std::nullopt, std::nullopt);
    test.expect(!duplicate.changed() &&
                    issueOf(duplicate) == "Two cues name the id 7; each cue's id is unique",
                "duplicate ids are refused");
    const auto backwards = apply<SetCues>(fixture, node, std::vector<Cue>{cue(0, t(4), t(3), "x")},
                                          std::nullopt, std::nullopt, std::nullopt);
    test.expect(!backwards.changed() &&
                    issueOf(backwards) ==
                        "The cues do not form a valid list: cue 5 ends at or before its start",
                "a cue ending before it starts is refused");
    const auto missing = apply<EditCue>(fixture, node, CueSelector{CueById{1}}, CueEdit{});
    test.expect(!missing.changed() &&
                    issueOf(missing) ==
                        "Cue 1 does not exist on node " + std::to_string(node.value()) +
                            "; query kind=subtitles lists its cues",
                "an edit of a missing cue is refused");
    const auto past = apply<RemoveCues>(fixture, node, std::vector<CueSelector>{CueByIndex{5}});
    test.expect(!past.changed() && issueOf(past) == "Index 5 is past the last cue: node " +
                                                        std::to_string(node.value()) +
                                                        " has 2 cues (indices start at 0)",
                "an index past the list is refused");
}

void testLinkedAndReload(TestContext& test) {
    Fixture fixture;
    auto linked = threeCues();
    CueFileProvenance file;
    file.path = "subtitles/show.srt";
    file.format = "srt";
    file.encoding = "utf-8";
    file.digest = "sha256:" + std::string(64, '0');
    linked.provenance = file;
    const auto node = addLayer(fixture, linked);
    CueEdit edit;
    edit.text = "x";
    const auto refused = apply<EditCue>(fixture, node, CueSelector{CueById{1}}, edit);
    test.expect(!refused.changed() && issueOf(refused) == "These cues follow subtitles/show.srt. "
                                                          "Edit the file, or Unlink to edit here.",
                "a linked list refuses edits");
    // A reload keeps the ids of cues that kept their times or their text.
    CueListValue read;
    read.provenance = file;
    read.cues = {std::make_shared<Cue>(cue(1, t(1), t(2), "one")),        // unchanged
                 std::make_shared<Cue>(cue(2, t(3), t(4), "TWO")),        // text fixed
                 std::make_shared<Cue>(cue(3, ms(5500), ms(6500), "three")), // retimed
                 std::make_shared<Cue>(cue(4, t(8), t(9), "four"))};      // new
    read.nextCueId = 5;
    const auto before = listOf(fixture, node);
    const auto reloaded = apply<ReplaceCues>(fixture, node, read, ReplaceCues::Kind::Reload,
                                             std::vector<std::string>{"dropped: NOTE blocks"});
    test.expect(outputIds(reloaded, kCueOutput) == std::vector<std::uint64_t>{1, 2, 3, 4},
                "a reload names every cue of the list it stored");
    test.expect(reloaded.warnings.size() == 1 &&
                    reloaded.warnings.front().issue.message == "dropped: NOTE blocks",
                "a reload publishes the read's report as warnings");
    const auto& list = listOf(fixture, node);
    test.expect(reloaded.changed() && list.cues.size() == 4 && list.cues[0] == before.cues[0] &&
                    list.cues[1]->id == 2 && list.cues[2]->id == 3 && list.cues[3]->id == 4 &&
                    list.nextCueId == 5,
                "reload keeps ids by times, then text, shares the unchanged cue, and numbers the "
                "new one after the list's next id");
    test.expect(spans(reloaded, {{t(3), t(4)}, {t(5), ms(6500)}, {t(8), t(9)}}),
                "the reload's footprint is the changed cues' spans");
    test.expect(fixture.stack.undo().changed() && listOf(fixture, node) == before,
                "a reload is one undo step");
    // Unlinking changes no pixels.
    const auto beforeUnlink = listOf(fixture, node);
    const auto unlinked = apply<UnlinkCues>(fixture, node);
    test.expect(undoRedo(fixture, node, beforeUnlink), "unlink is one undo step");
    test.expect(unlinked.changed() && unlinked.affectedTimes.has_value() &&
                    unlinked.affectedTimes->intervals.empty(),
                "unlink changes only the provenance: an empty footprint");
    test.expect(apply<EditCue>(fixture, node, CueSelector{CueById{1}}, edit).changed(),
                "an unlinked list is edited here");
    const auto notLinked = apply<ReplaceCues>(fixture, node, read, ReplaceCues::Kind::Reload);
    test.expect(!notLinked.changed() &&
                    issueOf(notLinked) ==
                        "The cues of node " + std::to_string(node.value()) +
                            " are not linked to a file; load a file with link=true to follow it",
                "reload refuses a list that is not linked");
}

void testConnected(TestContext& test) {
    Fixture fixture;
    const auto source = addLayer(fixture, threeCues());
    const auto target = addLayer(fixture, threeCues());
    const auto connected = apply<ConnectPorts>(
        fixture, OutputPortRef{source, std::string(kSubtitlesCuesPort)},
        InputPortRef{NodeInputRef{target, std::string(kSubtitlesCuesRole)}});
    test.expect(connected.changed(), "a Cues output connects to a Cues input");
    CueEdit edit;
    edit.text = "x";
    const auto refused = apply<EditCue>(fixture, target, CueSelector{CueById{1}}, edit);
    test.expect(!refused.changed() &&
                    issueOf(refused) == "The cues of node " + std::to_string(target.value()) +
                                            " come from node " + std::to_string(source.value()) +
                                            "; edit them there, or disconnect that node from its "
                                            "Cues input",
                "a connected list is refused naming its source");
    const auto read = apply<EditCue>(fixture, source, CueSelector{CueById{1}}, edit);
    test.expect(spans(read, {{t(0), t(10)}}),
                "a list whose Cues output is read changes every frame, not only the cue's span");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testAddLayer(test);
        bloom::commands::test::testEditsShareAndUndo(test);
        bloom::commands::test::testSetCuesAndIds(test);
        bloom::commands::test::testLinkedAndReload(test);
        bloom::commands::test::testConnected(test);
    } catch (const std::exception& error) {
        test.fail(error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
