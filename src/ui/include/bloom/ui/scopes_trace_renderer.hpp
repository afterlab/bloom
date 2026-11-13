#pragma once

// #724 Scopes: runs the count-to-image mapping of scopes_drawing.hpp as cancellable tasks. One
// request per scope is in flight at a time; a new request for the same scope cancels the one before
// it and its result is dropped. The interface thread only receives the finished image.

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/scopes_drawing.hpp>

#include <QObject>
#include <QSize>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace bloom::ui {

class TaskUiBridge;

class ScopesTraceRenderer final : public QObject {
    Q_OBJECT

  public:
    struct Finished final {
        scopes::TraceResult trace;
        // The request the image answers.
        scopes::TraceRequest request;
        std::thread::id workerThread;
    };
    using FinishedHandle = std::shared_ptr<const Finished>;

    ScopesTraceRenderer(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                        QObject* parent = nullptr);
    ~ScopesTraceRenderer() override;

    // Maps `request` on a worker. False when the request could not be started.
    bool request(const scopes::TraceRequest& request);
    // Cancels what is running for `kind` and forgets its finished image.
    void clear(scopes::Kind kind);
    // The newest finished mapping of `kind`; null before the first.
    [[nodiscard]] FinishedHandle finished(scopes::Kind kind) const noexcept {
        return finished_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] bool busy(scopes::Kind kind) const noexcept {
        return pending_[static_cast<std::size_t>(kind)].has_value();
    }
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] std::uint64_t mapsStarted() const noexcept { return started_; }

  signals:
    void traceReady(int kind);

  private:
    void poll();

    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    std::array<std::optional<runtime::TaskHandle<FinishedHandle>>, scopes::kKindCount> pending_;
    std::array<FinishedHandle, scopes::kKindCount> finished_;
    std::uint64_t started_ = 0;
};

} // namespace bloom::ui
