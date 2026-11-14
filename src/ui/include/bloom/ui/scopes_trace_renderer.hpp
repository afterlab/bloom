#pragma once

// #724, #730 Scopes: runs the count-to-image mapping of scopes_drawing.hpp as cancellable tasks.
// One mapping per scope is in flight at a time. A new request for a scope whose mapping is running
// waits behind it, replacing an older waiting one, and starts when it finishes: the running one is
// never cancelled for a newer frame, so the scope keeps updating during playback however long a
// mapping takes. The interface thread only receives the finished image.

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/scopes_drawing.hpp>

#include <QImage>
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
        QImage image;
        // The request the image answers.
        scopes::TraceRequest request;
        std::thread::id workerThread;
    };
    using FinishedHandle = std::shared_ptr<const Finished>;

    ScopesTraceRenderer(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                        QObject* parent = nullptr);
    ~ScopesTraceRenderer() override;

    // Maps `request` on a worker, now or after the running mapping of its scope. False when it
    // could not be started.
    bool request(const scopes::TraceRequest& request);
    // Cancels what is running or waiting for `kind` and forgets its finished image.
    void clear(scopes::Kind kind);
    // The newest finished mapping of `kind`; null before the first.
    [[nodiscard]] FinishedHandle finished(scopes::Kind kind) const noexcept {
        return finished_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] bool busy(scopes::Kind kind) const noexcept {
        return pending_[static_cast<std::size_t>(kind)].has_value() ||
               waiting_[static_cast<std::size_t>(kind)].has_value();
    }
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] std::uint64_t mapsStarted() const noexcept { return started_; }

  signals:
    void traceReady(int kind);

  private:
    void poll();
    bool start(const scopes::TraceRequest& request);

    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    std::array<std::optional<runtime::TaskHandle<FinishedHandle>>, scopes::kKindCount> pending_;
    std::array<std::optional<scopes::TraceRequest>, scopes::kKindCount> waiting_;
    std::array<FinishedHandle, scopes::kKindCount> finished_;
    std::uint64_t started_ = 0;
};

} // namespace bloom::ui
