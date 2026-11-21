#pragma once

// #752: the artist's cue-list edits, shared by the Properties cue list, the Timeline cue blocks and
// the Viewer (docs/architecture/subtitles.md, "Editing In The Application"). Every action is one
// transaction and so one undo step, built from the subtitle commands (subtitles_operations.hpp);
// a list that follows a file or comes from another node is refused by the command with its reason,
// which the session reports. Cue times are on the node's clock (the cue list's key time); the
// surfaces show and snap them on the composition's frames.

#include <bloom/commands/subtitles_operations.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/document/ids.hpp>

#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <vector>

namespace bloom::ui {
class CompositionSession;
}

namespace bloom::ui::subtitles_editing {

enum class CueSourceKind : std::uint8_t { Missing, Typed, Linked, Connected };

// Where a Subtitles node's cues come from, and why an edit in Bloom is refused.
struct CueSource final {
    document::NodeId node;
    document::ParameterId parameter;
    // The node's own list (typed or linked); null when another node supplies the cues.
    const document::CueListValue* list = nullptr;
    CueSourceKind kind = CueSourceKind::Missing;
    // The refusal an edit gets ("These cues follow movie.srt. ..."); empty for a typed list.
    QString refusal;
    // A linked list's file name.
    QString fileName;
};

[[nodiscard]] CueSource cueSource(const CompositionSession& session, document::NodeId node);
// The Subtitles node a layer shows (the node feeding its Layer, or its entry's content), if any.
[[nodiscard]] std::optional<document::NodeId> subtitlesNodeOfLayer(const CompositionSession& session,
                                                                   document::LayerId layer);

// The node card's and Source row's one-line summary: the cue count and where the cues come from
// ("12 cues · movie.srt, in sync"), with the whole story (a reload error) in `detail`.
[[nodiscard]] QString sourceSummary(CompositionSession& session, document::NodeId node,
                                    QString* detail = nullptr);

// The node clock. A cue time is a key time of the cue list (ADR 0024): the layer's time.
[[nodiscard]] std::optional<core::RationalTime>
compositionTimeOfCue(const CompositionSession& session, document::ParameterId parameter,
                     core::RationalTime cueTime);
[[nodiscard]] std::optional<core::RationalTime>
cueTimeOfCompositionTime(const CompositionSession& session, document::ParameterId parameter,
                         core::RationalTime time);
// The composition frame a cue time falls nearest, and a composition frame's time on the cue clock.
[[nodiscard]] std::int64_t compositionFrameOfCue(const CompositionSession& session,
                                                 document::ParameterId parameter,
                                                 core::RationalTime cueTime);
[[nodiscard]] std::optional<core::RationalTime>
cueTimeOfFrame(const CompositionSession& session, document::ParameterId parameter,
               std::int64_t frame);
// Whether times read as timecode (the Viewer's and Timeline's time reading), else frames.
[[nodiscard]] bool timecodeReading();
// A cue time in the composition's reading ("00:00:01:12" or "36"); a time before the composition
// starts reads as a negative frame count.
[[nodiscard]] QString cueTimeLabel(const CompositionSession& session, document::ParameterId parameter,
                                   core::RationalTime cueTime);
// A typed frame or timecode, as a cue time; nullopt for text that is neither.
[[nodiscard]] std::optional<core::RationalTime>
parseCueTimeLabel(const CompositionSession& session, document::ParameterId parameter,
                  const QString& text);
// A duration ("2:00" in timecode reading is not used: durations read as frames and seconds).
[[nodiscard]] QString cueDurationLabel(const CompositionSession& session, core::RationalTime start,
                                       core::RationalTime end);
// A cue's text without markup, its lines joined by " / " (a table cell, a block label).
[[nodiscard]] QString plainCueText(const document::Cue& cue, bool firstLineOnly = false);

// a + b and a - b exactly; nullopt past 64-bit terms.
[[nodiscard]] std::optional<core::RationalTime> addTimes(core::RationalTime a, core::RationalTime b);
[[nodiscard]] std::optional<core::RationalTime> subtractTimes(core::RationalTime a,
                                                              core::RationalTime b);

// The edits. Each runs one transaction; true when the list changed.
bool addCueAtPlayhead(CompositionSession& session, document::NodeId node,
                      std::uint64_t* added = nullptr);
bool removeCues(CompositionSession& session, document::NodeId node,
                const std::vector<std::uint64_t>& cues);
bool splitCueAtPlayhead(CompositionSession& session, document::NodeId node, std::uint64_t cue);
bool mergeCueWithNext(CompositionSession& session, document::NodeId node, std::uint64_t cue);
bool setCueStartToPlayhead(CompositionSession& session, document::NodeId node, std::uint64_t cue);
bool setCueEndToPlayhead(CompositionSession& session, document::NodeId node, std::uint64_t cue);
bool editCue(CompositionSession& session, document::NodeId node, std::uint64_t cue,
             commands::CueEdit edit, const QString& label);
// `cues` empty: every cue.
bool shiftCues(CompositionSession& session, document::NodeId node,
               const std::vector<std::uint64_t>& cues, core::RationalTime by);
bool scaleCues(CompositionSession& session, document::NodeId node,
               const std::vector<std::uint64_t>& cues, core::RationalTime factor,
               core::RationalTime origin);
// Edit as Text's Apply: the list read from the text, as one Load step (ids kept where cues match).
bool replaceCues(CompositionSession& session, document::NodeId node, document::CueListValue list,
                 const QString& label);
// A file read into the node (linked or not): one Load step; `report` becomes the Source row's
// report of the load.
bool loadCues(CompositionSession& session, document::NodeId node, document::CueListValue list,
              QStringList report);
bool reloadCues(CompositionSession& session, document::NodeId node, document::CueListValue list,
                QStringList report);
bool unlinkCues(CompositionSession& session, document::NodeId node);

} // namespace bloom::ui::subtitles_editing
