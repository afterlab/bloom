#include <bloom/ui/profiler_feed.hpp>

#include <algorithm>

namespace bloom::ui {

ProfilerFeed::ProfilerFeed(QObject* parent) : QObject(parent) {
    subscribers_.reserve(kReservedSubscribers);
    timer_.setTimerType(Qt::CoarseTimer);
    timer_.setInterval(interval_);
    connect(&timer_, &QTimer::timeout, this, [this] { (void)pump(); });
}

ProfilerFeed::~ProfilerFeed() { shutdown(); }

void ProfilerFeed::shutdown() {
    subscribers_.clear();
    setActive(false);
}

void ProfilerFeed::subscribe(ProfilerFeedSubscriber& subscriber) {
    if (std::ranges::find(subscribers_, &subscriber) != subscribers_.end())
        return;
    subscribers_.push_back(&subscriber);
    if (subscribers_.size() == 1)
        setActive(true);
}

void ProfilerFeed::unsubscribe(ProfilerFeedSubscriber& subscriber) {
    const auto found = std::ranges::find(subscribers_, &subscriber);
    if (found == subscribers_.end())
        return;
    // Null rather than erase while delivering, so the loop in pump() keeps its place.
    if (delivering_)
        *found = nullptr;
    else
        subscribers_.erase(found);
    if (subscriberCount() == 0)
        setActive(false);
}

int ProfilerFeed::subscriberCount() const noexcept {
    return static_cast<int>(std::ranges::count_if(
        subscribers_, [](const ProfilerFeedSubscriber* entry) { return entry != nullptr; }));
}

void ProfilerFeed::setInterval(const std::chrono::milliseconds interval) {
    interval_ = std::clamp(interval, kMinimumInterval, kMaximumInterval);
    timer_.setInterval(interval_);
}

void ProfilerFeed::setActive(const bool active) {
    if (active_ == active)
        return;
    active_ = active;
    if (active) {
        ++activations_;
        activate();
        timer_.start();
    } else {
        timer_.stop();
        deactivate();
    }
    Q_EMIT activeChanged(active);
}

bool ProfilerFeed::pump() {
    if (!active_ || delivering_)
        return false;
    const auto previous = latest_.serial;
    if (!readLatest(latest_) || latest_.serial == previous)
        return false;
    delivering_ = true;
    // Index loop: a subscriber may unsubscribe itself (or another) while it is being called.
    for (std::size_t index = 0; index < subscribers_.size(); ++index)
        if (auto* const subscriber = subscribers_[index])
            subscriber->profilerSnapshotArrived(latest_);
    delivering_ = false;
    std::erase(subscribers_, nullptr);
    return true;
}

} // namespace bloom::ui
