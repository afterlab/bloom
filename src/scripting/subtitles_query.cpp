// #752: the shared subtitles read (subtitles_query.hpp).

#include <bloom/scripting/subtitles_query.hpp>

#include <bloom/commands/subtitles_operations.hpp>
#include <bloom/core/int128.hpp>
#include <bloom/document/cue_markup.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/host/linked_subtitle_files.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include <algorithm>
#include <limits>
#include <charconv>
#include <functional>
#include <queue>
#include <string_view>
#include <variant>

namespace bloom::scripting {
namespace {

[[nodiscard]] SubtitlesReading refused(const SubtitlesQueryStatus status, std::string message) {
    SubtitlesReading reading;
    reading.status = status;
    reading.diagnostic = std::move(message);
    return reading;
}

[[nodiscard]] std::string_view provenanceKind(const document::CueProvenance& provenance) {
    if (std::holds_alternative<document::CueFileProvenance>(provenance))
        return "file";
    if (std::holds_alternative<document::CueProducerProvenance>(provenance))
        return "producer";
    return "authored";
}

[[nodiscard]] std::string_view stateName(const host::LinkedFileState state) {
    switch (state) {
    case host::LinkedFileState::InSync:
        return "in-sync";
    case host::LinkedFileState::Changed:
        return "changed";
    case host::LinkedFileState::Missing:
        return "missing";
    case host::LinkedFileState::Unreadable:
        return "unreadable";
    }
    return "none";
}

// "subtitles:<revision>:<node>:<index>".
[[nodiscard]] std::string cursorText(const std::uint64_t revision, const document::NodeId node,
                                     const std::size_t index) {
    return "subtitles:" + std::to_string(revision) + ":" + std::to_string(node.value()) + ":" +
           std::to_string(index);
}

[[nodiscard]] std::optional<std::size_t> cursorIndex(std::string_view cursor,
                                                     const std::uint64_t revision,
                                                     const document::NodeId node) {
    const auto prefix = "subtitles:" + std::to_string(revision) + ":" +
                        std::to_string(node.value()) + ":";
    if (!cursor.starts_with(prefix))
        return std::nullopt;
    cursor.remove_prefix(prefix.size());
    std::size_t index = 0;
    const auto [end, error] = std::from_chars(cursor.data(), cursor.data() + cursor.size(), index);
    if (error != std::errc{} || end != cursor.data() + cursor.size())
        return std::nullopt;
    return index;
}

// end - start, exactly.
[[nodiscard]] std::optional<core::RationalTime> difference(const core::RationalTime end,
                                                           const core::RationalTime start) {
    using core::Int128;
    const Int128 n = Int128(end.numerator()) * Int128(start.denominator()) -
                     Int128(start.numerator()) * Int128(end.denominator());
    const Int128 d = Int128(end.denominator()) * Int128(start.denominator());
    auto a = n < Int128(0) ? -n : n;
    auto b = d;
    while (b != Int128(0)) {
        const auto r = a % b;
        a = b;
        b = r;
    }
    const auto common = a == Int128(0) ? Int128(1) : a;
    const Int128 limit = std::numeric_limits<std::int64_t>::max();
    const auto numerator = n / common;
    const auto denominator = d / common;
    if (numerator > limit || -numerator > limit || denominator > limit)
        return std::nullopt;
    return core::RationalTime::create(static_cast<std::int64_t>(numerator),
                                      static_cast<std::int64_t>(denominator));
}

// How many cues show when each cue starts, the cue itself included.
[[nodiscard]] std::vector<std::size_t> showingAtStart(const document::CueListValue& list) {
    std::vector<std::size_t> showing(list.cues.size(), 0);
    std::priority_queue<core::RationalTime, std::vector<core::RationalTime>, std::greater<>> ends;
    for (std::size_t index = 0; index < list.cues.size(); ++index) {
        const auto& cue = *list.cues[index];
        while (!ends.empty() && ends.top() <= cue.start)
            ends.pop();
        ends.push(cue.end);
        showing[index] = ends.size();
    }
    return showing;
}

} // namespace

// "HH:MM:SS.mmm" or "HH:MM:SS,mmm" (hours any number of digits; a leading '-' negates it), exact
// in milliseconds.
std::optional<core::RationalTime> parseCueTimestamp(std::string_view text) {
    bool negative = false;
    if (text.starts_with('-')) {
        negative = true;
        text.remove_prefix(1);
    }
    const auto first = text.find(':');
    const auto second = first == std::string_view::npos ? first : text.find(':', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos || first == 0)
        return std::nullopt;
    const auto separator = text.find_first_of(".,", second + 1);
    if (separator == std::string_view::npos || text.size() - separator - 1 != 3 ||
        separator - second - 1 != 2 || second - first - 1 != 2)
        return std::nullopt;
    const auto number = [](const std::string_view digits) -> std::optional<std::int64_t> {
        if (digits.empty() || digits.size() > 9 ||
            !std::ranges::all_of(digits, [](const char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        std::int64_t value = 0;
        std::from_chars(digits.data(), digits.data() + digits.size(), value);
        return value;
    };
    const auto hours = number(text.substr(0, first));
    const auto minutes = number(text.substr(first + 1, 2));
    const auto seconds = number(text.substr(second + 1, 2));
    const auto milliseconds = number(text.substr(separator + 1));
    if (!hours || !minutes || !seconds || !milliseconds || *minutes > 59 || *seconds > 59)
        return std::nullopt;
    const auto total = ((*hours * 60 + *minutes) * 60 + *seconds) * 1000 + *milliseconds;
    return core::RationalTime::create(negative ? -total : total, 1000);
}

SubtitlesReading readSubtitles(const document::Snapshot& snapshot,
                               const std::filesystem::path& projectDirectory,
                               const SubtitlesQuery& query, const std::stop_token stop) {
    const auto& project = snapshot.project();
    const auto* composition = project.findComposition(query.composition);
    if (composition == nullptr)
        return refused(SubtitlesQueryStatus::MissingComposition,
                       "Composition " + std::to_string(query.composition.value()) +
                           " does not exist; query kind=compositions lists them");
    const auto revision = snapshot.revision().value();
    if (!query.node) {
        SubtitlesReading reading;
        reading.revision = revision;
        reading.frameRate = composition->format().frameRate();
        for (const auto& record : composition->graph().nodes()) {
            if (record.typeId != document::kSubtitlesNodeType)
                continue;
            SubtitleNodeSummary summary;
            summary.node = record.id;
            auto located = commands::locateCueList(*composition, record.id, true);
            if (const auto* location = std::get_if<commands::CueListLocation>(&located)) {
                summary.count = location->list->cues.size();
                summary.language = location->list->language;
                summary.provenance = std::string(provenanceKind(location->list->provenance));
            } else {
                summary.provenance = "connected";
            }
            reading.nodes.push_back(std::move(summary));
        }
        return reading;
    }
    const auto node = *query.node;
    const auto* record = composition->graph().findNode(node);
    if (record == nullptr)
        return refused(SubtitlesQueryStatus::MissingNode,
                       "Node " + std::to_string(node.value()) + " does not exist in composition " +
                           std::to_string(query.composition.value()) +
                           "; query kind=subtitles without a node lists the Subtitles nodes");
    if (record->typeId != document::kSubtitlesNodeType)
        return refused(SubtitlesQueryStatus::NotSubtitles,
                       "Node " + std::to_string(node.value()) + " is not a Subtitles node");
    auto located = commands::locateCueList(*composition, node, true);
    if (auto* refusal = std::get_if<commands::OperationResult>(&located)) {
        auto message = refusal->issues.empty() ? std::string("The node has no cue list")
                                               : refusal->issues.front().message;
        const auto cut = message.find("; edit them there");
        if (cut != std::string::npos)
            message = message.substr(0, cut) + "; query that node's output with kind=outputs";
        return refused(SubtitlesQueryStatus::Connected, std::move(message));
    }
    if (query.from && query.to && !(*query.from < *query.to))
        return refused(SubtitlesQueryStatus::InvalidRange,
                       "from must be before to; the range is [from, to)");
    const auto& list = *std::get<commands::CueListLocation>(located).list;

    SubtitlesReading reading;
    reading.revision = revision;
    reading.frameRate = composition->format().frameRate();
    reading.count = list.cues.size();
    reading.language = list.language;
    reading.provenance = list.provenance;
    reading.styles = list.styles;
    if (const auto* file = std::get_if<document::CueFileProvenance>(&list.provenance)) {
        const host::LinkedCueList linked{.composition = query.composition,
                                         .node = node,
                                         .provenance = *file,
                                         .path = subtitles::linkedFilePath(*file, projectDirectory)};
        reading.linkedFile = std::string(stateName(host::linkedFileState(linked, stop)));
    }
    if (query.format != SubtitlesQueryFormat::Json) {
        const auto written = subtitles::write(list, query.format == SubtitlesQueryFormat::Srt
                                                        ? subtitles::Format::Srt
                                                        : subtitles::Format::WebVtt);
        if (written) {
            reading.text = written->text;
            for (const auto& report : written->reports)
                reading.textWarnings.push_back(subtitles::describe(report));
        }
        return reading;
    }

    std::size_t first = 0;
    if (!query.cursor.empty()) {
        const auto index = cursorIndex(query.cursor, revision, node);
        if (!index)
            return refused(SubtitlesQueryStatus::InvalidCursor,
                           "The cursor is not one this list gave at revision " +
                               std::to_string(revision) +
                               "; the project changed or the cursor is from another query. "
                               "Query again without a cursor");
        first = *index;
    }
    const auto showing = showingAtStart(list);
    const auto pageSize =
        std::clamp<std::size_t>(query.pageSize, 1, kSubtitlesQueryMaximumPageSize);
    for (std::size_t index = 0; index < list.cues.size(); ++index) {
        const auto& cue = *list.cues[index];
        if (query.at && !(cue.start <= *query.at && *query.at < cue.end))
            continue;
        if (query.from && !(*query.from < cue.end))
            continue;
        if (query.to && !(cue.start < *query.to))
            continue;
        ++reading.matching;
        if (index < first)
            continue;
        if (reading.cues.size() == pageSize) {
            if (reading.nextCursor.empty())
                reading.nextCursor = cursorText(revision, node, index);
            continue;
        }
        SubtitleCueRow row;
        row.id = cue.id;
        row.index = index;
        row.start = cue.start;
        row.end = cue.end;
        row.duration = difference(cue.end, cue.start);
        row.timestamp =
            document::cueTimestampText(cue.start) + " --> " + document::cueTimestampText(cue.end);
        row.text = cue.text;
        row.plainText = document::cue_markup::plainText(cue.text);
        row.speaker = cue.speaker;
        row.style = cue.style;
        row.placement = cue.placement;
        if (!cue.style.empty() && list.findStyle(cue.style) == nullptr &&
            !document::isBuiltInCueStyleName(cue.style))
            row.warnings.push_back("The style \"" + cue.style +
                                   "\" is not one of the list's styles; the cue draws in the "
                                   "node's own style");
        if (showing[index] > document::kMaximumActiveCues)
            row.warnings.push_back(std::to_string(showing[index]) +
                                   " cues show when this one starts; a frame draws at most " +
                                   std::to_string(document::kMaximumActiveCues) +
                                   " (subtitleActiveCues)");
        reading.cues.push_back(std::move(row));
    }
    return reading;
}

} // namespace bloom::scripting
