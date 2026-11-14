#include <bloom/ui/scope_fill_taps.hpp>

#include <bloom/ui/task_ui_bridge.hpp>

#include <algorithm>
#include <mutex>
#include <utility>

namespace bloom::ui {
namespace {

// Tables of frames that were not retained yet, kept for them.
constexpr std::size_t kKeptTables = 4;
// Received taps waiting for a panel. The service holds one picture at a time, so more than a
// couple waiting is not expected; the oldest is dropped (answered) past this.
constexpr std::size_t kWaitingTaps = 4;
// Reasons a frame's tap did not measure, kept for the frame's status.
constexpr std::size_t kFailureReasons = 64;

} // namespace

ScopeFillTap::ScopeFillTap(std::shared_ptr<Port> port, runtime::PreviewRequestIdentity identity,
                           PreparedPreviewFrameHandle frame, const bool tapped)
    : port_(std::move(port)), identity_(std::move(identity)), frame_(std::move(frame)),
      tapped_(tapped), key_(runtime::GpuScopeTapKey::of(identity_)) {}

ScopeFillTap::~ScopeFillTap() { release(); }

runtime::GpuScopeTapMeasureOutcome
ScopeFillTap::measure(const runtime::GpuScopeMeasureJob& job,
                      const runtime::CancellationToken& cancellation) {
    runtime::GpuScopeTapMeasureOutcome outcome;
    outcome.status = runtime::GpuScopeTapStatus::Skipped;
    outcome.skip = runtime::GpuScopeTapSkip::TapGone;
    if (!tapped_) {
        outcome.message = "the frame holds no tapped picture";
        return outcome;
    }
    if (answered_.exchange(true)) {
        outcome.message = "the frame's tap was already answered";
        return outcome;
    }
    const std::shared_lock lock(port_->mutex);
    if (port_->service == nullptr) {
        outcome.message = "the preview display service is gone";
        port_->answeredByDrop.fetch_add(1, std::memory_order_relaxed);
        return outcome;
    }
    auto& service = *port_->service;
    // A Viewer tap of the same key shadows the fill's one in the service's lookup: the measure then
    // counts that (identical) picture and leaves the fill slot occupied, which a drop frees.
    const auto geometry = service.scopeTapGeometry(key_);
    const bool shadowed = geometry.has_value() && !geometry->fill;
    outcome = service.measureScopeTap(key_, job, cancellation);
    // The service frees the fill slot when a measure that began ends. Every other result (Busy,
    // refused, no longer tapped, nothing to count) leaves it to the caller.
    if (outcome.status != runtime::GpuScopeTapStatus::Measured || shadowed)
        service.dropScopeTap(key_);
    port_->answeredByMeasure.fetch_add(1, std::memory_order_relaxed);
    return outcome;
}

void ScopeFillTap::release() noexcept {
    if (!tapped_ || answered_.exchange(true))
        return;
    try {
        const std::shared_lock lock(port_->mutex);
        if (port_->service != nullptr)
            port_->service->dropScopeTap(key_);
        port_->answeredByDrop.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
    }
}

bool attachScopeSidecar(PreviewFrameCache& cache, const PreviewFrameCacheKey& key,
                        const PreviewScopeSidecarHandle& sidecar) {
    if (sidecar == nullptr)
        return false;
    const auto& identity = sidecar->identity;
    const auto held = [&](const runtime::ScopeSource source, const std::uint32_t detail) {
        auto probe = identity;
        probe.source = source;
        probe.settings.detail = detail;
        return cache.scopesFor(key, probe) != nullptr;
    };
    if (identity.source == runtime::ScopeSource::Retained) {
        // Retained tables never replace Rendered ones of the same measure.
        if (held(runtime::ScopeSource::Rendered, 0) || held(runtime::ScopeSource::Rendered, 1))
            return false;
    } else if (identity.settings.detail != 0 && held(runtime::ScopeSource::Rendered, 0)) {
        return false;
    }
    return cache.attachScopes(key, sidecar);
}

ScopeFillTaps::ScopeFillTaps(runtime::GpuPreviewDisplayService* const service,
                             PreviewFrameCacheHandle cache, TaskUiBridge& bridge,
                             QObject* const parent)
    : QObject(parent), service_(service), cache_(std::move(cache)),
      port_(new ScopeFillTap::Port()) {
    port_->service = service;
    // The orphaned submissions are polled as tasks finish.
    connect(&bridge, &TaskUiBridge::resultsAvailable, this, &ScopeFillTaps::pollOrphans);
    if (cache_ != nullptr) {
        // Connected before any panel's own connection, so a frame's kept tables are attached before
        // a panel looks at the frame.
        connect(cache_.get(), &PreviewFrameCache::entryRetained, this, &ScopeFillTaps::onRetained);
        connect(cache_.get(), &PreviewFrameCache::entriesCleared, this, [this] { kept_.clear(); });
    }
}

ScopeFillTaps::~ScopeFillTaps() { shutdown(); }

void ScopeFillTaps::shutdown() {
    if (closed_)
        return;
    closed_ = true;
    clients_.clear();
    failures_.clear();
    // Every tap this object holds or will receive is answered now: the received ones by their
    // destructor, the ones whose frames are not read yet by asking the service directly.
    ready_.clear();
    kept_.clear();
    // A submission whose frame was not read yet is answered through a tap that is let go of at once.
    for (const auto& entry : entries_)
        static_cast<void>(makeTap(entry.identity, nullptr, true));
    entries_.clear();
    // Late holders of a tap find no service and do nothing.
    const std::unique_lock lock(port_->mutex);
    port_->service = nullptr;
}

void ScopeFillTaps::setWanted(const void* const client, const bool wanted) {
    if (closed_)
        return;
    const auto found = std::ranges::find(clients_, client);
    const bool was = found != clients_.end();
    if (wanted == was)
        return;
    const bool servedBefore = !clients_.empty() && clients_.front() == client;
    if (wanted) {
        clients_.push_back(client);
    } else {
        clients_.erase(found);
    }
    if (clients_.empty()) {
        // Nobody wants the tables: the received taps are dropped, the kept tables forgotten.
        ready_.clear();
        kept_.clear();
    } else if (!wanted && servedBefore) {
        emit tapsAvailable();
    }
}

bool ScopeFillTaps::wanted() const noexcept { return !closed_ && !clients_.empty(); }

std::shared_ptr<ScopeFillTap> ScopeFillTaps::takeTap(const void* const client) {
    if (closed_ || clients_.empty() || clients_.front() != client || ready_.empty())
        return nullptr;
    auto tap = std::move(ready_.front());
    ready_.pop_front();
    return tap;
}

void ScopeFillTaps::deliver(const PreviewFrameCacheKey& key, PreviewScopeSidecarHandle sidecar) {
    if (sidecar == nullptr || cache_ == nullptr || closed_)
        return;
    if (cache_->contains(key)) {
        static_cast<void>(attachScopeSidecar(*cache_, key, sidecar));
        return;
    }
    // The frame is not retained (yet): its tables wait for it while a fill is running.
    if (!fillActive_ && entries_.empty())
        return;
    kept_.erase(std::remove_if(kept_.begin(), kept_.end(),
                               [&](const auto& item) { return item.first == key; }),
                kept_.end());
    if (kept_.size() >= kKeptTables)
        kept_.erase(kept_.begin());
    kept_.emplace_back(key, std::move(sidecar));
}

void ScopeFillTaps::onRetained(const PreviewFrameCacheKey& key) {
    const auto found = std::ranges::find(kept_, key, &decltype(kept_)::value_type::first);
    if (found == kept_.end())
        return;
    auto sidecar = std::move(found->second);
    kept_.erase(found);
    if (cache_ != nullptr)
        static_cast<void>(attachScopeSidecar(*cache_, key, sidecar));
}

runtime::TaskSubmission<runtime::PreviewPreparationResultHandle>
ScopeFillTaps::submit(runtime::TaskRequest request, const document::Snapshot& snapshot,
                      const runtime::PreviewRequestIdentity& identity,
                      const std::size_t pixelStorageByteLimit,
                      const std::vector<runtime::SnapshotParameterOverride>& overrides) {
    if (closed_ || service_ == nullptr)
        return {};
    auto submission = service_->submit(std::move(request), snapshot, identity,
                                       pixelStorageByteLimit, overrides, true);
    if (submission.accepted())
        adoptSubmission(identity, submission.handle);
    return submission;
}

void ScopeFillTaps::adoptSubmission(
    const runtime::PreviewRequestIdentity& identity,
    runtime::TaskHandle<runtime::PreviewPreparationResultHandle> handle) {
    // From here the service may hold this frame's picture until the tap is answered.
    entries_.push_back(Entry{identity, std::move(handle), false});
    ++flagged_;
    fillActive_ = true;
}

std::shared_ptr<ScopeFillTap> ScopeFillTaps::makeTap(const runtime::PreviewRequestIdentity& id,
                                                     PreparedPreviewFrameHandle frame,
                                                     const bool tapped) {
    return std::shared_ptr<ScopeFillTap>(new ScopeFillTap(port_, id, std::move(frame), tapped));
}

void ScopeFillTaps::received(const runtime::PreviewRequestIdentity& identity,
                             const PreparedPreviewFrameHandle& frame) {
    const auto found = std::ranges::find(entries_, identity, &Entry::identity);
    const bool flagged = found != entries_.end();
    if (flagged)
        entries_.erase(found);
    // A flagged submission is always turned into a tap, which is answered when the last holder lets
    // go of it. An unflagged frame is queued only when it still holds its process picture (the CPU
    // route's), which is gone once the cache retains the frame.
    const bool cpuPicture = frame != nullptr && frame->hasProcessFrame();
    if (!flagged && !(cpuPicture && wanted()))
        return;
    auto tap = makeTap(identity, frame, flagged);
    if (frame == nullptr || closed_ || clients_.empty()) {
        // Nothing to count it for (a frame that was not delivered holds nothing; the answer is the
        // same drop).
        return;
    }
    ready_.push_back(std::move(tap));
    while (ready_.size() > kWaitingTaps)
        ready_.pop_front();
    emit tapsAvailable();
}

std::string ScopeFillTaps::failureFor(const PreviewFrameCacheKey& key) const {
    const auto found = std::ranges::find(failures_, key, &decltype(failures_)::value_type::first);
    return found == failures_.end() ? std::string{} : found->second;
}

void ScopeFillTaps::rememberFailure(const PreviewFrameCacheKey& key, std::string reason) {
    std::erase_if(failures_, [&](const auto& item) { return item.first == key; });
    if (failures_.size() >= kFailureReasons)
        failures_.erase(failures_.begin());
    failures_.emplace_back(key, std::move(reason));
}

void ScopeFillTaps::orphan(const runtime::PreviewRequestIdentity& identity) {
    const auto found = std::ranges::find(entries_, identity, &Entry::identity);
    if (found == entries_.end())
        return;
    found->orphaned = true;
    pollOrphans();
}

void ScopeFillTaps::pollOrphans() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!it->orphaned) {
            ++it;
            continue;
        }
        // A terminal result in the mailbox means the task has finished and nothing else reads it:
        // the service holds the frame's picture, if it kept one, until this tap is answered.
        auto result = it->handle.tryTakeResult();
        if (!result.has_value()) {
            ++it;
            continue;
        }
        auto tap = makeTap(it->identity, nullptr, true);
        it = entries_.erase(it);
        // `tap` goes out of scope: unanswered, it drops the picture.
    }
}

void ScopeFillTaps::fillEnded() {
    fillActive_ = false;
    kept_.clear();
}

std::uint64_t ScopeFillTaps::answeredByMeasure() const noexcept {
    return port_->answeredByMeasure.load(std::memory_order_relaxed);
}

std::uint64_t ScopeFillTaps::answeredByDrop() const noexcept {
    return port_->answeredByDrop.load(std::memory_order_relaxed);
}

} // namespace bloom::ui
