#include <bloom/ui/memory_budget_controller.hpp>

#include <bloom/runtime/memory_headroom.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QSettings>

#include <QPointer>

#include <algorithm>
#include <limits>
#include <utility>

namespace bloom::ui {
MemoryBudgetController::MemoryBudgetController(
    runtime::TaskScheduler& scheduler, TaskUiBridge& bridge, runtime::MemoryBudgetLedger& ledger,
    Sampler sampler, QObject* parent,
    std::optional<runtime::MemoryPressureMonitor::Options> pressure)
    : QObject(parent), scheduler_(scheduler), ledger_(ledger), sampler_(std::move(sampler)) {
    connect(&bridge, &TaskUiBridge::resultsAvailable, this, &MemoryBudgetController::consume);
    timer_.setInterval(5'000);
    connect(&timer_, &QTimer::timeout, this, &MemoryBudgetController::requestSample);
    timer_.start();
    // Queue initial admission until application consumers have connected. This is not a result
    // poll; subsequent delivery comes exclusively through the runtime completion bridge.
    QTimer::singleShot(0, this, &MemoryBudgetController::requestSample);
    reclaimRequest_->owner = this;
    if (pressure) {
        // The monitor thread never touches cache owners: it queues the sample to this object's
        // thread. beginShutdown() joins the monitor before this object dies, and Qt drops a
        // queued call whose context object was destroyed.
        pressureMonitor_ = std::make_unique<runtime::MemoryPressureMonitor>(
            ledger_,
            [this, queued = pressureQueued_](const runtime::MachineMemorySample& sample) {
                if (queued->exchange(true))
                    return;
                if (!QMetaObject::invokeMethod(
                        this,
                        [this, queued, sample] {
                            queued->store(false);
                            applyPressure(sample);
                        },
                        Qt::QueuedConnection))
                    queued->store(false);
            },
            *pressure);
    }
}

void MemoryBudgetController::applyPressure(const runtime::MachineMemorySample& sample) {
    if (closed_)
        return;
    static_cast<void>(ledger_.applyPressureSample(sample));
    ++pressureApplied_;
    emit pressureApplied();
}

void MemoryBudgetController::reclaimForWork(const std::size_t bytes) {
    queueReclaim(reclaimRequest_, bytes);
}

std::function<void(std::size_t)> MemoryBudgetController::workReclaimer() const {
    return [request = reclaimRequest_](const std::size_t bytes) { queueReclaim(request, bytes); };
}

void MemoryBudgetController::queueReclaim(const std::shared_ptr<ReclaimRequest>& request,
                                          const std::size_t bytes) {
    const std::lock_guard lock(request->mutex);
    if (request->owner == nullptr)
        return;
    request->bytes = std::max({request->bytes, bytes, kMinimumReclaimBytes});
    if (request->queued)
        return;
    auto* const owner = request->owner;
    request->queued = QMetaObject::invokeMethod(
        owner,
        [owner, request] {
            std::size_t pending = 0;
            {
                const std::lock_guard held(request->mutex);
                pending = std::exchange(request->bytes, 0);
                request->queued = false;
                if (request->owner == nullptr)
                    return;
            }
            if (owner->closed_)
                return;
            static_cast<void>(owner->ledger_.reclaim(pending));
            ++owner->reclaimApplied_;
            // The waiting work retries on the next sample; take it now, not in five seconds.
            owner->requestSample();
        },
        Qt::QueuedConnection);
}

void MemoryBudgetController::wakePressureMonitor() {
    if (pressureMonitor_)
        pressureMonitor_->wake();
}

MemoryBudgetController::~MemoryBudgetController() { beginShutdown(); }

void MemoryBudgetController::beginShutdown() {
    if (closed_)
        return;
    closed_ = true;
    {
        const std::lock_guard lock(reclaimRequest_->mutex);
        reclaimRequest_->owner = nullptr;
    }
    timer_.stop();
    if (pressureMonitor_)
        pressureMonitor_->stop();
    if (active_)
        handle_.cancel();
}

void MemoryBudgetController::requestSample() {
    if (closed_ || active_ || !scheduler_.isAccepting())
        return;
    runtime::TaskRequest request(
        "Sample memory cache allowance",
        {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Background, runtime::TaskExecutor::BlockingIo);
    auto submission = scheduler_.submit<Snapshot>(
        std::move(request), [sampler = sampler_](runtime::TaskContext& context) {
            if (context.cancellation().isCancellationRequested())
                return runtime::TaskResult<Snapshot>::cancelled();
            auto sample = sampler(context.cancellation());
            if (context.cancellation().isCancellationRequested())
                return runtime::TaskResult<Snapshot>::cancelled();
            return runtime::TaskResult<Snapshot>::succeeded(
                std::make_unique<runtime::MemoryBudgetSample>(std::move(sample)));
        });
    if (!submission.accepted())
        return; // No worker/result is retained; the bounded normal cadence may retry admission.
    handle_ = std::move(submission.handle);
    active_ = true;
    if (admitted_ != std::numeric_limits<std::uint64_t>::max())
        ++admitted_;
}

void MemoryBudgetController::consume() {
    if (!active_)
        return;
    auto result = handle_.tryTakeResult();
    if (!result)
        return;
    active_ = false;
    handle_ = {};
    if (closed_)
        return;
    runtime::MemoryBudgetSample sample;
    sample.process.sampledAt = runtime::MemoryBudgetLedger::Clock::now();
    if (result->state() == runtime::TaskState::Succeeded) {
        auto value = std::move(*result).value();
        if (value.has_value() && *value)
            sample = std::move(**value);
    }
    const QPointer guard(this);
    static_cast<void>(ledger_.applySample(std::move(sample)));
    if (guard)
        emit sampleApplied(); // An observer may delete this owner; access no member afterward.
}

MemoryBudgetController::Sampler
applicationMemorySampler(std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy) {
    return [policy = std::move(policy)](const runtime::CancellationToken& cancellation) {
        runtime::releaseFreedHostMemory();
        return policy->sampleHostMemory(cancellation);
    };
}

std::function<void()>
connectApplicationMemoryBudget(MemoryBudgetController& controller,
                               const std::shared_ptr<runtime::GpuProcessingPhasePolicy>& policy,
                               runtime::MemoryBudgetLedger& ledger, QObject* context) {
    QObject::connect(&controller, &MemoryBudgetController::sampleApplied, context,
                     [policy, &ledger] {
                         const QSettings settings;
                         const auto allocation =
                             cacheMemoryBudgetAllocationFromSettings(settings, ledger);
                         policy->applyHostSample(ledger.state(), ledger.physicalByteBudget(),
                                                 allocation);
                         ledger.setActiveWorkClaims(
                             static_cast<std::size_t>(policy->usableCapacity().hostClaimBytes));
                     });
    policy->addRetentionReclaimer(
        [reclaim = controller.workReclaimer()](const render::GpuProcessingPhaseDomain domain,
                                               const std::uint64_t shortfall) -> std::uint64_t {
            // Device waits have no ledger pool to evict.
            if (domain == render::GpuProcessingPhaseDomain::Host)
                reclaim(static_cast<std::size_t>(shortfall));
            return 0;
        });
    const QPointer guard(&controller);
    auto applyHeadroom = [policy, &ledger, guard] {
        const QSettings settings;
        const auto preferences = loadApplicationPreferences(settings);
        policy->setHeadroom(
            {.ramBytes = preferences.ramHeadroomBytes, .vramBytes = preferences.vramHeadroomBytes});
        // setRamHeadroom applies the new budgets at once against the last sample; the pressure
        // monitor then samples the machine now, so a larger headroom evicts within one interval.
        ledger.setRamHeadroom(static_cast<std::size_t>(preferences.ramHeadroomBytes));
        if (guard)
            guard->wakePressureMonitor();
    };
    applyHeadroom();
    // Physical memory, a process limit and the device-local size are measured after startup:
    // re-apply when a sample changes the computed default (a saved value still wins).
    QObject::connect(&controller, &MemoryBudgetController::sampleApplied, context,
                     [applyHeadroom, applied = std::pair{runtime::defaultRamHeadroomBytes(),
                                                         runtime::defaultVramHeadroomBytes()}]() mutable {
                         const std::pair current{runtime::defaultRamHeadroomBytes(),
                                                 runtime::defaultVramHeadroomBytes()};
                         if (current != applied) {
                             applied = current;
                             applyHeadroom();
                         }
                     });
    return applyHeadroom;
}
} // namespace bloom::ui
