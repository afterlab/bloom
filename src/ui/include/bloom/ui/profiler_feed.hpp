#pragma once

// The Qt side of the profiler telemetry contract (#423; docs/architecture/profiler-feed.md).
//
// A ProfilerFeed delivers `const runtime::ProfilerSnapshot&` to its subscribers at the published
// rate, on the UI thread. It runs only while someone subscribes: the first subscriber activates the
// source (the runtime sampler is started), the last one deactivates it and stops the delivery
// timer, so a closed or hidden Profiler costs nothing. A source publishes a new snapshot with a
// new serial; the feed copies it once into its own storage and hands every subscriber the same
// reference, and delivers nothing when the serial has not changed. Subscriber storage is reserved
// up front, so delivery never allocates.
//
// The feed never mutates budgets, caches or scheduling; neither does any source.

#include <bloom/runtime/profiler_snapshot.hpp>

#include <QObject>
#include <QTimer>

#include <chrono>
#include <vector>

namespace bloom::ui {

class ProfilerFeedSubscriber {
  public:
    virtual void profilerSnapshotArrived(const runtime::ProfilerSnapshot& snapshot) = 0;

  protected:
    ~ProfilerFeedSubscriber() = default;
};

class ProfilerFeed : public QObject {
    Q_OBJECT

  public:
    // The contract's default rate is 2 Hz; a feed accepts 1 to 10 Hz.
    static constexpr std::chrono::milliseconds kDefaultInterval{500};
    static constexpr std::chrono::milliseconds kMinimumInterval{100};
    static constexpr std::chrono::milliseconds kMaximumInterval{1000};
    // Profiler panels open at once that the feed reserves room for (more still works).
    static constexpr std::size_t kReservedSubscribers = 8;

    explicit ProfilerFeed(QObject* parent = nullptr);
    ~ProfilerFeed() override;
    ProfilerFeed(const ProfilerFeed&) = delete;
    ProfilerFeed& operator=(const ProfilerFeed&) = delete;

    // Refcounted: subscribing twice is one subscription, so a panel shown twice still needs only
    // one unsubscribe.
    void subscribe(ProfilerFeedSubscriber& subscriber);
    void unsubscribe(ProfilerFeedSubscriber& subscriber);
    [[nodiscard]] int subscriberCount() const noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }

    void setInterval(std::chrono::milliseconds interval);
    [[nodiscard]] std::chrono::milliseconds interval() const noexcept { return interval_; }

    // Reads the source once and delivers when it published something new; returns whether it
    // delivered. The delivery timer calls this; tests call it to step deterministically.
    bool pump();

    // The last snapshot delivered (serial 0 before the first).
    [[nodiscard]] const runtime::ProfilerSnapshot& latest() const noexcept { return latest_; }
    // How many times the source has been activated (tests).
    [[nodiscard]] int activationCount() const noexcept { return activations_; }

  Q_SIGNALS:
    void activeChanged(bool active);

  protected:
    // The source starts and stops with the first and last subscriber.
    virtual void activate() {}
    virtual void deactivate() {}
    // Copies the source's newest snapshot into `out` (already holding the previous one). Returns
    // false when the source has nothing newer. Runs on the UI thread: a copy, never sampling.
    virtual bool readLatest(runtime::ProfilerSnapshot& out) = 0;
    // Stops the source if it is still running; a derived destructor calls this first, while its
    // own deactivate() can still run.
    void shutdown();

  private:
    void setActive(bool active);

    std::vector<ProfilerFeedSubscriber*> subscribers_;
    runtime::ProfilerSnapshot latest_{};
    QTimer timer_;
    std::chrono::milliseconds interval_ = kDefaultInterval;
    bool active_ = false;
    bool delivering_ = false;
    int activations_ = 0;
};

} // namespace bloom::ui
