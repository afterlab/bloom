#pragma once

// #752: the cue tracks an export carries (docs/architecture/subtitles.md, "Export"). A host
// preparation pass over the compiled plan, with no pixel evaluation: for every output frame it
// walks the plan from its output the way the evaluator reaches operations (a Layer Output outside
// its range, or on an absent clock, reaches nothing; a nested composition is reached at its
// mapped time; every layer clock, speed and Time Remap is the evaluator's own resolution) and
// records which cues of which Subtitles nodes are active there. Runs of frames then coalesce into
// track cues on the output's timeline: start = (first frame - range start) / rate and end = (last
// frame + 1 - range start) / rate, so an interrupted run (a trimmed layer) splits into two cues
// and the track shows exactly the frames the picture shows.

#include <bloom/document/cue_list.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bloom::document {
class Project;
}

namespace bloom::runtime {

struct SubtitleTrackRequest final {
    // The plan the export renders (compiled from the same snapshot).
    std::shared_ptr<const CompiledCompositionPlan> plan;
    // Composition frames, inclusive.
    std::uint64_t firstFrame = 0;
    std::uint64_t lastFrame = 0;
    // Absent: every Subtitles node with Export as track on. Otherwise exactly these nodes.
    std::optional<std::vector<document::NodeId>> nodes;
};

// One node's track. `cues` is on the output's timeline (its first frame is time zero), canonical,
// with ids 1..n in order and the source list's styles; each cue keeps its markup, style and
// placement. When the node shows the speaker as a name prefix, the prefix is in the cue's text (as
// the picture draws it) and the speaker field is empty. Timestamp tags are moved to the first
// frame that shows the time they name, and dropped when the run's clock does not move forward.
struct SubtitleTrack final {
    document::NodeId node;
    // The node's Language, else the list's; a BCP 47 tag ("und" when neither names one).
    std::string language;
    // The node's Track name (may be empty).
    std::string title;
    document::CueListValue cues;
    // What each cue shows, parallel to cues.cues: its plain text (markup removed, references
    // decoded, lines joined by '\n').
    std::vector<std::string> plainText;
};

struct SubtitleTracks final {
    std::vector<SubtitleTrack> tracks;
    // Plain sentences for the export log: a named node that never reaches the output, a node that
    // shows no cue in the range.
    std::vector<std::string> notes;
};

struct SubtitleTrackFailure final {
    std::string message;
    bool cancelled = false;
};

using SubtitleTracksOutcome = std::variant<SubtitleTracks, SubtitleTrackFailure>;
// Frames walked so far, and the range's frame count.
using SubtitleTrackProgress = std::function<void(std::uint64_t completed, std::uint64_t total)>;

// Runs on the caller's thread (a task); checks `cancellation` every frame. Node settings (Export
// as track, Language, Track name) are read from `project`, the snapshot the plan was compiled
// from.
[[nodiscard]] SubtitleTracksOutcome
collectSubtitleTracks(const SubtitleTrackRequest& request, const document::Project& project,
                      const CancellationToken& cancellation,
                      const SubtitleTrackProgress& progress = {});

} // namespace bloom::runtime
