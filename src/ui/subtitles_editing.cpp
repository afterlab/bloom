#include "subtitles_editing.hpp"

#include "composition_editor_support.hpp"
#include "subtitles_inspector.hpp"
#include "timeline_key_time.hpp"

#include <bloom/commands/transaction.hpp>
#include <bloom/core/int128.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QCoreApplication>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace bloom::ui::subtitles_editing {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("subtitles", text); }

bool run(CompositionSession& session, commands::Transaction&& transaction) {
    return session.executeTransaction(std::move(transaction)).changed();
}

commands::Transaction transactionFor(const CompositionSession& session, const QString& label) {
    return commands::Transaction(label.toStdString(), session.snapshot().revision());
}

const document::Cue* findCue(const CueSource& source, const std::uint64_t id) {
    return source.list == nullptr ? nullptr : source.list->findCue(id);
}

void reject(CompositionSession& session, const QString& message) {
    emit session.commandRejected(message);
}

} // namespace

CueSource cueSource(const CompositionSession& session, const document::NodeId node) {
    CueSource source;
    source.node = node;
    const auto* composition = session.composition();
    if (composition == nullptr)
        return source;
    auto located = commands::locateCueList(*composition, node, true);
    if (const auto* refusal = std::get_if<commands::OperationResult>(&located)) {
        if (const auto parameter = nodeRoleParameter(session, node, document::kSubtitlesCuesRole)) {
            source.parameter = *parameter;
            source.kind = CueSourceKind::Connected;
            source.refusal = refusal->issues.empty()
                                 ? tr("The cues come from another node")
                                 : QString::fromStdString(refusal->issues.front().message);
        }
        return source;
    }
    const auto location = std::get<commands::CueListLocation>(located);
    source.parameter = location.parameter;
    source.list = location.list;
    source.kind = CueSourceKind::Typed;
    if (const auto* file = std::get_if<document::CueFileProvenance>(&location.list->provenance)) {
        source.kind = CueSourceKind::Linked;
        const auto slash = file->path.find_last_of('/');
        source.fileName = QString::fromStdString(
            slash == std::string::npos ? file->path : file->path.substr(slash + 1));
        source.refusal = tr("These cues follow %1. Edit the file, or Unlink to edit here.")
                             .arg(QString::fromStdString(file->path));
    }
    return source;
}

QString sourceSummary(CompositionSession& session, const document::NodeId node, QString* detail) {
    const auto source = cueSource(session, node);
    const auto count = source.list == nullptr ? std::size_t{0} : source.list->cues.size();
    QString more;
    QString summary;
    switch (source.kind) {
    case CueSourceKind::Missing:
        summary = tr("No cues");
        break;
    case CueSourceKind::Connected:
        summary = tr("From another node");
        more = source.refusal;
        break;
    case CueSourceKind::Typed:
        summary = tr("%1 cues · typed").arg(count);
        break;
    case CueSourceKind::Linked: {
        QString state = tr("in sync");
        if (const auto* inspected = session.subtitlesInspector().result(node);
            inspected != nullptr && inspected->linked)
            switch (inspected->fileState) {
            case host::LinkedFileState::InSync:
                break;
            case host::LinkedFileState::Changed:
                state = tr("changed");
                more = inspected->fileMessage;
                break;
            case host::LinkedFileState::Missing:
                state = tr("missing");
                more = tr("The file is missing; the cues saved in the project are kept.");
                break;
            case host::LinkedFileState::Unreadable:
                state = tr("cannot be read");
                break;
            }
        summary = tr("%1 cues · %2, %3").arg(count).arg(source.fileName, state);
        break;
    }
    }
    if (detail != nullptr)
        *detail = more.isEmpty() ? summary : summary + QLatin1Char('\n') + more;
    return summary;
}

std::optional<document::NodeId> subtitlesNodeOfLayer(const CompositionSession& session,
                                                     const document::LayerId layer) {
    const auto* composition = session.composition();
    const auto node = session.directSourceNodeForLayer(layer);
    if (composition == nullptr || !node)
        return std::nullopt;
    const auto* record = composition->graph().findNode(*node);
    if (record == nullptr || record->typeId != document::kSubtitlesNodeType)
        return std::nullopt;
    return node;
}

std::optional<core::RationalTime> compositionTimeOfCue(const CompositionSession& session,
                                                       const document::ParameterId parameter,
                                                       const core::RationalTime cueTime) {
    return compositionTimeOfKey(session.keyClockForParameter(parameter), cueTime);
}

std::optional<core::RationalTime> cueTimeOfCompositionTime(const CompositionSession& session,
                                                           const document::ParameterId parameter,
                                                           const core::RationalTime time) {
    return keyTimeOfCompositionTime(session.keyClockForParameter(parameter), time);
}

std::int64_t compositionFrameOfCue(const CompositionSession& session,
                                   const document::ParameterId parameter,
                                   const core::RationalTime cueTime) {
    const auto* composition = session.composition();
    const auto time = compositionTimeOfCue(session, parameter, cueTime).value_or(cueTime);
    if (composition == nullptr)
        return 0;
    const auto rate = composition->format().frameRate();
    // A frame count stays far inside a double's exact integers (2^30 s at 240 fps is 2^38).
    return static_cast<std::int64_t>(std::llround(
        time.toSeconds() * static_cast<double>(rate.numerator()) / rate.denominator()));
}

std::optional<core::RationalTime> cueTimeOfFrame(const CompositionSession& session,
                                                 const document::ParameterId parameter,
                                                 const std::int64_t frame) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return std::nullopt;
    const auto rate = composition->format().frameRate();
    const auto time =
        core::RationalTime::create(frame * static_cast<std::int64_t>(rate.denominator()),
                                   static_cast<std::int64_t>(rate.numerator()));
    if (!time)
        return std::nullopt;
    return cueTimeOfCompositionTime(session, parameter, *time);
}

bool timecodeReading() {
    return QSettings().value(QStringLiteral("timeline/time-format"), QStringLiteral("frames")) ==
           QStringLiteral("timecode");
}

QString cueTimeLabel(const CompositionSession& session, const document::ParameterId parameter,
                     const core::RationalTime cueTime) {
    const auto context = frameContextFor(session);
    const auto frame = compositionFrameOfCue(session, parameter, cueTime);
    if (!context)
        return QString::number(frame);
    if (frame < 0)
        return QStringLiteral("-") + formatTimelineFrameLabel(static_cast<std::uint64_t>(-frame),
                                                              context->frameRate, timecodeReading(),
                                                              context->labels);
    return formatTimelineFrameLabel(static_cast<std::uint64_t>(frame), context->frameRate,
                                    timecodeReading(), context->labels);
}

std::optional<core::RationalTime> parseCueTimeLabel(const CompositionSession& session,
                                                    const document::ParameterId parameter,
                                                    const QString& text) {
    const auto context = frameContextFor(session);
    if (!context)
        return std::nullopt;
    const auto trimmed = text.trimmed();
    // Either reading is accepted whatever the display shows: a plain number is a frame.
    auto frame = parseTimelineFrameLabel(trimmed, context->frameRate, timecodeReading(),
                                         context->labels);
    if (!frame)
        frame = parseTimelineFrameLabel(trimmed, context->frameRate, !timecodeReading(),
                                        context->labels);
    if (!frame)
        return std::nullopt;
    return cueTimeOfFrame(session, parameter, static_cast<std::int64_t>(*frame));
}

QString cueDurationLabel(const CompositionSession& session, const core::RationalTime start,
                         const core::RationalTime end) {
    const auto* composition = session.composition();
    const double seconds = end.toSeconds() - start.toSeconds();
    if (composition == nullptr)
        return QString::number(seconds, 'f', 3);
    const auto rate = composition->format().frameRate();
    const double frames = seconds * static_cast<double>(rate.numerator()) / rate.denominator();
    return tr("%1 f · %2 s")
        .arg(QString::number(std::round(frames * 100.0) / 100.0))
        .arg(QString::number(seconds, 'f', 3));
}

QString plainCueText(const document::Cue& cue, const bool firstLineOnly) {
    QString text;
    bool tag = false;
    for (const auto character : QString::fromStdString(cue.text)) {
        if (character == QLatin1Char('<'))
            tag = true;
        else if (character == QLatin1Char('>'))
            tag = false;
        else if (!tag)
            text += character;
    }
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"))
        .replace(QStringLiteral("&lt;"), QStringLiteral("<"))
        .replace(QStringLiteral("&gt;"), QStringLiteral(">"))
        .replace(QStringLiteral("&nbsp;"), QStringLiteral(" "))
        .replace(QStringLiteral("&lrm;"), QString())
        .replace(QStringLiteral("&rlm;"), QString());
    const auto lines = text.split(QLatin1Char('\n'));
    return firstLineOnly ? lines.front() : lines.join(QStringLiteral(" / "));
}

namespace {
std::optional<core::RationalTime> reduced(core::Int128 numerator, core::Int128 denominator) {
    using core::Int128;
    auto a = numerator < Int128(0) ? -numerator : numerator;
    auto b = denominator;
    while (b != Int128(0)) {
        const auto r = a % b;
        a = b;
        b = r;
    }
    const auto common = a == Int128(0) ? Int128(1) : a;
    numerator = numerator / common;
    denominator = denominator / common;
    const Int128 limit = std::numeric_limits<std::int64_t>::max();
    if (numerator > limit || -numerator > limit || denominator > limit)
        return std::nullopt;
    return core::RationalTime::create(static_cast<std::int64_t>(numerator),
                                      static_cast<std::int64_t>(denominator));
}
} // namespace

std::optional<core::RationalTime> addTimes(const core::RationalTime a, const core::RationalTime b) {
    using core::Int128;
    return reduced(Int128(a.numerator()) * Int128(b.denominator()) +
                       Int128(b.numerator()) * Int128(a.denominator()),
                   Int128(a.denominator()) * Int128(b.denominator()));
}

std::optional<core::RationalTime> subtractTimes(const core::RationalTime a,
                                                const core::RationalTime b) {
    using core::Int128;
    return reduced(Int128(a.numerator()) * Int128(b.denominator()) -
                       Int128(b.numerator()) * Int128(a.denominator()),
                   Int128(a.denominator()) * Int128(b.denominator()));
}

bool addCueAtPlayhead(CompositionSession& session, const document::NodeId node,
                      std::uint64_t* added) {
    const auto source = cueSource(session, node);
    const auto* composition = session.composition();
    if (composition == nullptr || !source.parameter.isValid())
        return false;
    // Two seconds of frames from the playhead's frame, or up to the next cue when it starts
    // sooner (and after the playhead).
    const auto rate = composition->format().frameRate();
    const auto playheadFrame = static_cast<std::int64_t>(
        std::llround(session.currentTime().toSeconds() * rate.numerator() / rate.denominator()));
    const auto twoSeconds = static_cast<std::int64_t>(
        std::llround(2.0 * static_cast<double>(rate.numerator()) / rate.denominator()));
    const auto start = cueTimeOfFrame(session, source.parameter, playheadFrame);
    auto end = cueTimeOfFrame(session, source.parameter, playheadFrame + std::max<std::int64_t>(1, twoSeconds));
    if (!start || !end)
        return false;
    if (source.list != nullptr)
        for (const auto& cue : source.list->cues)
            if (*start < cue->start) {
                if (cue->start < *end)
                    end = cue->start;
                break;
            }
    document::Cue cue;
    cue.start = *start;
    cue.end = *end;
    auto transaction = transactionFor(session, tr("Add Cue"));
    transaction.emplace<commands::InsertCue>(session.compositionId(), node, std::move(cue));
    const auto result = session.executeTransaction(std::move(transaction));
    if (!result.changed())
        return false;
    if (const auto id = result.outputId<document::CueId>(commands::kInsertCueOutput)) {
        if (added != nullptr)
            *added = id->value();
        session.selectCue(node, id->value());
    }
    return true;
}

bool removeCues(CompositionSession& session, const document::NodeId node,
                const std::vector<std::uint64_t>& cues) {
    if (cues.empty())
        return false;
    std::vector<commands::CueSelector> selectors;
    for (const auto id : cues)
        selectors.push_back(commands::CueById{id});
    auto transaction = transactionFor(session, cues.size() == 1 ? tr("Delete Cue")
                                                                : tr("Delete Cues"));
    transaction.emplace<commands::RemoveCues>(session.compositionId(), node, std::move(selectors));
    return run(session, std::move(transaction));
}

bool splitCueAtPlayhead(CompositionSession& session, const document::NodeId node,
                        const std::uint64_t id) {
    const auto source = cueSource(session, node);
    const auto* cue = findCue(source, id);
    if (cue == nullptr)
        return false;
    const auto at = session.keyTimeAtPlayhead(source.parameter);
    if (!(cue->start < at && at < cue->end)) {
        reject(session, tr("The playhead is not inside cue %1, so it cannot be split there.")
                            .arg(source.list->indexOf(id).value_or(0) + 1));
        return false;
    }
    document::Cue second = *cue;
    second.start = at;
    commands::CueEdit first;
    first.end = at;
    auto transaction = transactionFor(session, tr("Split Cue"));
    transaction.emplace<commands::EditCue>(session.compositionId(), node, commands::CueById{id},
                                           std::move(first));
    transaction.emplace<commands::InsertCue>(session.compositionId(), node, std::move(second));
    return run(session, std::move(transaction));
}

bool mergeCueWithNext(CompositionSession& session, const document::NodeId node,
                      const std::uint64_t id) {
    const auto source = cueSource(session, node);
    const auto* cue = findCue(source, id);
    const auto index = source.list == nullptr ? std::nullopt : source.list->indexOf(id);
    if (cue == nullptr || !index)
        return false;
    if (*index + 1 >= source.list->cues.size()) {
        reject(session, tr("Cue %1 is the last cue; there is no next cue to merge it with.")
                            .arg(*index + 1));
        return false;
    }
    const auto& next = *source.list->cues[*index + 1];
    commands::CueEdit edit;
    edit.end = std::max(cue->end, next.end);
    edit.text = cue->text.empty() ? next.text
                                  : (next.text.empty() ? cue->text : cue->text + "\n" + next.text);
    auto transaction = transactionFor(session, tr("Merge Cues"));
    transaction.emplace<commands::EditCue>(session.compositionId(), node, commands::CueById{id},
                                           std::move(edit));
    transaction.emplace<commands::RemoveCues>(
        session.compositionId(), node,
        std::vector<commands::CueSelector>{commands::CueById{next.id}});
    return run(session, std::move(transaction));
}

bool setCueStartToPlayhead(CompositionSession& session, const document::NodeId node,
                           const std::uint64_t id) {
    const auto source = cueSource(session, node);
    if (!source.parameter.isValid())
        return false;
    commands::CueEdit edit;
    edit.start = session.keyTimeAtPlayhead(source.parameter);
    return editCue(session, node, id, std::move(edit), tr("Set Cue Start"));
}

bool setCueEndToPlayhead(CompositionSession& session, const document::NodeId node,
                         const std::uint64_t id) {
    const auto source = cueSource(session, node);
    if (!source.parameter.isValid())
        return false;
    commands::CueEdit edit;
    edit.end = session.keyTimeAtPlayhead(source.parameter);
    return editCue(session, node, id, std::move(edit), tr("Set Cue End"));
}

bool editCue(CompositionSession& session, const document::NodeId node, const std::uint64_t id,
             commands::CueEdit edit, const QString& label) {
    auto transaction = transactionFor(session, label);
    transaction.emplace<commands::EditCue>(session.compositionId(), node, commands::CueById{id},
                                           std::move(edit));
    return run(session, std::move(transaction));
}

namespace {
std::optional<std::vector<commands::CueSelector>> selectorsOf(const std::vector<std::uint64_t>& cues) {
    if (cues.empty())
        return std::nullopt;
    std::vector<commands::CueSelector> selectors;
    for (const auto id : cues)
        selectors.push_back(commands::CueById{id});
    return selectors;
}
} // namespace

bool shiftCues(CompositionSession& session, const document::NodeId node,
               const std::vector<std::uint64_t>& cues, const core::RationalTime by) {
    auto transaction = transactionFor(session, tr("Shift Cue Timing"));
    transaction.emplace<commands::ShiftCues>(session.compositionId(), node, selectorsOf(cues), by);
    return run(session, std::move(transaction));
}

bool scaleCues(CompositionSession& session, const document::NodeId node,
               const std::vector<std::uint64_t>& cues, const core::RationalTime factor,
               const core::RationalTime origin) {
    auto transaction = transactionFor(session, tr("Scale Cue Timing"));
    transaction.emplace<commands::ScaleCues>(session.compositionId(), node, selectorsOf(cues),
                                             factor, origin);
    return run(session, std::move(transaction));
}

bool replaceCues(CompositionSession& session, const document::NodeId node,
                 document::CueListValue list, const QString& label) {
    const auto source = cueSource(session, node);
    if (source.kind != CueSourceKind::Typed) {
        if (!source.refusal.isEmpty())
            reject(session, source.refusal);
        return false;
    }
    // The text keeps the list's language and styles it does not name.
    if (source.list != nullptr) {
        list.language = source.list->language;
        for (const auto& style : source.list->styles)
            if (list.findStyle(style.name) == nullptr)
                list.styles.push_back(style);
        document::canonicalizeCueList(list);
    }
    auto transaction = transactionFor(session, label);
    transaction.emplace<commands::ReplaceCues>(session.compositionId(), node, std::move(list),
                                               commands::ReplaceCues::Kind::Load);
    return run(session, std::move(transaction));
}

bool loadCues(CompositionSession& session, const document::NodeId node,
              document::CueListValue list, QStringList report) {
    std::vector<std::string> warnings;
    for (const auto& row : report)
        warnings.push_back(row.toStdString());
    auto transaction = transactionFor(session, tr("Load Subtitles"));
    transaction.emplace<commands::ReplaceCues>(session.compositionId(), node, std::move(list),
                                               commands::ReplaceCues::Kind::Load,
                                               std::move(warnings));
    if (!run(session, std::move(transaction)))
        return false;
    session.setSubtitlesLoadReport(node, std::move(report));
    return true;
}

bool reloadCues(CompositionSession& session, const document::NodeId node,
                document::CueListValue list, QStringList report) {
    std::vector<std::string> warnings;
    for (const auto& row : report)
        warnings.push_back(row.toStdString());
    auto transaction = transactionFor(session, tr("Reload Subtitles"));
    transaction.emplace<commands::ReplaceCues>(session.compositionId(), node, std::move(list),
                                               commands::ReplaceCues::Kind::Reload,
                                               std::move(warnings));
    const auto result = session.executeTransaction(std::move(transaction));
    // A file that did not change reloads to the same list: no step, but the report still stands.
    if (result.status == commands::CommandStatus::Rejected)
        return false;
    session.setSubtitlesLoadReport(node, std::move(report));
    return result.changed();
}

bool unlinkCues(CompositionSession& session, const document::NodeId node) {
    auto transaction = transactionFor(session, tr("Unlink Subtitles"));
    transaction.emplace<commands::UnlinkCues>(session.compositionId(), node);
    return run(session, std::move(transaction));
}

} // namespace bloom::ui::subtitles_editing
