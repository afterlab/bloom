#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/profiler_snapshot.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include "task_scheduler_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace bloom::runtime {
namespace {

constexpr std::uint32_t kFairnessStep = 4;

[[nodiscard]] bool validPriority(const TaskPriority priority) noexcept {
    switch (priority) {
    case TaskPriority::Interactive:
    case TaskPriority::Visible:
    case TaskPriority::Foreground:
    case TaskPriority::Background:
        return true;
    }
    return false;
}

[[nodiscard]] std::uint32_t priorityBase(const TaskPriority priority) noexcept {
    switch (priority) {
    case TaskPriority::Interactive:
        return 0;
    case TaskPriority::Visible:
        return kFairnessStep;
    case TaskPriority::Foreground:
        return kFairnessStep * 2;
    case TaskPriority::Background:
        return kFairnessStep * 3;
    }
    return kFairnessStep * 3;
}

[[nodiscard]] std::uint32_t agingIncrement(const TaskPriority priority) noexcept {
    switch (priority) {
    case TaskPriority::Interactive:
        return 1;
    case TaskPriority::Visible:
        return 2;
    case TaskPriority::Foreground:
        return 3;
    case TaskPriority::Background:
        return 4;
    }
    return 4;
}

[[nodiscard]] bool cancellationRequested(const detail::TaskRecord& record) noexcept {
    const bool taskCancelled = record.cancellation->requested.load(std::memory_order_acquire);
    const bool groupCancelled = record.groupCancellation != nullptr &&
                                record.groupCancellation->requested.load(std::memory_order_acquire);
    return taskCancelled || groupCancelled;
}

[[nodiscard]] TaskSnapshot copySnapshot(const std::shared_ptr<detail::TaskRecord>& record) {
    std::lock_guard lock(record->mutex);
    TaskSnapshot result = record->snapshot;
    result.cancellationRequested = !isTerminal(result.state) && cancellationRequested(*record);
    return result;
}

void normalizeProgress(TaskProgress& next, const TaskProgress& previous) {
    if (next.phase.empty()) {
        next.phase = previous.phase.empty() ? "Working" : previous.phase;
    }
    if (next.phase == previous.phase) {
        next.completed = std::max(next.completed, previous.completed);
    }
    if (next.total.has_value() && *next.total < next.completed) {
        next.total = next.completed;
    }
}

} // namespace

namespace detail {

SchedulerState::SchedulerState(TaskSchedulerConfig schedulerConfig,
                               std::function<void(std::size_t)> startHook)
    : config(std::move(schedulerConfig)),
      legacyGpuRequestOwnedByteCapacity(
          config.gpuRequestOwnedByteCapacity.value_or(defaultGpuRequestOwnedByteCapacity())),
      workerStartHook(std::move(startHook)) {
    // One row-band pool per scheduler (docs/architecture/task-system.md, "Row bands"): the pixel
    // work of one frame is split across it, which is a different question from how many independent
    // tasks may run at once, so it gets its own bounded width rather than borrowing cpu.workers.
    if (config.rowBandWorkerCount != kSerialRowBandWorkers) {
        rowBands.emplace(config.rowBandWorkerCount);
    }
    cpu.capacity = config.cpuQueueCapacity;
    blockingIo.capacity = config.blockingIoQueueCapacity;
    gpu.capacity = config.gpuPendingQueueCapacity;
    cpu.pending.reserve(cpu.capacity);
    blockingIo.pending.reserve(blockingIo.capacity);
    gpu.pending.reserve(gpu.capacity);
    cancellationQueue.reserve(cpu.capacity + blockingIo.capacity + gpu.capacity);
    cpu.workers.reserve(config.cpuWorkerCount);
    blockingIo.workers.reserve(config.blockingIoWorkerCount);
    terminalRing.resize(config.terminalHistoryCapacity);
    const std::size_t activeAndQueuedCapacity =
        config.cpuQueueCapacity + config.blockingIoQueueCapacity + config.gpuAdmittedStateCapacity +
        config.cpuWorkerCount + config.blockingIoWorkerCount;
    running.reserve(config.cpuWorkerCount + config.blockingIoWorkerCount +
                    config.gpuLiveContinuationCapacity);
    records.reserve(activeAndQueuedCapacity + config.terminalHistoryCapacity);
    groups.reserve(config.groupRegistryCapacity);
}

SchedulerState::~SchedulerState() { shutdownAndJoin(); }

void SchedulerState::shutdownAndJoin() noexcept {
    requestShutdown();
    std::optional<GpuServiceGeneration> generation;
    std::uint64_t attachmentId = 0;
    {
        std::lock_guard lock(mutex);
        generation = gpuGeneration;
        attachmentId = gpuAttachmentId;
    }
    if (generation.has_value()) {
        static_cast<void>(forceGpuShutdown(*generation, attachmentId));
    }
    joinWorkers();
    completions.close();
}

void SchedulerState::startWorkers() {
    std::size_t workerIndex = 0;
    try {
        startExecutor(cpu, TaskExecutor::Cpu, config.cpuWorkerCount, workerIndex);
        startExecutor(blockingIo, TaskExecutor::BlockingIo, config.blockingIoWorkerCount,
                      workerIndex);
        if (workerStartHook) {
            workerStartHook(workerIndex);
        }
        finalizerWorker.emplace([this] { finalizerLoop(); });
    } catch (...) {
        requestShutdown();
        joinWorkers();
        throw;
    }
}

void SchedulerState::startExecutor(ExecutorState& target, const TaskExecutor kind,
                                   const std::size_t count, std::size_t& workerIndex) {
    for (std::size_t index = 0; index < count; ++index, ++workerIndex) {
        static_cast<void>(index);
        if (workerStartHook) {
            workerStartHook(workerIndex);
        }
        target.workers.emplace_back([this, kind] { workerLoop(kind); });
    }
}

void SchedulerState::joinWorkers() noexcept {
    cpu.workers.clear();
    blockingIo.workers.clear();
    finalizerWorker.reset();
}

void SchedulerState::finalizerLoop() noexcept {
    while (true) {
        std::shared_ptr<TaskRecord> record;
        {
            std::unique_lock lock(mutex);
            cancellationAvailable.wait(lock,
                                       [&] { return stopping || !cancellationQueue.empty(); });
            if (cancellationQueue.empty()) {
                if (stopping) {
                    return;
                }
                continue;
            }
            record = std::move(cancellationQueue.back());
            cancellationQueue.pop_back();
        }
        finalizeRemoved(record);
    }
}

SchedulerState::ExecutorState& SchedulerState::executor(const TaskExecutor kind) noexcept {
    switch (kind) {
    case TaskExecutor::Cpu:
        return cpu;
    case TaskExecutor::BlockingIo:
        return blockingIo;
    case TaskExecutor::Gpu:
        return gpu;
    }
    return cpu;
}

std::shared_ptr<TaskRecord> SchedulerState::takeNextLocked(ExecutorState& target) {
    const auto selected = std::min_element(
        target.pending.begin(), target.pending.end(), [](const auto& lhs, const auto& rhs) {
            const auto lhsScore = static_cast<std::int64_t>(priorityBase(lhs->snapshot.priority)) -
                                  static_cast<std::int64_t>(lhs->ageCredit);
            const auto rhsScore = static_cast<std::int64_t>(priorityBase(rhs->snapshot.priority)) -
                                  static_cast<std::int64_t>(rhs->ageCredit);
            return lhsScore == rhsScore ? lhs->snapshot.id < rhs->snapshot.id : lhsScore < rhsScore;
        });
    auto record = *selected;
    for (auto& pending : target.pending) {
        const std::uint32_t increment = agingIncrement(pending->snapshot.priority);
        if (pending != record &&
            pending->ageCredit <= std::numeric_limits<std::uint32_t>::max() - increment) {
            pending->ageCredit += increment;
        }
    }
    target.pending.erase(selected);
    return record;
}

void SchedulerState::workerLoop(const TaskExecutor kind) noexcept {
    auto& target = executor(kind);
    while (true) {
        std::shared_ptr<TaskRecord> record;
        std::chrono::steady_clock::time_point started;
        {
            std::unique_lock lock(mutex);
            target.available.wait(lock, [&] { return stopping || !target.pending.empty(); });
            if (target.pending.empty()) {
                if (stopping) {
                    return;
                }
                continue;
            }
            record = takeNextLocked(target);
            ++target.active;
            running.push_back(record);
            std::lock_guard recordLock(record->mutex);
            record->snapshot.state = TaskState::Running;
            started = std::chrono::steady_clock::now();
            record->snapshot.startedAt = started;
            record->snapshot.progress.phase = std::move(record->runningPhase);
        }

        groupStarted(record->group);
        TaskState outcome = TaskState::Failed;
        try {
            if (cancellationRequested(*record)) {
                outcome = TaskState::Cancelled;
            } else {
                auto contextState = makeContextState(record);
                TaskContext context(std::move(contextState));
                outcome = record->work->execute(context);
            }
        } catch (...) {
            outcome = TaskState::Failed;
        }
        finishRunning(record, outcome);
        const auto busy = std::chrono::steady_clock::now() - started;
        target.busyNs.fetch_add(
            static_cast<std::uint64_t>(std::max<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(busy).count(), 0)),
            std::memory_order_relaxed);
    }
}

std::shared_ptr<TaskContextState>
SchedulerState::makeContextState(const std::shared_ptr<TaskRecord>& record) {
    auto context = std::make_shared<TaskContextState>();
    context->rowBands = rowBandExecutor();
    context->priority = [record] {
        std::lock_guard lock(record->mutex);
        return record->snapshot.priority;
    };
    context->taskCancellation = record->cancellation;
    context->groupCancellation = record->groupCancellation;
    context->reportProgress = [record](TaskProgress progress) {
        GroupProgressContribution previous;
        GroupProgressContribution next;
        {
            std::lock_guard lock(record->mutex);
            normalizeProgress(progress, record->snapshot.progress);
            previous = record->groupContribution;
            next = progressContribution(progress);
            record->snapshot.progress = std::move(progress);
            record->groupContribution = next;
        }
        groupProgress(record->group, previous, next);
    };
    context->addDiagnostic = [record,
                              limit = config.diagnosticsPerTask](TaskDiagnostic diagnostic) {
        std::lock_guard lock(record->mutex);
        if (record->snapshot.diagnostics.size() < limit) {
            record->snapshot.diagnostics.push_back(std::move(diagnostic));
        }
    };
    return context;
}

void SchedulerState::finishRunning(const std::shared_ptr<TaskRecord>& record,
                                   TaskState outcome) noexcept {
    {
        std::lock_guard lock(record->mutex);
        if (record->completionClaimed) {
            return;
        }
        record->completionClaimed = true;
        if (!isTerminal(outcome)) {
            outcome = TaskState::Failed;
        }
        if (outcome == TaskState::Succeeded && cancellationRequested(*record)) {
            outcome = TaskState::Cancelled;
        }
    }

    // The group counts the task as finished before its result becomes observable, so a caller that
    // took the result always reads the group's terminal aggregate (#708).
    groupFinished(record->group, TaskState::Running, record->groupContribution);
    const TaskState publishedOutcome = record->work->publish(outcome);
    publishTerminalSnapshot(record, publishedOutcome);
    {
        std::lock_guard lock(mutex);
        auto& target = executor(record->snapshot.executor);
        const auto runningRecord = std::ranges::find(running, record);
        if (runningRecord != running.end()) {
            running.erase(runningRecord);
        }
        if (target.active > 0) {
            --target.active;
        }
        retainTerminalLocked(record->snapshot.id);
    }
    completions.publish();
    cpu.available.notify_all();
    blockingIo.available.notify_all();
}

void SchedulerState::finalizeRemoved(const std::shared_ptr<TaskRecord>& record) noexcept {
    groupFinished(record->group, TaskState::Queued, record->groupContribution);
    const TaskState publishedOutcome = record->work->publish(TaskState::Cancelled);
    publishTerminalSnapshot(record, publishedOutcome);
    {
        std::lock_guard lock(mutex);
        releaseGpuAccountingLocked(record);
        if (finalizing > 0) {
            --finalizing;
        }
        retainTerminalLocked(record->snapshot.id);
    }
    completions.publish();
    cpu.available.notify_all();
    blockingIo.available.notify_all();
}

void SchedulerState::publishTerminalSnapshot(const std::shared_ptr<TaskRecord>& record,
                                             const TaskState outcome) noexcept {
    std::lock_guard lock(record->mutex);
    record->snapshot.state = outcome;
    record->snapshot.finishedAt = std::chrono::steady_clock::now();
    record->cancellation->completed.store(true, std::memory_order_release);
}

void SchedulerState::retainTerminalLocked(const TaskId id) noexcept {
    if (terminalCount == terminalRing.size()) {
        const TaskId expired = terminalRing[terminalHead];
        records.erase(expired);
        terminalRing[terminalHead] = id;
        terminalHead = (terminalHead + 1) % terminalRing.size();
        return;
    }
    const std::size_t position = (terminalHead + terminalCount) % terminalRing.size();
    terminalRing[position] = id;
    ++terminalCount;
}

bool SchedulerState::reprioritize(const TaskId id, const TaskPriority priority) noexcept {
    if (!validPriority(priority)) {
        return false;
    }
    std::shared_ptr<const GpuTaskWakeSink> wake;
    {
        std::lock_guard lock(mutex);
        const auto found = records.find(id);
        if (found == records.end()) {
            return false;
        }
        const auto& record = found->second;
        std::lock_guard recordLock(record->mutex);
        if (record->snapshot.state != TaskState::Queued || record->completionClaimed) {
            return false;
        }
        // Keep the aging credit the task has already earned: resetting it would let a promotion
        // schedule the task later than its old priority would have.
        record->snapshot.priority = priority;
        if (record->snapshot.executor == TaskExecutor::Gpu) {
            wake = gpuWakeLocked();
        } else {
            executor(record->snapshot.executor).available.notify_one();
        }
    }
    if (wake != nullptr) {
        try {
            (*wake)();
        } catch (...) {
            return true;
        }
    }
    return true;
}

std::vector<TaskSnapshot> SchedulerState::snapshots() const {
    std::vector<std::shared_ptr<TaskRecord>> retained;
    {
        std::lock_guard lock(mutex);
        retained.reserve(records.size());
        for (const auto& [id, record] : records) {
            static_cast<void>(id);
            retained.push_back(record);
        }
    }
    std::vector<TaskSnapshot> result;
    result.reserve(retained.size());
    for (const auto& record : retained) {
        result.push_back(copySnapshot(record));
    }
    std::ranges::sort(result, {}, &TaskSnapshot::id);
    return result;
}

void SchedulerState::copyProfilerTasks(
    ProfilerWorkload& workload, ProfilerCpu& executors,
    const std::chrono::steady_clock::time_point now) const noexcept {
    // Top-task candidates by (running before queued, oldest first). Records stay alive while the
    // scheduler lock is held, so the candidates keep raw pointers and copy fields in a second pass.
    struct Candidate {
        const TaskRecord* record = nullptr;
        int rank = 0;
        std::chrono::steady_clock::time_point queuedAt;
    };
    std::array<Candidate, kProfilerMaximumTasks> top{};
    std::size_t topCount = 0;
    const auto before = [](const Candidate& left, const Candidate& right) {
        return left.rank != right.rank ? left.rank < right.rank : left.queuedAt < right.queuedAt;
    };
    workload.tasksByState = {};
    workload.tasksByOwner = {};
    workload.tasksByExecutor = {};
    std::lock_guard lock(mutex);
    for (const auto& [id, record] : records) {
        static_cast<void>(id);
        Candidate candidate{.record = record.get(), .rank = 0, .queuedAt = {}};
        {
            std::lock_guard recordLock(record->mutex);
            const auto& task = record->snapshot;
            ++workload.tasksByState[static_cast<std::size_t>(task.state)];
            ++workload.tasksByOwner[static_cast<std::size_t>(task.owner.kind)];
            ++workload.tasksByExecutor[static_cast<std::size_t>(task.executor)];
            if (isTerminal(task.state))
                continue;
            candidate.rank = task.state == TaskState::Running ? 0 : 1;
            candidate.queuedAt = task.queuedAt;
        }
        if (topCount == top.size() && !before(candidate, top[topCount - 1]))
            continue;
        auto slot = topCount < top.size() ? topCount++ : top.size() - 1;
        while (slot > 0 && before(candidate, top[slot - 1])) {
            top[slot] = top[slot - 1];
            --slot;
        }
        top[slot] = candidate;
    }
    workload.taskCount = static_cast<std::uint32_t>(topCount);
    for (std::size_t index = 0; index < topCount; ++index) {
        const auto& record = *top[index].record;
        auto& out = workload.tasks[index];
        std::lock_guard recordLock(record.mutex);
        const auto& task = record.snapshot;
        setProfilerName(out.name, task.name);
        out.ownerKind = task.owner.kind;
        out.ownerId = task.owner.id.value();
        out.executor = task.executor;
        out.state = task.state;
        const auto& progress = task.progress;
        out.progress =
            progress.total && *progress.total > 0
                ? ProfilerRatio::of(static_cast<float>(
                      static_cast<double>(std::min(progress.completed, *progress.total)) /
                      static_cast<double>(*progress.total)))
                : ProfilerRatio{};
        out.ageNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - task.queuedAt).count();
    }
    struct Pool {
        std::string_view name;
        const ExecutorState& executor;
    };
    const std::array<Pool, 3> pools{
        {{"CPU", cpu}, {"Blocking I/O", blockingIo}, {"GPU submit", gpu}}};
    executors.executorPoolCount = 0;
    for (const auto& pool : pools) {
        if (executors.executorPoolCount == executors.executorPools.size())
            break;
        auto& out = executors.executorPools[executors.executorPoolCount++];
        setProfilerName(out.name, pool.name);
        out.workers = static_cast<std::uint32_t>(pool.executor.workers.size());
        out.queueDepth = static_cast<std::uint32_t>(pool.executor.pending.size());
        out.busyNs =
            ProfilerMetric<std::uint64_t>::of(pool.executor.busyNs.load(std::memory_order_relaxed));
    }
    // The CPU row-band pool's own workers (the calling thread's bands count as its task's time).
    if (rowBands.has_value() && executors.executorPoolCount < executors.executorPools.size()) {
        auto& out = executors.executorPools[executors.executorPoolCount++];
        setProfilerName(out.name, "Row bands");
        const auto activity = rowBands->activity();
        out.workers = static_cast<std::uint32_t>(rowBands->workerCount());
        out.queueDepth = static_cast<std::uint32_t>(activity.queuedBands);
        out.busyNs = ProfilerMetric<std::uint64_t>::of(activity.busyNs);
    }
}

std::optional<TaskSnapshot> SchedulerState::snapshot(const TaskId id) const {
    std::shared_ptr<TaskRecord> record;
    {
        std::lock_guard lock(mutex);
        const auto found = records.find(id);
        if (found == records.end()) {
            return std::nullopt;
        }
        record = found->second;
    }
    return copySnapshot(record);
}

void SchedulerState::requestShutdown() noexcept {
    std::shared_ptr<const GpuTaskWakeSink> wake;
    {
        std::lock_guard lock(mutex);
        if (stopping) {
            return;
        }
        accepting = false;
        stopping = true;
        for (const auto& record : running) {
            std::lock_guard recordLock(record->mutex);
            if (!record->completionClaimed && !isTerminal(record->snapshot.state)) {
                record->cancellation->requested.store(true, std::memory_order_release);
            }
        }
        for (auto* target : {&cpu, &blockingIo, &gpu}) {
            for (const auto& record : target->pending) {
                std::lock_guard recordLock(record->mutex);
                record->cancellation->requested.store(true, std::memory_order_release);
                record->completionClaimed = true;
                cancellationQueue.push_back(record);
                ++finalizing;
            }
            target->pending.clear();
        }
        for (const auto& [id, weakGroup] : groups) {
            static_cast<void>(id);
            if (const auto group = weakGroup.lock()) {
                group->cancellation->requested.store(true, std::memory_order_release);
            }
        }
        wake = gpuWakeLocked();
    }
    cpu.available.notify_all();
    blockingIo.available.notify_all();
    cancellationAvailable.notify_all();
    if (wake != nullptr) {
        try {
            (*wake)();
        } catch (...) {
            return;
        }
    }
}

bool SchedulerState::isAccepting() const noexcept {
    std::lock_guard lock(mutex);
    return accepting;
}

bool SchedulerState::isQuiescent() const noexcept {
    std::lock_guard lock(mutex);
    return cpu.pending.empty() && blockingIo.pending.empty() && gpu.pending.empty() &&
           cpu.active == 0 && blockingIo.active == 0 && gpuLiveContinuations == 0 &&
           finalizing == 0;
}

bool SchedulerState::hasActivePurgeableCacheConsumers() const noexcept {
    std::lock_guard lock(mutex);
    for (const auto& [id, record] : records) {
        static_cast<void>(id);
        std::lock_guard recordLock(record->mutex);
        if (record->snapshot.usesPurgeableCaches && !isTerminal(record->snapshot.state))
            return true;
    }
    return false;
}

} // namespace detail
} // namespace bloom::runtime
