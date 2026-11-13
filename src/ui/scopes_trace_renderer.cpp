#include <bloom/ui/scopes_trace_renderer.hpp>

#include <bloom/ui/task_ui_bridge.hpp>

#include <utility>

namespace bloom::ui {

ScopesTraceRenderer::ScopesTraceRenderer(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                         QObject* const parent)
    : QObject(parent), scheduler_(scheduler), bridge_(bridge) {
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &ScopesTraceRenderer::poll);
}

ScopesTraceRenderer::~ScopesTraceRenderer() {
    for (auto& pending : pending_)
        if (pending)
            pending->cancel();
}

bool ScopesTraceRenderer::busy() const noexcept {
    for (const auto& pending : pending_)
        if (pending)
            return true;
    return false;
}

bool ScopesTraceRenderer::request(const scopes::TraceRequest& request) {
    const auto slot = static_cast<std::size_t>(request.kind);
    if (pending_[slot]) {
        pending_[slot]->cancel();
        pending_[slot].reset();
    }
    if (bridge_.isShuttingDown())
        return false;
    runtime::TaskRequest task(
        "Map scope image",
        {.kind = runtime::TaskOwnerKind::PanelRequest,
         .id = runtime::TaskOwnerId::fromRaw(static_cast<std::uint64_t>(request.kind) + 1U)},
        runtime::TaskPriority::Visible);
    task.sourceVersion = {.documentRevision = 0, .requestGeneration = started_ + 1};
    auto submission = scheduler_.submit<FinishedHandle>(
        std::move(task), [request](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<FinishedHandle>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto trace = scopes::renderTrace(
                request, [&context] { return context.isCancellationRequested(); });
            if (context.isCancellationRequested())
                return Result::cancelled();
            return Result::succeeded(std::make_shared<const Finished>(
                Finished{std::move(trace), request, std::this_thread::get_id()}));
        });
    if (!submission.accepted())
        return false;
    ++started_;
    pending_[slot] = std::move(submission.handle);
    bridge_.wake();
    return true;
}

void ScopesTraceRenderer::clear(const scopes::Kind kind) {
    const auto slot = static_cast<std::size_t>(kind);
    if (pending_[slot]) {
        pending_[slot]->cancel();
        pending_[slot].reset();
    }
    finished_[slot].reset();
}

void ScopesTraceRenderer::poll() {
    for (std::size_t slot = 0; slot < pending_.size(); ++slot) {
        if (!pending_[slot])
            continue;
        auto taken = pending_[slot]->tryTakeResult();
        if (!taken)
            continue;
        pending_[slot].reset();
        const auto& value = taken->value();
        if (taken->state() != runtime::TaskState::Succeeded || !value || !*value)
            continue;
        finished_[slot] = *value;
        emit traceReady(static_cast<int>(slot));
    }
}

} // namespace bloom::ui
