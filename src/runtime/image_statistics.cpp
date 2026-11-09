#include <bloom/runtime/image_statistics.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>
#include <variant>

namespace bloom::runtime {
namespace {

struct Entry final {
    // The plan the values were measured from: a hit needs the plan to be the same plan, not just
    // one of the same revision (a live edit changes a plan without changing the revision).
    std::shared_ptr<const CompiledCompositionPlan> plan;
    std::shared_ptr<const ImageStatisticsValues> values;
    std::uint64_t used = 0;
};

struct State final {
    std::mutex mutex;
    std::map<std::string, Entry> entries;
    std::uint64_t tick = 0;
    ImageStatisticsCacheStatistics statistics;
};

State& state() {
    static State instance;
    return instance;
}

thread_local const ImageStatisticsPreparation* currentPreparation = nullptr;

[[nodiscard]] bool samePlan(const Entry& entry, const ImageStatisticsQuery& query) {
    return entry.plan == query.plan ||
           (entry.plan != nullptr && query.plan != nullptr && *entry.plan == *query.plan);
}

} // namespace

std::optional<ImageStatisticsValues> readImageStatistics(const render::Rgba32fImage& result,
                                                         const ImageStatisticsRoute route) {
    const auto window = result.descriptor()->dataWindow();
    if (window.extent().width() != 2 || window.extent().height() != 1)
        return std::nullopt;
    const auto view = result.view();
    if (!view)
        return std::nullopt;
    const auto row = view.value()->row(window.originY());
    if (!row)
        return std::nullopt;
    const auto& first = (*row.value())[0];
    const auto& second = (*row.value())[1];
    ImageStatisticsValues values;
    values.route = route;
    values.count = static_cast<double>(first.red());
    if (!(values.count >= 0.0) || !std::isfinite(values.count))
        return std::nullopt;
    if (values.count == 0.0)
        return values;
    values.minimum = static_cast<double>(first.green());
    values.maximum = static_cast<double>(first.blue());
    values.mean = static_cast<double>(second.red());
    const double variance = static_cast<double>(second.green());
    values.deviation = std::sqrt(std::max(variance, 0.0));
    return values;
}

std::string imageStatisticsKey(const std::string& frameKey, const ImageStatisticsQuery& query) {
    if (query.plan == nullptr)
        return {};
    return frameKey + "|node" + std::to_string(query.node.value()) + "|rev" +
           std::to_string(query.plan->sourceRevision().value()) + "|composition" +
           std::to_string(query.plan->compositionId().value()) + "|t" +
           std::to_string(query.time.numerator()) + "/" + std::to_string(query.time.denominator());
}

std::string imageStatisticsFrameKey(const EvaluationRequest& request,
                                    const ImageStatisticsRoute route) {
    std::string key = route == ImageStatisticsRoute::Device ? "device" : "cpu";
    key += "|q" + std::to_string(static_cast<int>(request.quality));
    key += "|look" + std::to_string(request.bypassLookNodes ? 1 : 0);
    if (const auto* proxy = std::get_if<ProxyResolution>(&request.resolution))
        key += "|proxy" + std::to_string(proxy->extent.width()) + "x" +
               std::to_string(proxy->extent.height());
    else
        key += "|full";
    key += "|" + std::string(request.colorIntent.workingColorSpaceId);
    const auto revision = request.colorIntent.ocioConfigRevision.toLowercaseHex();
    key += "|" + std::string(revision.data(), revision.size());
    return key;
}

EvaluationRequest imageStatisticsRequest(const EvaluationRequest& frame,
                                         const ImageStatisticsQuery& query) {
    EvaluationRequest request = frame;
    request.time = query.time;
    request.output = query.plan->output();
    // The statistics count the whole data window of the picture, whatever region the frame asks for.
    // The result is the two pixels at the origin of the composition, and only they are read back.
    const auto window = render::ImageWindow::create(0, 0, 2, 1);
    request.roi = *window.value();
    request.linearPass = nullptr;
    request.finalOutputPhase = nullptr;
    return request;
}

bool planReadsImageStatistics(const CompiledCompositionPlan& plan) noexcept {
    return std::ranges::any_of(plan.valueOperations(), [](const CompiledValueOperation& operation) {
        return std::holds_alternative<CompiledValueImageStatistics>(operation.kernel);
    });
}

bool ImageStatisticsPreparation::installedHere() noexcept {
    return currentPreparation != nullptr && static_cast<bool>(currentPreparation->provider_);
}

ImageStatisticsPreparation::ImageStatisticsPreparation(std::string frameKey,
                                                       ImageStatisticsProvider provider,
                                                       CancellationToken cancellation)
    : frameKey_(std::move(frameKey)), provider_(std::move(provider)),
      cancellation_(std::move(cancellation)), previous_(currentPreparation) {
    currentPreparation = this;
}

ImageStatisticsPreparation::~ImageStatisticsPreparation() { currentPreparation = previous_; }

std::shared_ptr<const ImageStatisticsValues>
ImageStatisticsPreparation::findHere(const ImageStatisticsQuery& query) {
    if (currentPreparation == nullptr)
        return nullptr;
    const auto key = imageStatisticsKey(currentPreparation->frameKey_, query);
    if (key.empty())
        return nullptr;
    auto& s = state();
    const std::scoped_lock lock(s.mutex);
    const auto found = s.entries.find(key);
    if (found == s.entries.end() || !samePlan(found->second, query))
        return nullptr;
    found->second.used = ++s.tick;
    ++s.statistics.hits;
    return found->second.values;
}

std::optional<ImageStatisticsStatus>
ImageStatisticsPreparation::prepareHere(const ImageStatisticsQuery& query) {
    if (currentPreparation == nullptr || !currentPreparation->provider_)
        return std::nullopt;
    const auto key = imageStatisticsKey(currentPreparation->frameKey_, query);
    if (key.empty())
        return ImageStatisticsStatus{ImageStatisticsOutcome::Unavailable,
                                     "The picture has no statistics plan."};
    auto& s = state();
    {
        const std::scoped_lock lock(s.mutex);
        if (const auto found = s.entries.find(key);
            found != s.entries.end() && samePlan(found->second, query))
            return ImageStatisticsStatus{ImageStatisticsOutcome::Ready, {}};
    }
    if (currentPreparation->cancellation_.isCancellationRequested()) {
        const std::scoped_lock lock(s.mutex);
        ++s.statistics.cancelled;
        return ImageStatisticsStatus{ImageStatisticsOutcome::Cancelled, {}};
    }
    // The picture is evaluated outside the lock: it may itself prepare statistics.
    auto run = currentPreparation->provider_(query, currentPreparation->cancellation_);
    const std::scoped_lock lock(s.mutex);
    if (run.status.outcome != ImageStatisticsOutcome::Ready || !run.values) {
        // Nothing is cached for a failed or cancelled run.
        if (run.status.outcome == ImageStatisticsOutcome::Cancelled)
            ++s.statistics.cancelled;
        else
            ++s.statistics.failed;
        if (run.status.outcome == ImageStatisticsOutcome::Ready)
            run.status = {ImageStatisticsOutcome::Unavailable, "The statistics are not readable."};
        return run.status;
    }
    ++s.statistics.computed;
    if (run.values->route == ImageStatisticsRoute::Device)
        ++s.statistics.computedOnDevice;
    s.entries[key] = Entry{query.plan, std::make_shared<const ImageStatisticsValues>(*run.values),
                           ++s.tick};
    while (s.entries.size() > kImageStatisticsCacheEntries) {
        auto oldest = s.entries.begin();
        for (auto it = s.entries.begin(); it != s.entries.end(); ++it)
            if (it->second.used < oldest->second.used)
                oldest = it;
        s.entries.erase(oldest);
        ++s.statistics.evicted;
    }
    return ImageStatisticsStatus{ImageStatisticsOutcome::Ready, {}};
}

ImageStatisticsCacheStatistics imageStatisticsCacheStatistics() {
    auto& s = state();
    const std::scoped_lock lock(s.mutex);
    auto copy = s.statistics;
    copy.entries = s.entries.size();
    return copy;
}

void clearImageStatistics() {
    auto& s = state();
    const std::scoped_lock lock(s.mutex);
    s.entries.clear();
    s.statistics = {};
}

} // namespace bloom::runtime
