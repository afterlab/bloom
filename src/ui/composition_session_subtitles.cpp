// #752: the session's subtitle state: the shared cue selection, the last load's report per node and
// the background inspector. Session/UI state only; nothing here is saved or undone.

#include "subtitles_inspector.hpp"

#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>

#include <variant>

namespace bloom::ui {

void CompositionSession::selectCue(const document::NodeId node, const std::uint64_t cue) {
    const CueSelection next{node, cue};
    if (cueSelection_ == next)
        return;
    cueSelection_ = next;
    emit cueSelectionChanged();
}

void CompositionSession::clearCueSelection() {
    if (!cueSelection_)
        return;
    cueSelection_.reset();
    emit cueSelectionChanged();
}

std::optional<CueSelection> CompositionSession::selectedCue() const {
    const auto* current = composition();
    if (!cueSelection_ || current == nullptr)
        return std::nullopt;
    const auto parameter =
        nodeRoleParameter(*this, cueSelection_->node, document::kSubtitlesCuesRole);
    const auto* record = parameter ? current->parameters().find(*parameter) : nullptr;
    const auto* constant =
        record == nullptr ? nullptr : std::get_if<document::ConstantValueSource>(&record->source);
    const auto* list =
        constant == nullptr ? nullptr : std::get_if<document::CueListValue>(&constant->value);
    if (list == nullptr || list->findCue(cueSelection_->cue) == nullptr)
        return std::nullopt;
    return cueSelection_;
}

void CompositionSession::setSubtitlesLoadReport(const document::NodeId node, QStringList rows) {
    if (rows.isEmpty())
        subtitlesLoadReports_.erase(node);
    else
        subtitlesLoadReports_[node] = std::move(rows);
}

QStringList CompositionSession::subtitlesLoadReport(const document::NodeId node) const {
    const auto found = subtitlesLoadReports_.find(node);
    return found == subtitlesLoadReports_.end() ? QStringList{} : found->second;
}

SubtitlesInspector& CompositionSession::subtitlesInspector() {
    if (subtitlesInspector_ == nullptr)
        subtitlesInspector_ = new SubtitlesInspector(*this, this);
    return *subtitlesInspector_;
}

} // namespace bloom::ui
