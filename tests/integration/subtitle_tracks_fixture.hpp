#pragma once
// #752: one composition with three Subtitles layers whose cues reach the output through different
// time paths, shared by the track pass test (bloom.runtime.subtitle-tracks) and the export test
// (bloom.host.subtitle-export):
//
// - Alpha (anchor Top): a layer that starts 0.5 s late (an offset), cues at layer time.
// - Bravo (anchor Center): a layer trimmed to [1.0, 2.5) s with an identity Time Remap enabled; its
//   first cue is cut at both ends and its second never shows.
// - Charlie (anchor Bottom): a Subtitles layer inside a nested composition, whose layer starts at
//   1.0 s and plays at twice the speed.
// - Delta: a fourth layer with Export as track off; it draws (anchor Bottom left, outside the
//   middle columns the test reads) but exports nothing.
//
// The composition is 256x144 at 24 fps, 4 s long. Every expected cue edge below is a whole frame.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/subtitles_nodes.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace bloom::test::subtitle_tracks {

inline constexpr std::uint32_t kWidth = 256;
inline constexpr std::uint32_t kHeight = 144;
inline constexpr std::int64_t kRate = 24;

[[nodiscard]] inline core::RationalTime at(const std::int64_t numerator,
                                           const std::int64_t denominator = 1) {
    return *core::RationalTime::create(numerator, denominator);
}

[[nodiscard]] inline document::CompositionFormat format() {
    return *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                                *document::FrameRate::create(kRate, 1));
}

[[nodiscard]] inline document::CueListValue
cues(const std::vector<std::tuple<core::RationalTime, core::RationalTime, std::string>>& items,
     const std::string& language) {
    document::CueListValue list;
    list.language = language;
    std::uint64_t id = 1;
    for (const auto& [start, end, text] : items) {
        auto cue = std::make_shared<document::Cue>();
        cue->id = id++;
        cue->start = start;
        cue->end = end;
        cue->text = text;
        list.cues.push_back(std::move(cue));
    }
    list.nextCueId = id;
    document::canonicalizeCueList(list);
    return list;
}

// One expected track cue in composition frames [first, end).
struct ExpectedCue final {
    std::uint64_t first = 0, end = 0;
    std::string text;
};

struct Fixture final {
    document::NewProject initial =
        document::makeNewProject("Subtitle tracks", "Main", at(4), format());
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId main = initial.initialCompositionId;
    document::CompositionId inner;
    document::NodeId alpha, bravo, charlie, delta;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("subtitle track fixture edit refused");
        return result;
    }

    [[nodiscard]] const document::Composition& composition(const document::CompositionId id) const {
        const auto* found = document.snapshot().project().findComposition(id);
        if (found == nullptr)
            throw std::runtime_error("fixture composition is missing");
        return *found;
    }
    [[nodiscard]] document::NodeId lastSubtitles(const document::CompositionId id) const {
        const auto nodes = composition(id).graph().nodes();
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
            if (it->typeId == document::kSubtitlesNodeType)
                return it->id;
        throw std::runtime_error("fixture Subtitles node is missing");
    }
    [[nodiscard]] document::LayerId lastLayer(const document::CompositionId id) const {
        return composition(id).graph().layerOutputs().back().layerId;
    }
    void set(const document::CompositionId id, const document::NodeId node,
             const std::string_view role, document::ParameterValue value) {
        const auto* record = composition(id).graph().findNode(node);
        if (record != nullptr)
            for (const auto& binding : record->parameters)
                if (binding.role == role) {
                    run<commands::SetParameterSource>(
                        id, binding.parameterId, document::ConstantValueSource{std::move(value)});
                    return;
                }
        throw std::runtime_error("fixture parameter is missing");
    }
    void style(const document::CompositionId id, const document::NodeId node,
               const std::int64_t anchor) {
        set(id, node, "safeArea", std::int64_t{document::kSubtitlesSafeFullFrame});
        set(id, node, "anchor", anchor);
        set(id, node, "size", 16.0);
    }

    Fixture() {
        // Alpha: starts 0.5 s late.
        run<commands::AddSubtitlesLayer>(
            main, "Alpha",
            cues({{at(1, 2), at(1), "Alpha one"}, {at(5, 4), at(2), "Alpha two"}}, "en"));
        alpha = lastSubtitles(main);
        style(main, alpha, 1);
        set(main, alpha, "trackName", std::string("Alpha"));
        run<commands::SetLayerTiming>(main, lastLayer(main), at(1, 2), at(1, 2), at(4));
        // Bravo: trimmed to [1.0, 2.5) s with an identity Time Remap.
        run<commands::AddSubtitlesLayer>(
            main, "Bravo",
            cues({{at(0), at(3), "Bravo long"}, {at(3), at(7, 2), "Bravo end"}}, "fr"));
        bravo = lastSubtitles(main);
        style(main, bravo, 4);
        run<commands::EnableLayerTimeRemap>(main, lastLayer(main));
        run<commands::SetLayerTiming>(main, lastLayer(main), at(0), at(1), at(5, 2));
        // Charlie: inside Inner (3 s), nested at 1.0 s at twice the speed.
        run<commands::AddComposition>("Inner", format(), at(3));
        inner = document.snapshot().project().compositions().back().id();
        run<commands::AddSubtitlesLayer>(
            inner, "Charlie",
            cues({{at(0), at(1, 2), "Charlie one"}, {at(3, 4), at(3, 2), "Charlie two"}}, "en"));
        charlie = lastSubtitles(inner);
        style(inner, charlie, 7);
        set(inner, charlie, "language", std::string("en"));
        run<commands::AddCompositionSource>(main, inner, document::Vec2d{}, true);
        run<commands::SetLayerTiming>(main, lastLayer(main), at(1), at(1), at(5, 2),
                                      *document::LayerSpeed::create(2, 1));
        // Delta: draws, exports no track.
        run<commands::AddSubtitlesLayer>(main, "Delta", cues({{at(0), at(4), "D"}}, "de"));
        delta = lastSubtitles(main);
        style(main, delta, 6);
        set(main, delta, "exportTrack", false);
    }

    // The cues each exported node shows, in composition frames.
    [[nodiscard]] static std::vector<ExpectedCue> expectedAlpha() {
        return {{24, 36, "Alpha one"}, {42, 60, "Alpha two"}};
    }
    [[nodiscard]] static std::vector<ExpectedCue> expectedBravo() {
        return {{24, 60, "Bravo long"}};
    }
    [[nodiscard]] static std::vector<ExpectedCue> expectedCharlie() {
        return {{24, 30, "Charlie one"}, {33, 42, "Charlie two"}};
    }
};

} // namespace bloom::test::subtitle_tracks
