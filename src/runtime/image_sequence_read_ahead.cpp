#include <bloom/runtime/image_sequence_read_ahead.hpp>

#include <bloom/media/image.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include "image_source.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

namespace bloom::runtime {

struct ImageSequenceReadAheadState final {
    struct Job final {
        ImageSequenceReadAhead::Request request;
        std::weak_ptr<OperationCache> cache;
    };
    std::size_t workerLimit = 1;
    std::size_t queueLimit = 1;
    mutable std::mutex mutex;
    std::condition_variable ready;
    // Signalled whenever a capture ends.
    std::condition_variable captured;
    std::deque<Job> queue;
    // Revision keys queued or being captured.
    std::set<std::string> pending;
    // Revision keys being captured now.
    std::set<std::string> capturing;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    // Frames reading their own source file now; no capture starts while any is.
    std::size_t foregroundReads = 0;
    // The transport's position: the tracked frame of the latest request (#386).
    SourceUse transport{};
    // Frames decoding their own sources, read-ahead decodes running, and the cores they share.
    std::size_t foregroundDecodes = 0;
    std::size_t aheadDecodes = 0;
    std::size_t decodeLimit = std::max<std::size_t>(1, std::thread::hardware_concurrency() / 2);
    ImageSequenceReadAhead::Statistics statistics;

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                bool deferred = false;
                ready.wait(lock, [&] {
                    if (stopping.load())
                        return true;
                    if (queue.empty())
                        return false;
                    if (foregroundReads == 0)
                        return true;
                    // Work is queued but a frame is reading its own source: wait for it.
                    if (!deferred)
                        ++statistics.deferred;
                    deferred = true;
                    return false;
                });
                if (stopping.load())
                    return;
                job = std::move(queue.front());
                queue.pop_front();
                capturing.insert(job.request.revisionKey);
                ++statistics.started;
            }
            auto revision = capture(job);
            const bool stored = revision != nullptr;
            bool decoded = false;
            bool channelsDecoded = false;
            if (stored && job.request.decode && decodeWanted(job.request.use)) {
                decoded = decode(job, *revision, channelsDecoded);
                const std::lock_guard lock(mutex);
                --aheadDecodes;
            }
            revision.reset();
            {
                const std::lock_guard lock(mutex);
                pending.erase(job.request.revisionKey);
                capturing.erase(job.request.revisionKey);
                ++(stored ? statistics.captured : statistics.failed);
                statistics.decoded += decoded ? 1U : 0U;
                statistics.channelsDecoded += channelsDecoded ? 1U : 0U;
            }
            captured.notify_all();
        }
    }

    // The captured (or already cached) revision, or null when it was not captured.
    [[nodiscard]] std::shared_ptr<const media::ImageSourceRevision> capture(const Job& job) const {
        const auto cache = job.cache.lock();
        if (!cache || stopping.load())
            return nullptr;
        const SourceWorkingSet::UseScope use(job.request.use);
        if (auto cached = cache->find(job.request.revisionKey, document::Revision{},
                                      OperationCacheEntryKind::DecodedMedia))
            return cached->sourceRevision;
        // Never evict for a guess: the file must fit the cache's free budget as it stands.
        std::error_code error;
        const auto size = std::filesystem::file_size(job.request.path, error);
        if (error || size > job.request.byteBudget ||
            cache->retainedBytes() + size > cache->byteBudget())
            return nullptr;
        const auto generation = cache->decodedMediaGeneration();
        auto revision = media::captureImageSourceRevision(
            job.request.path, job.request.byteBudget, [this] { return stopping.load(); },
            job.request.recordedDigest, job.request.stamp);
        if (!revision.value || !*revision.value ||
            (!job.request.stamp && (*revision.value)->probe().contentDigest != job.request.digest))
            return nullptr;
        cache->store(job.request.revisionKey, document::Revision{},
                     {.image = {}, .values = {}, .bounds = {}, .sourceRevision = *revision.value},
                     OperationCacheEntryKind::DecodedMedia, generation);
        return *revision.value;
    }

    // #386: a decode is for a frame the transport has not reached yet and that is still inside the
    // read-ahead window from where it is; any other frame was skipped (or is being prepared
    // already, and decodes its own samples).
    // A decode that is wanted also takes a core (aheadDecodes); the caller releases it.
    [[nodiscard]] bool decodeWanted(const SourceUse& use) {
        const std::lock_guard lock(mutex);
        if (stopping.load())
            return false;
        if (use.tracked && transport.tracked && use.composition == transport.composition &&
            (use.frame <= transport.frame ||
             use.frame - transport.frame > SourceWorkingSet::kReadAheadFrames)) {
            ++statistics.decodesSkipped;
            return false;
        }
        if (foregroundDecodes + aheadDecodes >= decodeLimit) {
            ++statistics.decodesYielded;
            return false;
        }
        ++aheadDecodes;
        return true;
    }

    // Decodes the member's native samples into the cache under the key the frame's upload looks
    // for, and (#549) the requested channels of its part in the same read under the key the
    // frame's channel reads look for. Returns true when the samples were stored (or were already
    // cached); `channelsStored` says a channel set was stored.
    [[nodiscard]] bool decode(const Job& job, const media::ImageSourceRevision& revision,
                              bool& channelsStored) const {
        const auto cache = job.cache.lock();
        if (!cache || stopping.load())
            return false;
        auto interpretation = *job.request.decode;
        // #478: the frame keys and decodes its part's resolved colour layer.
        if (const auto* part = revision.probe().findPart(interpretation.part); part != nullptr)
            interpretation.layer = part->resolveColourLayer(interpretation.layer);
        const auto key = detail::decodedImageSamplesKey(detail::imageDecodeKey(
            job.request.digest, interpretation.storedSamples, interpretation.alphaAssociation,
            interpretation.part, interpretation.layer));
        const auto& channels = job.request.channels;
        const auto channelsKey =
            channels.empty()
                ? std::string{}
                : detail::decodedImageChannelsKey(
                      detail::sourcePartKey(job.request.digest, interpretation.part), channels);
        const SourceWorkingSet::UseScope use(job.request.use);
        const bool haveSamples =
            cache->find(key, document::Revision{}, OperationCacheEntryKind::DecodedMedia)
                .has_value();
        const bool wantChannels =
            !channels.empty() &&
            !cache->find(channelsKey, document::Revision{}, OperationCacheEntryKind::DecodedMedia);
        if (haveSamples && !wantChannels)
            return true;
        // Never evict for a guess: an upper bound of the samples (four 32-bit channels) and the
        // channel planes must fit the cache's free budget as it stands, and the decode its own.
        const auto& probe = revision.probe();
        const auto pixels = static_cast<std::uint64_t>(probe.width) * probe.height;
        const auto bound =
            (haveSamples ? 0U : pixels * 16U) + (wantChannels ? pixels * 4U * channels.size() : 0U);
        if (bound > job.request.byteBudget || cache->retainedBytes() + bound > cache->byteBudget())
            return haveSamples;
        const auto stop = [this] { return stopping.load(); };
        media::ImageChannelsAlongside alongside;
        if (wantChannels)
            alongside.channels = channels;
        bool samplesStored = haveSamples;
        if (!haveSamples) {
            auto samples =
                media::decodeImageSamples(revision, interpretation, stop, job.request.byteBudget,
                                          nullptr, wantChannels ? &alongside : nullptr);
            if (stopping.load())
                return false;
            if (samples.value && *samples.value) {
                cache->store(key, document::Revision{},
                             {.image = {}, .values = {}, .bounds = {}, .samples = *samples.value},
                             OperationCacheEntryKind::DecodedMedia);
                samplesStored = true;
            }
        }
        // A FLOAT or UINT colour layer has no native samples (the frame decodes it to float): its
        // channels are still read ahead, on their own.
        if (wantChannels && !alongside.decoded) {
            media::ImageChannelSelection selection;
            selection.part = interpretation.part;
            selection.channels = channels;
            auto decoded =
                media::decodeImageChannels(revision, selection, stop, {}, job.request.byteBudget);
            if (decoded.value)
                alongside.decoded = std::move(*decoded.value);
        }
        if (wantChannels && alongside.decoded && !stopping.load()) {
            cache->store(channelsKey, document::Revision{},
                         {.image = {},
                          .values = {},
                          .bounds = {},
                          .sourceRevision = {},
                          .samples = {},
                          .channels = alongside.decoded},
                         OperationCacheEntryKind::DecodedMedia);
            channelsStored = true;
        }
        return samplesStored;
    }
};

ImageSequenceReadAhead::ImageSequenceReadAhead(const std::size_t workers,
                                               const std::size_t queueLimit)
    : state_(std::make_shared<State>()) {
    state_->workerLimit = std::max<std::size_t>(1, workers);
    state_->queueLimit = std::max<std::size_t>(1, queueLimit);
}

ImageSequenceReadAhead::~ImageSequenceReadAhead() {
    {
        const std::lock_guard lock(state_->mutex);
        state_->stopping.store(true);
        state_->queue.clear();
    }
    state_->ready.notify_all();
    state_->captured.notify_all();
    for (auto& worker : state_->workers)
        if (worker.joinable())
            worker.join();
}

void ImageSequenceReadAhead::request(const std::shared_ptr<OperationCache>& cache,
                                     std::vector<Request> requests) {
    if (!cache || requests.empty())
        return;
    std::size_t added = 0;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->stopping.load())
            return;
        if (const auto current = SourceWorkingSet::current(); current.tracked)
            state_->transport = current;
        for (auto& request : requests) {
            // A probe that finds the file re-tags it for the frame it is captured for.
            const SourceWorkingSet::UseScope use(request.use);
            if (state_->queue.size() >= state_->queueLimit ||
                state_->pending.contains(request.revisionKey) ||
                cache->find(request.revisionKey, document::Revision{},
                            OperationCacheEntryKind::DecodedMedia)) {
                ++state_->statistics.skipped;
                continue;
            }
            state_->pending.insert(request.revisionKey);
            state_->queue.push_back({.request = std::move(request), .cache = cache});
            ++state_->statistics.queued;
            ++added;
        }
        // Workers start on first use and stay bounded; each one keeps a read outstanding.
        while (state_->workers.size() < std::min(state_->workerLimit, state_->queue.size()))
            state_->workers.emplace_back([state = state_.get()] { state->run(); });
    }
    if (added != 0)
        state_->ready.notify_all();
}

bool ImageSequenceReadAhead::claim(const std::string& revisionKey,
                                   const std::function<bool()>& cancelled) {
    std::unique_lock lock(state_->mutex);
    if (!state_->pending.contains(revisionKey))
        return false;
    if (!state_->capturing.contains(revisionKey)) {
        // Queued and not started: the frame reads it now instead.
        const auto queued = std::ranges::find_if(state_->queue, [&](const State::Job& job) {
            return job.request.revisionKey == revisionKey;
        });
        if (queued != state_->queue.end())
            state_->queue.erase(queued);
        state_->pending.erase(revisionKey);
        ++state_->statistics.withdrawn;
        return false;
    }
    ++state_->statistics.awaited;
    while (state_->capturing.contains(revisionKey) && !state_->stopping.load() &&
           !(cancelled && cancelled()))
        state_->captured.wait_for(lock, std::chrono::milliseconds(5));
    return true;
}

ImageSequenceReadAhead::ForegroundRead::ForegroundRead(std::shared_ptr<State> state)
    : state_(std::move(state)) {
    const std::lock_guard lock(state_->mutex);
    ++state_->foregroundReads;
}

ImageSequenceReadAhead::ForegroundRead::~ForegroundRead() { release(); }

ImageSequenceReadAhead::ForegroundRead::ForegroundRead(ForegroundRead&& other) noexcept
    : state_(std::move(other.state_)) {}

ImageSequenceReadAhead::ForegroundRead&
ImageSequenceReadAhead::ForegroundRead::operator=(ForegroundRead&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
    }
    return *this;
}

void ImageSequenceReadAhead::ForegroundRead::release() noexcept {
    if (!state_)
        return;
    bool idle = false;
    {
        const std::lock_guard lock(state_->mutex);
        idle = --state_->foregroundReads == 0;
    }
    if (idle)
        state_->ready.notify_all();
    state_.reset();
}

ImageSequenceReadAhead::ForegroundDecode::ForegroundDecode(std::shared_ptr<State> state)
    : state_(std::move(state)) {
    const std::lock_guard lock(state_->mutex);
    ++state_->foregroundDecodes;
}

ImageSequenceReadAhead::ForegroundDecode::~ForegroundDecode() { release(); }

ImageSequenceReadAhead::ForegroundDecode::ForegroundDecode(ForegroundDecode&& other) noexcept
    : state_(std::move(other.state_)) {}

ImageSequenceReadAhead::ForegroundDecode&
ImageSequenceReadAhead::ForegroundDecode::operator=(ForegroundDecode&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
    }
    return *this;
}

void ImageSequenceReadAhead::ForegroundDecode::release() noexcept {
    if (!state_)
        return;
    {
        const std::lock_guard lock(state_->mutex);
        --state_->foregroundDecodes;
    }
    state_.reset();
}

ImageSequenceReadAhead::ForegroundDecode ImageSequenceReadAhead::foregroundDecode() {
    return ForegroundDecode(state_);
}

ImageSequenceReadAhead::ForegroundRead ImageSequenceReadAhead::foregroundRead() {
    return ForegroundRead(state_);
}

ImageSequenceReadAhead::Statistics ImageSequenceReadAhead::statistics() const {
    const std::lock_guard lock(state_->mutex);
    return state_->statistics;
}

} // namespace bloom::runtime
