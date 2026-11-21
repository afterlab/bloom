// #747 (D2): change subscriptions and coalesced events. events.subscribe gives a protocol session a
// subscription (a filter by kind and composition, and an interval); a transport delivers it through
// McpEventStream as notifications/bloom/events: one per interval at most, each the revision ranges
// and counts of what changed since the previous one, plus the steps of asynchronous transact jobs.
// The stream keeps no buffer of its own: a notification is built from the core's retained events
// when it is due, so every subscriber shares one bounded buffer, and one that falls behind it is
// told to reset (resetRequired) instead of being sent a backlog. The events tool's summary form is
// the same summary for one call.
#include "core_internal.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace bloom::mcp {

bool McpEventFilter::selectsKind(const std::string_view kind) const {
    return kinds.empty() || std::ranges::find(kinds, kind) != kinds.end();
}

bool McpEventFilter::selectsCompositions(const std::span<const std::uint64_t> named) const {
    if (compositions.empty() || named.empty())
        return true;
    return std::ranges::any_of(named, [&](const std::uint64_t composition) {
        return std::ranges::find(compositions, composition) != compositions.end();
    });
}

McpEventFilter eventFilter(yyjson_val* arguments) {
    McpEventFilter filter;
    if (auto* kinds = member(arguments, "kinds")) {
        if (!yyjson_is_arr(kinds))
            throw InvalidInput("kinds must be an array of event kinds");
        std::size_t index = 0;
        std::size_t count = 0;
        yyjson_val* item = nullptr;
        yyjson_arr_iter iterator = yyjson_arr_iter_with(kinds);
        while ((item = yyjson_arr_iter_next(&iterator)) != nullptr) {
            if (++count > kMcpEventKinds.size() + 1)
                throw InvalidInput("kinds lists at most " +
                                   std::to_string(kMcpEventKinds.size() + 1) + " kinds");
            const auto kind = string(item, 64);
            const bool known = kind == kMcpJobEventKind ||
                               std::ranges::find(kMcpEventKinds, kind) != kMcpEventKinds.end();
            if (!known) {
                std::string accepted;
                for (const auto name : kMcpEventKinds)
                    accepted += std::string(name) + ", ";
                accepted += kMcpJobEventKind;
                throw InvalidInput("kinds[" + std::to_string(index) + "] \"" + kind +
                                   "\" is not an event kind; use " + accepted);
            }
            if (std::ranges::find(filter.kinds, kind) == filter.kinds.end())
                filter.kinds.push_back(kind);
            ++index;
        }
        if (filter.kinds.empty())
            throw InvalidInput("kinds is empty; leave it out to select every kind");
    }
    if (auto* compositions = member(arguments, "compositions")) {
        if (!yyjson_is_arr(compositions))
            throw InvalidInput("compositions must be an array of composition ids");
        if (yyjson_arr_size(compositions) > kMcpEventsPageLimit)
            throw InvalidInput("compositions lists at most " + grouped(kMcpEventsPageLimit) +
                               " compositions");
        yyjson_val* item = nullptr;
        yyjson_arr_iter iterator = yyjson_arr_iter_with(compositions);
        while ((item = yyjson_arr_iter_next(&iterator)) != nullptr) {
            const auto composition = identifier(item);
            if (composition == 0)
                throw InvalidInput("A composition id is a positive integer");
            if (std::ranges::find(filter.compositions, composition) == filter.compositions.end())
                filter.compositions.push_back(composition);
        }
        if (filter.compositions.empty())
            throw InvalidInput("compositions is empty; leave it out to select every composition");
        std::ranges::sort(filter.compositions);
    }
    return filter;
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<std::string_view, 3> kCommandKinds{"revision", "history", "rejected"};

std::string_view kindOf(const McpEventEntry& entry) {
    if (const auto* command = std::get_if<McpRetainedCommand>(&entry))
        return kCommandKinds.at(static_cast<std::size_t>(command->kind));
    return std::get<McpNotice>(entry).kind;
}

std::uint64_t revisionOf(const McpEventEntry& entry) {
    if (const auto* command = std::get_if<McpRetainedCommand>(&entry))
        return command->result.afterRevision.value();
    return std::get<McpNotice>(entry).revision;
}

std::span<const std::uint64_t> compositionsOf(const McpEventEntry& entry, std::uint64_t& single) {
    if (const auto* command = std::get_if<McpRetainedCommand>(&entry))
        return command->compositions;
    const auto& notice = std::get<McpNotice>(entry);
    if (!notice.composition)
        return {};
    single = *notice.composition;
    return {&single, 1};
}

// An inclusive range of revisions with the number of events in it.
struct Range final {
    std::size_t count = 0;
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    void add(const std::uint64_t revision) {
        if (count == 0 || revision < from)
            from = revision;
        if (count == 0 || revision > to)
            to = revision;
        ++count;
    }
};

yyjson_mut_val* rangeValue(Json& out, const Range& range) {
    auto* value = out.object();
    out.set(value, "count", out.number(range.count));
    out.set(value, "fromRevision", out.number(range.from));
    out.set(value, "toRevision", out.number(range.to));
    return value;
}

// The newest steps of the followed transact jobs `subscription` has not seen, written into
// `into` as `jobs`. Called with eventsMutex held; returns how many it listed.
std::size_t appendJobs(const McpServerCore::State& core, Json& out, yyjson_mut_val* into,
                       McpSubscription& subscription) {
    if (!subscription.filter.selectsKind(kMcpJobEventKind) ||
        subscription.jobCursor >= core.jobVersion) {
        subscription.jobCursor = core.jobVersion;
        return 0;
    }
    std::vector<std::pair<std::uint64_t, const std::pair<const std::string, McpJobLine>*>> changed;
    for (const auto& line : core.jobLines)
        if (line.second.version > subscription.jobCursor)
            changed.emplace_back(line.second.version, &line);
    std::ranges::sort(changed, {}, &decltype(changed)::value_type::first);
    auto* jobs = out.array();
    for (const auto& [version, entry] : changed) {
        static_cast<void>(version);
        const auto& [id, line] = *entry;
        auto* job = out.object();
        out.set(job, "job", out.text(id));
        out.set(job, "mine", out.boolean(line.client == subscription.client));
        out.set(job, "state", out.text(jobStateName(line.state)));
        out.set(job, "phase", out.text(line.phase));
        out.set(job, "completed", out.real(line.progress.progress));
        if (line.progress.total)
            out.set(job, "total", out.real(*line.progress.total));
        if (!line.progress.message.empty())
            out.set(job, "message", out.text(line.progress.message));
        if (line.revision)
            out.set(job, "revision", out.number(*line.revision));
        if (!line.code.empty())
            out.set(job, "code", out.text(line.code));
        out.set(job, "finished", out.boolean(line.finished));
        if (line.cancellationRequested)
            out.set(job, "cancellationRequested", out.boolean(true));
        out.append(jobs, job);
    }
    subscription.jobCursor = core.jobVersion;
    if (!changed.empty())
        out.set(into, "jobs", jobs);
    return changed.size();
}

} // namespace

McpEventSummary summarizeEvents(const McpServerCore::State& core, Json& out, yyjson_mut_val* into,
                                const McpEventFilter& filter, std::uint64_t& after,
                                const std::uint64_t sinceRevision) {
    McpEventSummary summary;
    const auto& events = core.events;
    // The range this summary covers starts after the cursor it was given, also when a reset
    // skips part of it.
    const auto requested = after;
    std::string_view reason;
    // A cursor from before the project was replaced: the old project's events are gone.
    if (core.replaced && after < core.resetSequence) {
        reason = "project-replaced";
        after = core.resetSequence;
    }
    if (after > core.sequence) {
        // Not a sequence this server gave.
        reason = "cursor";
        after = core.sequence;
    } else if (after < core.sequence) {
        // Events after the cursor were trimmed from the retained events: the subscriber fell
        // further behind than the bounded buffer reaches.
        const auto first = events.empty() ? core.sequence + 1 : events.front().first;
        if (first > after + 1) {
            if (reason.empty())
                reason = "overflow";
            after = core.sequence;
        }
    }
    Range all;
    Range project;
    std::map<std::uint64_t, Range> compositions;
    std::map<std::string_view, std::size_t> kinds;
    std::array<std::size_t, commands::kCommandOriginCount> origins{};
    const auto start =
        events.empty() || after < events.front().first ? 0 : after + 1 - events.front().first;
    for (auto index = static_cast<std::size_t>(start); index < events.size(); ++index) {
        const auto& [sequence, entry] = events[index];
        if (sequence <= after)
            continue;
        const auto kind = kindOf(entry);
        if (!filter.selectsKind(kind))
            continue;
        const auto revision = revisionOf(entry);
        if (revision < sinceRevision)
            continue;
        std::uint64_t single = 0;
        const auto named = compositionsOf(entry, single);
        if (!filter.selectsCompositions(named))
            continue;
        all.add(revision);
        ++kinds[kind];
        if (const auto* command = std::get_if<McpRetainedCommand>(&entry))
            ++origins.at(static_cast<std::size_t>(command->result.origin));
        if (named.empty()) {
            project.add(revision);
            continue;
        }
        for (const auto composition : named)
            if (filter.compositions.empty() ||
                std::ranges::binary_search(filter.compositions, composition))
                compositions[composition].add(revision);
    }
    after = core.sequence;
    summary.count = all.count;
    summary.resetRequired = !reason.empty();
    out.set(into, "fromSequence", out.number(requested + 1));
    out.set(into, "toSequence", out.number(after));
    out.set(into, "resetRequired", out.boolean(summary.resetRequired));
    if (!reason.empty())
        out.set(into, "reason", out.text(reason));
    out.set(into, "count", out.number(all.count));
    if (all.count != 0) {
        out.set(into, "fromRevision", out.number(all.from));
        out.set(into, "toRevision", out.number(all.to));
    }
    auto* kindCounts = out.object();
    for (const auto& [kind, count] : kinds)
        out.set(kindCounts, kind, out.number(count));
    out.set(into, "kinds", kindCounts);
    auto* originCounts = out.object();
    constexpr std::array<commands::CommandOrigin, commands::kCommandOriginCount> originOrder{
        commands::CommandOrigin::User, commands::CommandOrigin::Agent,
        commands::CommandOrigin::Script, commands::CommandOrigin::Host};
    for (const auto origin : originOrder)
        if (const auto count = origins.at(static_cast<std::size_t>(origin)); count != 0)
            out.set(originCounts, commands::commandOriginName(origin), out.number(count));
    out.set(into, "origins", originCounts);
    auto* list = out.array();
    std::size_t listed = 0;
    for (const auto& [composition, range] : compositions) {
        if (listed == kMcpEventCompositions)
            break;
        auto* value = rangeValue(out, range);
        out.set(value, "composition", out.number(composition));
        out.append(list, value);
        ++listed;
    }
    out.set(into, "compositions", list);
    out.set(into, "compositionsOmitted", out.number(compositions.size() - listed));
    if (project.count != 0)
        out.set(into, "project", rangeValue(out, project));
    return summary;
}

// ---------------------------------------------------------------------------------------------
// events.subscribe and events.unsubscribe

namespace {

yyjson_mut_val* subscriptionValue(Json& out, const McpSubscription& subscription) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(true));
    out.set(value, "subscription", out.number(subscription.id));
    out.set(value, "method", out.text("notifications/bloom/events"));
    out.set(value, "sequence", out.number(subscription.cursor));
    out.set(value, "intervalMs",
            out.number(static_cast<std::uint64_t>(subscription.interval.count())));
    auto* kinds = out.array();
    for (const auto& kind : subscription.filter.kinds)
        out.append(kinds, out.text(kind));
    out.set(value, "kinds", subscription.filter.kinds.empty() ? out.null() : kinds);
    auto* compositions = out.array();
    for (const auto composition : subscription.filter.compositions)
        out.append(compositions, out.number(composition));
    out.set(value, "compositions",
            subscription.filter.compositions.empty() ? out.null() : compositions);
    return value;
}

} // namespace

yyjson_mut_val* eventsSubscribeTool(McpServerCore::State& core, Json& out,
                                    McpProtocolSession::State& session, yyjson_val* arguments) {
    members(arguments, {"kinds", "compositions", "intervalMs", "afterSequence"});
    auto filter = eventFilter(arguments);
    auto interval = kMcpEventIntervalDefault;
    if (auto* value = member(arguments, "intervalMs")) {
        const auto milliseconds = integer(value);
        if (milliseconds < static_cast<std::uint64_t>(kMcpEventIntervalMinimum.count()) ||
            milliseconds > static_cast<std::uint64_t>(kMcpEventIntervalMaximum.count()))
            throw InvalidInput(
                "intervalMs is " + grouped(milliseconds) + "; use " +
                grouped(static_cast<std::uint64_t>(kMcpEventIntervalMinimum.count())) + " to " +
                grouped(static_cast<std::uint64_t>(kMcpEventIntervalMaximum.count())));
        interval = std::chrono::milliseconds(milliseconds);
    }
    std::optional<std::uint64_t> after;
    if (auto* value = member(arguments, "afterSequence"))
        after = integer(value);
    std::shared_ptr<McpSubscription> subscription;
    {
        const std::lock_guard lock(session.mutex);
        if (!session.subscription) {
            session.subscription = std::make_shared<McpSubscription>();
            session.subscription->client = session.id;
        }
        subscription = session.subscription;
    }
    const std::lock_guard lock(core.eventsMutex);
    const bool created = subscription->id == 0;
    if (created) {
        subscription->id = core.nextSubscription++;
        subscription->cursor = core.sequence;
        subscription->jobCursor = core.jobVersion;
    }
    if (after) {
        if (*after > core.sequence)
            throw InvalidInput("afterSequence " + grouped(*after) +
                               " is in the future; the newest event is " + grouped(core.sequence));
        subscription->cursor = *after;
    }
    subscription->filter = std::move(filter);
    subscription->interval = interval;
    auto* value = subscriptionValue(out, *subscription);
    out.set(value, "created", out.boolean(created));
    core.eventsChanged.notify_all();
    return value;
}

yyjson_mut_val* eventsUnsubscribeTool(McpServerCore::State& core, Json& out,
                                      McpProtocolSession::State& session, yyjson_val* arguments) {
    if (arguments)
        members(arguments, {});
    std::shared_ptr<McpSubscription> subscription;
    {
        const std::lock_guard lock(session.mutex);
        subscription = std::exchange(session.subscription, nullptr);
    }
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(true));
    out.set(value, "ended", out.boolean(subscription != nullptr));
    if (subscription) {
        const std::lock_guard lock(core.eventsMutex);
        subscription->ended = true;
        out.set(value, "subscription", out.number(subscription->id));
        out.set(value, "sequence", out.number(subscription->cursor));
        core.eventsChanged.notify_all();
    }
    return value;
}

// ---------------------------------------------------------------------------------------------
// The stream

struct McpEventStream::State {
    McpServerCore::State* core = nullptr;
    std::shared_ptr<McpSubscription> subscription;
    std::uint64_t epoch = 0;
    bool ended = false;
};

McpEventStream::McpEventStream(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
McpEventStream::~McpEventStream() = default;

bool McpEventStream::ended() const {
    const std::lock_guard lock(state_->core->eventsMutex);
    return state_->ended;
}

std::optional<McpEventNotification> McpEventStream::next(const Clock::time_point until,
                                                         const std::stop_token& stop) {
    auto& stream = *state_;
    auto& core = *stream.core;
    auto& subscription = *stream.subscription;
    std::unique_lock lock(core.eventsMutex);
    const auto over = [&] {
        return core.closing || subscription.ended || subscription.streamEpoch != stream.epoch;
    };
    const auto pending = [&] {
        return subscription.cursor != core.sequence ||
               (subscription.jobCursor < core.jobVersion &&
                subscription.filter.selectsKind(kMcpJobEventKind));
    };
    while (true) {
        if (stop.stop_requested())
            return std::nullopt;
        if (over()) {
            stream.ended = true;
            return std::nullopt;
        }
        const auto now = Clock::now();
        if (pending()) {
            const auto due = subscription.lastSent + subscription.interval;
            if (now >= due) {
                Json out;
                auto* params = out.object();
                out.set(params, "subscription", out.number(subscription.id));
                const auto summary =
                    summarizeEvents(core, out, params, subscription.filter, subscription.cursor, 0);
                const auto jobs = appendJobs(core, out, params, subscription);
                if (summary.count == 0 && !summary.resetRequired && jobs == 0)
                    continue; // Nothing the filter selects; the cursors moved past it.
                out.set(params, "sequence", out.number(subscription.cursor));
                auto* message = out.object();
                out.set(message, "jsonrpc", out.text("2.0"));
                out.set(message, "method", out.text("notifications/bloom/events"));
                out.set(message, "params", params);
                subscription.lastSent = now;
                return McpEventNotification{.message = out.encode(message),
                                            .sequence = subscription.cursor};
            }
            if (now >= until)
                return std::nullopt;
            core.eventsChanged.wait_until(lock, stop, std::min(due, until), over);
            continue;
        }
        if (now >= until)
            return std::nullopt;
        core.eventsChanged.wait_until(lock, stop, until, [&] { return over() || pending(); });
    }
}

std::unique_ptr<McpEventStream>
McpServerCore::openEventStream(McpProtocolSession& session,
                               const std::optional<std::uint64_t> resumeAfter) {
    auto& core = *state_;
    std::shared_ptr<McpSubscription> subscription;
    {
        const std::lock_guard lock(session.state_->mutex);
        subscription = session.state_->subscription;
    }
    if (!subscription)
        return nullptr;
    auto state = std::make_unique<McpEventStream::State>();
    state->core = &core;
    state->subscription = subscription;
    {
        const std::lock_guard lock(core.eventsMutex);
        if (subscription->ended)
            return nullptr;
        if (resumeAfter)
            subscription->cursor = *resumeAfter;
        // A new stream reports what it resumes at once; the older stream ends.
        subscription->lastSent = {};
        state->epoch = ++subscription->streamEpoch;
        core.eventsChanged.notify_all();
    }
    return std::make_unique<McpEventStream>(std::move(state));
}

} // namespace bloom::mcp
