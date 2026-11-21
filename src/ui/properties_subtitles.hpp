#pragma once

// #752: a Subtitles node's cues in Properties (docs/ux/interaction-model.md, "Subtitles"): the
// Source row (typed, linked file or another node; Load File, Reload, Unlink, the file's state and
// the last load's report), the cue list (a virtualized table of number, start, end, duration,
// text, speaker and checks, with a filter, edited in place), the cue actions (add at the playhead,
// delete, split, merge with the next, set start or end to the playhead, shift and scale timing)
// and Edit as Text (the list as SRT or WebVTT, parsed off the interface thread on Apply, errors
// listed inline, nothing written until it parses). Every edit is one undo step through
// subtitles_editing.hpp. The table builds no widget per cue: only an open cell editor is a widget.

#include <bloom/document/ids.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include <QWidget>

#include <filesystem>
#include <optional>
#include <string>

namespace bloom::ui {
class CompositionSession;

[[nodiscard]] QWidget* makeSubtitlesCuePanel(CompositionSession& session, document::NodeId node,
                                             QWidget* parent);
// What the Source row's Load File does once a file is chosen (its dialog is the kit file chooser):
// the file is read on the session's subtitle worker and stored as one Load step. `panel` is a
// widget makeSubtitlesCuePanel() returned.
void loadSubtitlesFile(QWidget& panel, std::filesystem::path path,
                       std::optional<subtitles::Format> format, std::string encoding, bool link);

} // namespace bloom::ui
