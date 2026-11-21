#include <bloom/core/rational_time.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// bloom.runtime.compiled-cue-list (#752): the cue list value's rules and the compiled list the
// evaluation reads, docs/architecture/subtitles.md. The interval index answers what a brute-force
// scan answers, and the digest an operation key holds changes with render content only.
namespace {

namespace core = bloom::core;
namespace document = bloom::document;
namespace runtime = bloom::runtime;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

[[nodiscard]] document::CueHandle cue(const std::uint64_t id, const core::RationalTime start,
                                      const core::RationalTime end, std::string text) {
    auto made = std::make_shared<document::Cue>();
    made->id = id;
    made->start = start;
    made->end = end;
    made->text = std::move(text);
    return made;
}

[[nodiscard]] core::RationalTime seconds(const std::int64_t numerator,
                                         const std::int64_t denominator = 1) {
    return core::RationalTime::create(numerator, denominator).value();
}

// Overlapping cues of mixed lengths: a long cue under short ones, a cue that ends where the next
// starts, and two that start together.
[[nodiscard]] document::CueListValue overlapping() {
    document::CueListValue list;
    list.cues = {cue(1, seconds(0), seconds(10), "long"),     cue(2, seconds(1), seconds(2), "a"),
                 cue(3, seconds(2), seconds(3), "b"),         cue(4, seconds(5, 2), seconds(4), "c"),
                 cue(5, seconds(5, 2), seconds(7, 2), "d"),   cue(6, seconds(12), seconds(13), "e")};
    list.nextCueId = 7;
    document::canonicalizeCueList(list);
    return list;
}

void testActiveAtMatchesAScan() {
    const auto list = overlapping();
    expect(list.isValid(), "the overlapping fixture is a valid list");
    const runtime::CompiledCueList compiled(list);
    for (std::int64_t quarter = -2; quarter <= 60; ++quarter) {
        const auto time = seconds(quarter, 4);
        std::vector<std::size_t> scanned;
        for (std::size_t index = 0; index < list.cues.size(); ++index)
            if (!(time < list.cues[index]->start) && time < list.cues[index]->end)
                scanned.push_back(index);
        expect(compiled.activeAt(time) == scanned,
               "activeAt(" + std::to_string(quarter) + "/4 s) equals the scan");
    }
    // The end is exclusive: at 2 s cue "a" is gone and "b" shows.
    const auto atTwo = compiled.activeAt(seconds(2));
    bool hasA = false;
    bool hasB = false;
    for (const auto index : atTwo) {
        hasA = hasA || list.cues[index]->text == "a";
        hasB = hasB || list.cues[index]->text == "b";
    }
    expect(!hasA && hasB, "a cue stops on its end and the next starts there");
    expect(compiled.activeAt(seconds(11)).empty(), "nothing shows between cues");
    expect(runtime::CompiledCueList{}.activeAt(seconds(1)).empty(), "an empty list shows nothing");
}

void testDigestFollowsRenderContent() {
    const auto list = overlapping();
    const runtime::CompiledCueList compiled(list);
    expect(compiled.digest == runtime::CompiledCueList::digestOf(list),
           "the compiled digest is the list's digest");
    expect(compiled.digest != runtime::CompiledCueList::emptyDigest(),
           "a list with cues is not the empty digest");

    auto renamed = list;
    renamed.provenance = document::CueProducerProvenance{"transcriber", "1", "sha256:0"};
    renamed.nextCueId = 40;
    expect(runtime::CompiledCueList::digestOf(renamed) == compiled.digest,
           "provenance and the next id leave the digest (and the cache) alone");

    auto edited = list;
    auto changed = std::make_shared<document::Cue>(*edited.cues[2]);
    changed->text = "changed";
    edited.cues[2] = changed;
    expect(runtime::CompiledCueList::digestOf(edited) != compiled.digest,
           "a cue's text changes the digest");

    auto moved = list;
    auto later = std::make_shared<document::Cue>(*moved.cues[2]);
    later->end = seconds(73, 24); // 3 s and one frame at 24 fps
    moved.cues[2] = later;
    expect(runtime::CompiledCueList::digestOf(moved) != compiled.digest,
           "a cue's end changes the digest");

    auto language = list;
    language.language = "fr";
    expect(runtime::CompiledCueList::digestOf(language) != compiled.digest,
           "the language changes the digest");

    // Equal content held in other pointers compares equal, as a reopened project's list does.
    auto copied = list;
    for (auto& handle : copied.cues)
        handle = std::make_shared<document::Cue>(*handle);
    expect(runtime::CompiledCueList(copied) == compiled,
           "equal content in other cues compiles to an equal value");
}

void testValueRules() {
    expect(document::isAcceptedSocketConnection(document::SocketValueKind::CueList,
                                                document::SocketValueKind::CueList),
           "a cue list connects to a cue list");
    for (const auto other : {document::SocketValueKind::String, document::SocketValueKind::Scalar,
                             document::SocketValueKind::NumberList,
                             document::SocketValueKind::Image}) {
        expect(!document::isAcceptedSocketConnection(document::SocketValueKind::CueList, other),
               "a cue list connects to nothing else");
        expect(!document::isAcceptedSocketConnection(other, document::SocketValueKind::CueList),
               "nothing else connects to a cue list");
    }
    expect(!document::isAnimatableSchemaKey(document::kCueListValueParameterSchemaKey),
           "a cue list is never keyed");

    const auto refused = [](document::CueListValue list, const std::string_view rule) {
        const auto problem = list.problem();
        expect(problem.has_value(), std::string("refused: ") + std::string(rule));
        if (problem)
            std::cout << rule << ": " << *problem << '\n';
    };
    {
        document::CueListValue list;
        list.cues = {cue(1, seconds(-1), seconds(1), "x")};
        list.nextCueId = 2;
        refused(list, "a negative start");
    }
    {
        document::CueListValue list;
        list.cues = {cue(1, seconds(1), seconds(1), "x")};
        list.nextCueId = 2;
        refused(list, "an end at its start");
    }
    {
        document::CueListValue list;
        list.cues = {cue(1, seconds(0), seconds(1), std::string(document::kMaximumCueTextBytes + 1, 'x'))};
        list.nextCueId = 2;
        refused(list, "text past the byte limit");
    }
    {
        document::CueListValue list;
        list.cues = {cue(3, seconds(0), seconds(1), "x")};
        list.nextCueId = 3;
        refused(list, "a next id at or below the highest id");
    }
    {
        document::CueListValue list;
        list.cues = {cue(2, seconds(1), seconds(2), "x"), cue(1, seconds(0), seconds(1), "y")};
        list.nextCueId = 3;
        refused(list, "cues out of order");
        document::canonicalizeCueList(list);
        expect(list.isValid() && list.cues.front()->id == 1, "canonical order makes the list valid");
    }
    {
        document::CueListValue list;
        for (std::uint64_t id = 1; id <= document::kMaximumActiveCues + 1; ++id)
            list.cues.push_back(cue(id, seconds(0), seconds(4), "x"));
        list.nextCueId = document::kMaximumActiveCues + 2;
        refused(list, "more cues at once than a frame draws");
        list.cues.pop_back();
        list.nextCueId = document::kMaximumActiveCues + 1;
        expect(list.isValid(), "exactly the active-cue limit is valid");
    }
    {
        document::CueListValue list;
        list.cues.reserve(document::kMaximumCues + 1);
        for (std::uint64_t id = 1; id <= document::kMaximumCues + 1; ++id)
            list.cues.push_back(cue(id, seconds(static_cast<std::int64_t>(id)),
                                    seconds(static_cast<std::int64_t>(id) + 1), "x"));
        list.nextCueId = document::kMaximumCues + 2;
        refused(list, "more cues than a list holds");
        list.cues.pop_back();
        list.nextCueId = document::kMaximumCues + 1;
        expect(list.isValid(), "exactly the cue limit is valid");
    }
    {
        document::CueListValue list;
        for (std::size_t index = 0; index <= document::kMaximumCueStyles; ++index) {
            document::CueStyle style;
            style.name = "style" + std::to_string(1000 + index);
            list.styles.push_back(style);
        }
        refused(list, "more styles than a list holds");
    }
}

} // namespace

int main() {
    testActiveAtMatchesAScan();
    testDigestFollowsRenderContent();
    testValueRules();
    if (failures != 0)
        std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
