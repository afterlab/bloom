#include <bloom/commands/subtitles_operations.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/core/int128.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/subtitles_nodes.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace bloom::commands {
namespace {

[[nodiscard]] OperationResult refuse(std::string message) {
    return OperationResult::rejected(OperationIssueCode::InvalidValue, std::move(message));
}

[[nodiscard]] std::string nodeText(const document::NodeId node) {
    return "node " + std::to_string(node.value());
}

[[nodiscard]] std::string seconds(const core::RationalTime time) {
    const bool negative = time < core::RationalTime{};
    const auto magnitude = negative ? *core::RationalTime::create(-time.numerator(),
                                                                 time.denominator())
                                    : time;
    return (negative ? "-" : "") + document::cueTimestampText(magnitude);
}

// The index of the cue a selector names, or the refusal.
[[nodiscard]] std::variant<std::size_t, OperationResult>
indexOf(const document::CueListValue& list, const CueSelector& selector,
        const document::NodeId node) {
    if (const auto* byId = std::get_if<CueById>(&selector)) {
        if (const auto index = list.indexOf(byId->id))
            return *index;
        return refuse("Cue " + std::to_string(byId->id) + " does not exist on " + nodeText(node) +
                      "; query kind=subtitles lists its cues");
    }
    const auto index = std::get<CueByIndex>(selector).index;
    if (index < list.cues.size())
        return index;
    return refuse("Index " + std::to_string(index) + " is past the last cue: " + nodeText(node) +
                  " has " + std::to_string(list.cues.size()) + " cues (indices start at 0)");
}

// The indices a selection names (every cue when it names none), sorted and unique.
[[nodiscard]] std::variant<std::vector<std::size_t>, OperationResult>
indicesOf(const document::CueListValue& list, const std::optional<std::vector<CueSelector>>& cues,
          const document::NodeId node) {
    std::set<std::size_t> chosen;
    if (!cues) {
        for (std::size_t index = 0; index < list.cues.size(); ++index)
            chosen.insert(index);
    } else {
        for (const auto& selector : *cues) {
            auto found = indexOf(list, selector, node);
            if (auto* refusal = std::get_if<OperationResult>(&found))
                return std::move(*refusal);
            chosen.insert(std::get<std::size_t>(found));
        }
    }
    return std::vector<std::size_t>(chosen.begin(), chosen.end());
}

// The list checked and put in order, or the refusal naming its first problem.
[[nodiscard]] std::variant<document::CueListValue, OperationResult>
finished(document::CueListValue list) {
    document::canonicalizeCueList(list);
    if (auto problem = list.problem())
        return refuse("The cues do not form a valid list: " + *problem);
    return list;
}

// n / d reduced, or nullopt past 64-bit terms.
[[nodiscard]] std::optional<core::RationalTime> reduced(const core::Int128 n, const core::Int128 d) {
    using core::Int128;
    auto a = n < Int128(0) ? -n : n;
    auto b = d;
    while (b != Int128(0)) {
        const auto r = a % b;
        a = b;
        b = r;
    }
    const auto common = a == Int128(0) ? Int128(1) : a;
    const auto numerator = n / common;
    const auto denominator = d / common;
    const Int128 limit = std::numeric_limits<std::int64_t>::max();
    if (numerator > limit || -numerator > limit || denominator > limit)
        return std::nullopt;
    return core::RationalTime::create(static_cast<std::int64_t>(numerator),
                                      static_cast<std::int64_t>(denominator));
}

// origin + (time - origin) * factor, exactly.
[[nodiscard]] std::optional<core::RationalTime> scaled(const core::RationalTime time,
                                                       const core::RationalTime factor,
                                                       const core::RationalTime origin) {
    using core::Int128;
    const Int128 tn = time.numerator(), td = time.denominator();
    const Int128 on = origin.numerator(), od = origin.denominator();
    const Int128 fn = factor.numerator(), fd = factor.denominator();
    // ((tn*od - on*td) * fn) / (td*od*fd) + on/od
    return reduced((tn * od - on * td) * fn * od + on * td * od * fd, td * od * fd * od);
}

[[nodiscard]] std::optional<core::RationalTime> plus(const core::RationalTime a,
                                                     const core::RationalTime b) {
    using core::Int128;
    return reduced(Int128(a.numerator()) * Int128(b.denominator()) +
                       Int128(b.numerator()) * Int128(a.denominator()),
                   Int128(a.denominator()) * Int128(b.denominator()));
}

} // namespace

std::variant<CueListLocation, OperationResult>
locateCueList(const document::Composition& composition, const document::NodeId node,
              const bool allowLinked) {
    const auto& graph = composition.graph();
    const auto* record = graph.findNode(node);
    if (record == nullptr)
        return refuse("Node " + std::to_string(node.value()) + " does not exist in composition " +
                      std::to_string(composition.id().value()));
    if (record->typeId != document::kSubtitlesNodeType)
        return refuse("Node " + std::to_string(node.value()) + " is not a Subtitles node");
    for (const auto& edge : graph.edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            input != nullptr && input->nodeId == node && input->port == document::kSubtitlesCuesRole)
            return refuse("The cues of " + nodeText(node) + " come from node " +
                          std::to_string(edge.source.nodeId.value()) +
                          "; edit them there, or disconnect that node from its Cues input");
    document::ParameterId parameter;
    for (const auto& binding : record->parameters)
        if (binding.role == document::kSubtitlesCuesRole)
            parameter = binding.parameterId;
    const auto* stored = parameter.isValid() ? composition.parameters().find(parameter) : nullptr;
    if (stored == nullptr)
        return refuse("Node " + std::to_string(node.value()) + " has no cue list to edit");
    if (const auto* driver = std::get_if<document::DriverBindingSource>(&stored->source))
        return refuse("The cues of " + nodeText(node) + " come from node " +
                      std::to_string(driver->sourceNodeId.value()) +
                      "; edit them there, or disconnect that node from its Cues input");
    const auto* constant = std::get_if<document::ConstantValueSource>(&stored->source);
    const auto* list =
        constant == nullptr ? nullptr : std::get_if<document::CueListValue>(&constant->value);
    if (list == nullptr)
        return refuse("Node " + std::to_string(node.value()) + " has no cue list to edit");
    if (!allowLinked)
        if (const auto* file = std::get_if<document::CueFileProvenance>(&list->provenance))
            return refuse("These cues follow " + file->path +
                          ". Edit the file, or Unlink to edit here.");
    return CueListLocation{parameter, list};
}

OperationResult CueListEdit::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Composition " + std::to_string(composition_.value()) +
                                             " does not exist");
    auto located = locateCueList(*composition, node_, editsLinkedList());
    if (auto* refusal = std::get_if<OperationResult>(&located))
        return std::move(*refusal);
    const auto location = std::get<CueListLocation>(located);
    std::vector<OperationOutput> outputs;
    auto edited = edit(*location.list, outputs);
    if (auto* refusal = std::get_if<OperationResult>(&edited))
        return std::move(*refusal);
    auto& list = std::get<document::CueListValue>(edited);
    const auto& current = *location.list;
    const auto withWarnings = [this](OperationResult result) {
        for (auto& warning : warnings())
            result.issues.push_back({OperationIssueCode::InvalidValue, std::move(warning)});
        return result;
    };
    // The cues added or changed (or every cue, for a load), then the cues removed.
    std::map<std::uint64_t, const document::Cue*> previous;
    for (const auto& cue : current.cues)
        previous.emplace(cue->id, cue.get());
    for (const auto& cue : list.cues) {
        const auto found = previous.find(cue->id);
        const bool same = found != previous.end() &&
                          (found->second == cue.get() || *found->second == *cue);
        if (found != previous.end())
            previous.erase(found);
        if (!same || reportsWholeList())
            outputs.push_back({std::string(kCueOutput), document::CueId::fromRaw(cue->id)});
    }
    for (const auto& [id, cue] : previous) {
        static_cast<void>(cue);
        outputs.push_back({std::string(kRemovedCueOutput), document::CueId::fromRaw(id)});
    }
    if (list == current && list.provenance == current.provenance &&
        list.nextCueId == current.nextCueId)
        return withWarnings(OperationResult::noChange(std::move(outputs)));
    auto written = SetParameterSource(composition_, location.parameter,
                                      document::ConstantValueSource{std::move(list)})
                       .apply(draft);
    if (written.status == OperationStatus::Rejected)
        return written;
    return withWarnings(OperationResult::applied(std::move(outputs)));
}

// --------------------------------------------------------------------------------------------

std::string_view SetCues::typeId() const noexcept { return "bloom.subtitles.set-cues"; }

std::variant<document::CueListValue, OperationResult>
SetCues::edit(const document::CueListValue& current, std::vector<OperationOutput>&) const {
    if (cues_.size() > document::kMaximumCues)
        return refuse("The list would hold " + std::to_string(cues_.size()) +
                      " cues; a list holds at most " + std::to_string(document::kMaximumCues));
    document::CueListValue list;
    list.language = language_.value_or(current.language);
    list.provenance = provenance_.value_or(document::CueProvenance{document::CueAuthoredProvenance{}});
    list.styles = styles_.value_or(current.styles);
    // Ids named in the request are kept; a cue whose id the current list holds with the same
    // content keeps sharing that record.
    std::set<std::uint64_t> named;
    std::uint64_t next = current.nextCueId;
    for (const auto& cue : cues_)
        if (cue.id != 0) {
            if (!named.insert(cue.id).second)
                return refuse("Two cues name the id " + std::to_string(cue.id) +
                              "; each cue's id is unique");
            next = std::max(next, cue.id + 1);
        }
    list.cues.reserve(cues_.size());
    for (const auto& cue : cues_) {
        if (cue.id != 0)
            if (const auto index = current.indexOf(cue.id);
                index && *current.cues[*index] == cue) {
                list.cues.push_back(current.cues[*index]);
                continue;
            }
        auto made = std::make_shared<document::Cue>(cue);
        if (made->id == 0)
            made->id = next++;
        list.cues.push_back(std::move(made));
    }
    list.nextCueId = next;
    return finished(std::move(list));
}

std::string_view InsertCue::typeId() const noexcept { return "bloom.subtitles.insert-cue"; }

std::variant<document::CueListValue, OperationResult>
InsertCue::edit(const document::CueListValue& current,
                std::vector<OperationOutput>& outputs) const {
    if (current.cues.size() >= document::kMaximumCues)
        return refuse("The list holds " + std::to_string(current.cues.size()) +
                      " cues; a list holds at most " + std::to_string(document::kMaximumCues));
    static_cast<void>(outputs); // the new cue is reported as the one "cue" output
    auto list = current;
    auto cue = std::make_shared<document::Cue>(cue_);
    cue->id = list.nextCueId++;
    list.cues.push_back(std::move(cue));
    return finished(std::move(list));
}

std::string_view EditCue::typeId() const noexcept { return "bloom.subtitles.edit-cue"; }

std::variant<document::CueListValue, OperationResult>
EditCue::edit(const document::CueListValue& current, std::vector<OperationOutput>&) const {
    auto found = indexOf(current, cue_, node_);
    if (auto* refusal = std::get_if<OperationResult>(&found))
        return std::move(*refusal);
    const auto index = std::get<std::size_t>(found);
    auto cue = std::make_shared<document::Cue>(*current.cues[index]);
    if (edit_.start)
        cue->start = *edit_.start;
    if (edit_.end)
        cue->end = *edit_.end;
    if (edit_.text)
        cue->text = *edit_.text;
    if (edit_.speaker)
        cue->speaker = *edit_.speaker;
    if (edit_.style)
        cue->style = *edit_.style;
    if (edit_.placement)
        cue->placement = *edit_.placement;
    if (*cue == *current.cues[index])
        return current;
    auto list = current;
    list.cues[index] = std::move(cue);
    return finished(std::move(list));
}

std::string_view RemoveCues::typeId() const noexcept { return "bloom.subtitles.remove-cues"; }

std::variant<document::CueListValue, OperationResult>
RemoveCues::edit(const document::CueListValue& current, std::vector<OperationOutput>&) const {
    if (cues_.empty())
        return refuse("Name the cues to remove (cues or indices)");
    auto chosen = indicesOf(current, cues_, node_);
    if (auto* refusal = std::get_if<OperationResult>(&chosen))
        return std::move(*refusal);
    const auto& indices = std::get<std::vector<std::size_t>>(chosen);
    auto list = current;
    list.cues.clear();
    std::size_t next = 0;
    for (std::size_t index = 0; index < current.cues.size(); ++index) {
        if (next < indices.size() && indices[next] == index) {
            ++next;
            continue;
        }
        list.cues.push_back(current.cues[index]);
    }
    return finished(std::move(list));
}

std::string_view ShiftCues::typeId() const noexcept { return "bloom.subtitles.shift-cues"; }

std::variant<document::CueListValue, OperationResult>
ShiftCues::edit(const document::CueListValue& current, std::vector<OperationOutput>&) const {
    auto chosen = indicesOf(current, cues_, node_);
    if (auto* refusal = std::get_if<OperationResult>(&chosen))
        return std::move(*refusal);
    if (by_ == core::RationalTime{})
        return current;
    auto list = current;
    for (const auto index : std::get<std::vector<std::size_t>>(chosen)) {
        auto cue = std::make_shared<document::Cue>(*current.cues[index]);
        const auto start = plus(cue->start, by_);
        const auto end = plus(cue->end, by_);
        if (!start || !end)
            return refuse("Shifting cue " + std::to_string(cue->id) + " by " + seconds(by_) +
                          " leaves the times Bloom can hold");
        if (*start < core::RationalTime{})
            return refuse("Shifting by " + seconds(by_) + " would start cue " +
                          std::to_string(cue->id) + " before 0 (it starts at " +
                          seconds(cue->start) + ")");
        cue->start = *start;
        cue->end = *end;
        list.cues[index] = std::move(cue);
    }
    return finished(std::move(list));
}

std::string_view ScaleCues::typeId() const noexcept { return "bloom.subtitles.scale-cues"; }

std::variant<document::CueListValue, OperationResult>
ScaleCues::edit(const document::CueListValue& current, std::vector<OperationOutput>&) const {
    if (!(core::RationalTime{} < factor_))
        return refuse("The scale factor is a positive ratio, such as [1001, 1000]");
    auto chosen = indicesOf(current, cues_, node_);
    if (auto* refusal = std::get_if<OperationResult>(&chosen))
        return std::move(*refusal);
    if (factor_ == core::RationalTime::fromInteger(1))
        return current;
    auto list = current;
    for (const auto index : std::get<std::vector<std::size_t>>(chosen)) {
        auto cue = std::make_shared<document::Cue>(*current.cues[index]);
        const auto start = scaled(cue->start, factor_, origin_);
        const auto end = scaled(cue->end, factor_, origin_);
        if (!start || !end)
            return refuse("Scaling cue " + std::to_string(cue->id) +
                          " leaves the times Bloom can hold exactly");
        if (*start < core::RationalTime{})
            return refuse("Scaling about " + seconds(origin_) + " would start cue " +
                          std::to_string(cue->id) + " before 0");
        cue->start = *start;
        cue->end = *end;
        list.cues[index] = std::move(cue);
    }
    return finished(std::move(list));
}

std::string_view ReplaceCues::typeId() const noexcept {
    return kind_ == Kind::Load ? "bloom.subtitles.load" : "bloom.subtitles.reload";
}

std::variant<document::CueListValue, OperationResult>
ReplaceCues::edit(const document::CueListValue& current, std::vector<OperationOutput>&) const {
    if (kind_ == Kind::Reload && !std::holds_alternative<document::CueFileProvenance>(current.provenance))
        return refuse("The cues of " + nodeText(node_) +
                      " are not linked to a file; load a file with link=true to follow it");
    auto list = cues_;
    document::reconcileCueIds(current, list);
    // A cue that did not change keeps sharing its record.
    for (auto& cue : list.cues)
        if (const auto index = current.indexOf(cue->id); index && *current.cues[*index] == *cue)
            cue = current.cues[*index];
    return finished(std::move(list));
}

std::string_view UnlinkCues::typeId() const noexcept { return "bloom.subtitles.unlink"; }

std::variant<document::CueListValue, OperationResult>
UnlinkCues::edit(const document::CueListValue& current, std::vector<OperationOutput>&) const {
    auto list = current;
    list.provenance = document::CueAuthoredProvenance{};
    return list;
}

} // namespace bloom::commands
