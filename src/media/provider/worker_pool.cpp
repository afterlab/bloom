#include <algorithm>
#include <array>
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/core/worker_thread.hpp>
#include <bloom/media/provider/worker_pool.hpp>
#include <bloom/platform/file_snapshot.hpp>
#include <bloom/platform/shared_frame_arena.hpp>
#include <condition_variable>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <thread>

namespace bloom::media::provider {
namespace {
using Clock = std::chrono::steady_clock;
// A retained worker is relaunched after five minutes or this many calls. Relaunching discards
// the decoder position and packet index, so the call bound is sized for continuous playback
// (about 20 minutes at 50 frames per second) and the five-minute lifetime is the binding limit.
constexpr std::size_t kRetainedCallLimit = 65536;
Unavailable failure(const platform::ProcessFailure& p) {
    switch (p.code) {
    case platform::ProcessError::Unavailable:
        return {Error::Unavailable, "Worker process unavailable on this platform"};
    case platform::ProcessError::Timeout:
        return {Error::Timeout, "Worker watchdog expired"};
    case platform::ProcessError::Cancelled:
        return {Error::Cancelled, "Worker call cancelled"};
    case platform::ProcessError::Crashed:
        return {Error::Crashed, "Media worker exited during the call"};
    case platform::ProcessError::ResourceLimit:
        return {Error::Oversized, "Worker resource limits rejected"};
    case platform::ProcessError::Spawn:
    case platform::ProcessError::Io:
        return {Error::Io, "Worker launch or pipe I/O failed"};
    }
    return {Error::Io, "Unknown process failure"};
}
// A worker that vanished mid-call is reaped here so the error names how it ended.
Unavailable failure(const platform::ProcessFailure& p, platform::ProcessSupervisor& process) {
    auto error = failure(p);
    if (p.code == platform::ProcessError::Crashed)
        if (const auto exit = process.waitExit(std::chrono::milliseconds(250)))
            error.detail = "Media worker exited during the call (" +
                           platform::describeProcessExit(*exit) + ")";
    return error;
}
std::string diagnosticCode(Error e) {
    switch (e) {
    case Error::Cancelled:
        return "media.worker.cancelled";
    case Error::Timeout:
        return "media.worker.timeout";
    case Error::Crashed:
        return "media.worker.crashed";
    case Error::Unavailable:
        return "media.worker.unavailable";
    default:
        return "media.worker.rejected";
    }
}
class Connection final {
  public:
    // `snapshot` is the source the worker was given, for the byte ranges it asks for (#718).
    Connection(platform::ProcessSupervisor& process, platform::ProcessDeadline deadline,
               platform::ProcessCancellation cancel,
               const platform::ReadOnlyFileSnapshot* snapshot = nullptr)
        : process_(process), deadline_(deadline), cancel_(std::move(cancel)),
          snapshot_(snapshot) {}
    Result<Message> exchange(const Message& message, HostProtocol& host, MessageKind expected) {
        auto encoded = encodeMessage(message);
        const auto* bytes = std::get_if<Bytes>(&encoded);
        if (!bytes)
            return std::get<Unavailable>(encoded);
        auto written = process_.write(*bytes, deadline_, cancel_);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&written))
            return failure(*e, process_);
        while (true) {
            std::array<std::byte, 4> prefix{};
            auto read = process_.read(prefix, deadline_, cancel_);
            if (const auto* e = std::get_if<platform::ProcessFailure>(&read))
                return failure(*e, process_);
            const auto size = messageLength(prefix);
            if (const auto* e = std::get_if<Unavailable>(&size))
                return *e;
            Bytes response(prefix.begin(), prefix.end());
            response.resize(4U + std::get<std::uint32_t>(size));
            read = process_.read(std::span(response).subspan(4), deadline_, cancel_);
            if (const auto* e = std::get_if<platform::ProcessFailure>(&read)) {
                if (e->code == platform::ProcessError::Crashed) {
                    auto error = failure(*e, process_);
                    error.reason = Error::Truncated;
                    return error;
                }
                return failure(*e, process_);
            }
            // #718: a worker reading a progressive source asks for a byte range in the middle of
            // its call. The range is made local here and the worker told; the reply to the call
            // follows. Only a snapshot this host installed can be asked about.
            if (snapshot_ != nullptr && snapshot_->progressive() &&
                response.size() <= 4U + kEnvelopeBytes + 32U) {
                const auto decoded = decodeMessage(response);
                const auto* asked = std::get_if<Message>(&decoded);
                if (asked != nullptr && host.isRangeRequest(*asked)) {
                    const auto answered = answerRange(*asked);
                    if (answered)
                        return *answered;
                    continue;
                }
            }
            return host.receive(response, expected);
        }
    }

  private:
    // Makes the asked range local and tells the worker. The time the source takes is not the
    // worker's: the call's deadline moves by it. Empty on success; the call's failure otherwise.
    std::optional<Unavailable> answerRange(const Message& asked) {
        const auto& wanted = std::get<SourceRangeV1>(asked.payload);
        const auto began = std::chrono::steady_clock::now();
        const auto made = snapshot_->ensureRange(wanted.offset, wanted.length, cancel_);
        deadline_ += std::chrono::steady_clock::now() - began;
        const auto* range = std::get_if<platform::SnapshotRange>(&made);
        if (range == nullptr) {
            const auto& refused = std::get<platform::ProcessFailure>(made);
            return Unavailable{refused.code == platform::ProcessError::Cancelled ? Error::Cancelled
                                                                                 : Error::Io,
                               "Video source could not be read; check the drive or relink the "
                               "asset in Assets"};
        }
        const auto encoded =
            encodeMessage({MessageKind::RangeReady, asked.session, asked.sequence,
                           SourceRangeV1{range->begin, range->end - range->begin, range->complete}});
        const auto* bytes = std::get_if<Bytes>(&encoded);
        if (bytes == nullptr)
            return std::get<Unavailable>(encoded);
        const auto written = process_.write(*bytes, deadline_, cancel_);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&written))
            return failure(*e, process_);
        return std::nullopt;
    }
    platform::ProcessSupervisor& process_;
    platform::ProcessDeadline deadline_;
    platform::ProcessCancellation cancel_;
    const platform::ReadOnlyFileSnapshot* snapshot_ = nullptr;
};

[[nodiscard]] std::uint64_t newSessionId() {
    std::random_device random;
    std::uint64_t session = (static_cast<std::uint64_t>(random()) << 32U) | random();
    return session == 0 ? 1 : session;
}

[[nodiscard]] MessageKind expectedProductKind(const CallRequest& request) {
    return request.capability.role == Role::Probe         ? MessageKind::Probe
           : request.capability.role == Role::DemuxIndex  ? MessageKind::Index
           : request.capability.role == Role::AudioDecode ? MessageKind::Audio
                                                          : MessageKind::Frame;
}

[[nodiscard]] WorkerReply validatedProduct(const CallRequest& request, Message message) {
    if (auto* probe = std::get_if<ProbeResult>(&message.payload)) {
        if (request.capability.mapping != "bounded-discovery-v1" &&
            (probe->container != request.capability.container ||
             std::ranges::any_of(probe->streams, [&](const auto& stream) {
                 return stream.kind == MediaKind::Video &&
                        (stream.width != request.width || stream.height != request.height ||
                         stream.codec != request.capability.codec);
             })))
            return Unavailable{Error::IdentityMismatch, "Probe does not match the requested tuple"};
        return std::move(*probe);
    }
    if (auto* index = std::get_if<DemuxIndex>(&message.payload))
        return std::move(*index);
    if (auto* audio = std::get_if<AudioBlock>(&message.payload))
        return std::move(*audio);
    if (auto* frame = std::get_if<FrameProduct>(&message.payload)) {
        if (frame->planes.empty() || frame->planes.front().width != request.width ||
            frame->planes.front().height != request.height)
            return Unavailable{Error::IdentityMismatch, "Frame extent does not match the request"};
        return std::move(*frame);
    }
    return Unavailable{Error::UnexpectedMessage, "Worker returned no media product"};
}

// Issue #474: the shared frame arena a retained decoder's frames cross. It holds the largest
// product of the stream (16-bit 4:4:4:4 or RGBA16, 8 bytes a pixel at the displayed extent the
// frame is checked against) and at least an 8K UHD one, so the worker a probe launches serves the
// frames after it. The region is sparse: only the pages a frame writes are committed.
constexpr std::uint64_t kFrameArenaFloorBytes = 7680ULL * 4320ULL * 8ULL;
[[nodiscard]] std::uint64_t frameArenaBytes(const CallRequest& request) noexcept {
    if (request.capability.role != Role::VideoDecode || request.width <= 1 || request.height <= 1)
        return kFrameArenaFloorBytes;
    return std::max(kFrameArenaFloorBytes,
                    static_cast<std::uint64_t>(request.width) * request.height * 8U);
}

// The Profiler's helper-process names (#423).
constexpr const char* kMediaWorkerName = "Media worker";
constexpr const char* kRetainedMediaWorkerName = "Media worker (retained)";

WorkerReply execute(const WorkerPoolOptions& options, const CallRequest& request,
                    const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
                    const platform::ProcessCancellation& cancel, const void* owner) {
    if (cancel())
        return Unavailable{Error::Cancelled, "Worker call cancelled before launch"};
    const auto deadline = Clock::now() + options.timeout;
    auto processOptions = options.process;
    processOptions.inputSnapshot = snapshot;
    processOptions.addressSpaceBytes = std::max(
        {options.process.addressSpaceBytes, workerAddressSpaceBytes(request.width, request.height),
         options.addressSpaceFloor});
    auto launched = platform::ProcessSupervisor::launch(processOptions);
    auto* process = std::get_if<std::unique_ptr<platform::ProcessSupervisor>>(&launched);
    if (!process)
        return failure(std::get<platform::ProcessFailure>(launched));
    if (options.launched)
        options.launched((*process)->processId());
    // Listed for the Profiler while this call's process lives.
    const core::telemetry::HelperProcessRegistration listed((*process)->processId(),
                                                            kMediaWorkerName, owner);
    const std::uint64_t session = newSessionId();
    HostProtocol host(session, options.expected);
    Connection connection(**process, deadline, cancel, snapshot.get());
    auto stop = [&] {
        const auto message = encodeMessage({MessageKind::Cancel, session, 3, std::monostate{}});
        if (const auto* bytes = std::get_if<Bytes>(&message))
            (*process)->stop(*bytes);
        else
            (*process)->stop();
    };
    auto hello = connection.exchange({MessageKind::Handshake, session, 1, options.expected}, host,
                                     MessageKind::Handshake);
    if (const auto* e = std::get_if<Unavailable>(&hello)) {
        stop();
        return *e;
    }
    auto response = connection.exchange({MessageKind::Call, session, 2, request}, host,
                                        expectedProductKind(request));
    if (const auto* e = std::get_if<Unavailable>(&response)) {
        stop();
        return *e;
    }
    auto message = std::get<Message>(std::move(response));
    if (const auto* e = std::get_if<Unavailable>(&message.payload)) {
        stop();
        return *e;
    }
    auto closed = connection.exchange({MessageKind::Shutdown, session, 3, std::monostate{}}, host,
                                      MessageKind::Ack);
    if (const auto* e = std::get_if<Unavailable>(&closed)) {
        stop();
        return *e;
    }
    if (const auto* e = std::get_if<Unavailable>(&std::get<Message>(closed).payload)) {
        stop();
        return *e;
    }
    auto finished = (*process)->finish(deadline);
    if (const auto* e = std::get_if<platform::ProcessFailure>(&finished))
        return failure(*e, **process);
    // Cancellation arriving during orderly close also prevents product publication.
    if (cancel())
        return Unavailable{Error::Cancelled, "Cancelled before product publication"};
    return validatedProduct(request, std::move(message));
}
} // namespace
struct WorkerTicket::State {
    std::mutex mutex;
    std::shared_ptr<const WorkerReply> reply;
    std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
};
std::shared_ptr<const WorkerReply> WorkerTicket::result() const {
    if (!state_)
        return {};
    std::lock_guard lock(state_->mutex);
    return state_->reply;
}
struct MediaWorkerPool::State {
    explicit State(WorkerPoolOptions value) : options(std::move(value)) {}
    struct Retained final {
        Retained(std::unique_ptr<platform::ProcessSupervisor> value,
                 std::shared_ptr<const platform::ReadOnlyFileSnapshot> source,
                 const std::uint64_t id, const Handshake& expected)
            : process(std::move(value)), snapshot(std::move(source)), session(id),
              host(id, expected), started(Clock::now()), lastUsed(started) {}
        std::unique_ptr<platform::ProcessSupervisor> process;
        // Listed for the Profiler while the retained process lives.
        core::telemetry::HelperProcessRegistration listed;
        std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
        std::uint64_t session = 0;
        std::uint64_t sequence = 1;
        HostProtocol host;
        Clock::time_point started;
        Clock::time_point lastUsed;
        std::size_t calls = 0;
        std::uint64_t addressSpace = 0;
        // Issue #474: the frames this worker decodes cross here instead of the pipe.
        std::shared_ptr<const platform::SharedFrameArena> arena;
    };
    WorkerPoolOptions options;
    // Set when this platform or process cannot create a shared frame arena: frames then cross the
    // pipe, and retained workers are not relaunched to get one.
    bool arenaUnavailable = false;
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> active{0};
    // Held by call() from start to end; retainedMutex is taken inside it.
    std::mutex callMutex;
    std::mutex retainedMutex;
    std::unique_ptr<Retained> retained;
    // A retained worker that ended abnormally while idle: the next call that needed it fails once
    // with this cause; later calls start a fresh worker. Guarded by retainedMutex.
    struct Lost final {
        std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
        Unavailable error;
    };
    std::optional<Lost> lost;
    // Reaps an idle retained worker as soon as it ends, so it is never left defunct and what it
    // held (frame arena, source handle, Profiler entry) is released. Started with the first
    // retained worker and joined by the pool's destructor.
    bloom::core::WorkerThread monitor;
    std::mutex monitorMutex;
    std::condition_variable monitorWake;
    bool monitorStop = false;
    void reapIdle() {
        std::unique_lock lock(retainedMutex, std::try_to_lock);
        // A call in flight owns the worker and reports its own failure.
        if (!lock.owns_lock() || !retained)
            return;
        const auto exit = retained->process->waitExit(std::chrono::milliseconds(0));
        if (!exit)
            return;
        // A clean exit is the worker's own idle retirement: not a failure.
        if (exit->signaled || exit->code != 0)
            lost = Lost{retained->snapshot,
                        {Error::Crashed, "Media worker exited while idle (" +
                                             platform::describeProcessExit(*exit) + ")"}};
        retained.reset();
    }
    void startMonitor() {
        if (monitor.joinable())
            return;
        monitor = bloom::core::WorkerThread([this] {
            std::unique_lock lock(monitorMutex);
            while (!monitorStop) {
                monitorWake.wait_for(lock, std::chrono::milliseconds(200));
                if (monitorStop)
                    break;
                lock.unlock();
                reapIdle();
                lock.lock();
            }
        });
    }
    void stopMonitor() {
        {
            const std::lock_guard lock(monitorMutex);
            monitorStop = true;
        }
        monitorWake.notify_all();
        if (monitor.joinable())
            monitor.join();
    }
};
MediaWorkerPool::MediaWorkerPool(WorkerPoolOptions options)
    : state_(std::make_shared<State>(std::move(options))) {}
MediaWorkerPool::~MediaWorkerPool() {
    shutdown();
    state_->stopMonitor();
}
void MediaWorkerPool::shutdown() { state_->stopping.store(true); }
std::size_t MediaWorkerPool::workerProcessIds(const std::span<std::int64_t> out) const noexcept {
    std::array<core::telemetry::HelperProcess, core::telemetry::kHelperProcessSlots> helpers{};
    const auto count = core::telemetry::readHelperProcesses(helpers);
    std::size_t written = 0;
    for (std::size_t index = 0; index < count && written < out.size(); ++index)
        if (helpers[index].owner == state_.get())
            out[written++] = helpers[index].pid;
    return written;
}
void MediaWorkerPool::releaseRetained() {
    const std::lock_guard lock(state_->retainedMutex);
    state_->retained.reset();
}

WorkerReply
MediaWorkerPool::call(const MediaAttempt& attempt, const std::size_t step, CallRequest request,
                      const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
                      const platform::ProcessCancellation& cancel, const bool retainWorker) {
    if (request.snapshotBytes != 0 || state_->options.process.inputSnapshot)
        return Unavailable{Error::IdentityMismatch,
                           "Snapshot authority must be supplied by its owning host capability"};
    if (snapshot) {
        if (request.capability.role != Role::Probe && request.sourceDigest != snapshot->digest())
            return Unavailable{Error::SourceChanged,
                               "Snapshot does not match the requested source revision"};
        request.sourceDigest = snapshot->digest();
        request.snapshotBytes = snapshot->size();
        request.progressiveSnapshot = snapshot->progressive();
    }
    if (state_->stopping.load())
        return Unavailable{Error::Shutdown, "Worker pool is shutting down"};
    if (state_->options.capacity != 1 || state_->options.timeout <= std::chrono::milliseconds(0) ||
        state_->options.timeout > std::chrono::minutes(5))
        return Unavailable{Error::InvalidValue, "Invalid direct worker pool limits"};
    if (state_->options.expected.execution.resourceProfile != "media-v1" ||
        state_->options.process.addressSpaceBytes != kWorkerAddressSpaceFloor ||
        state_->options.process.openFiles != 64)
        return Unavailable{Error::IdentityMismatch,
                           "Process limits do not match the execution profile"};
    if (!attempt.accepts(step, state_->options.expected.execution) ||
        attempt.pipeline().providers[step].capability != request.capability ||
        (request.capability.role != Role::Probe && request.capability.role != Role::VideoDecode &&
         request.capability.role != Role::DemuxIndex &&
         request.capability.role != Role::AudioDecode))
        return Unavailable{Error::Unavailable,
                           "Call does not match pinned execution and capability"};
    if (std::ranges::find(state_->options.expected.declarations,
                          attempt.pipeline().providers[step]) ==
        state_->options.expected.declarations.end())
        return Unavailable{Error::IdentityMismatch,
                           "Pinned qualification is absent from the handshake"};
    // One call at a time: a second one is refused. The idle monitor and releaseRetained() hold
    // retainedMutex only for a moment and are not a call in flight, so this call waits for them
    // instead of reporting a busy pool.
    const std::unique_lock inFlight(state_->callMutex, std::try_to_lock);
    if (!inFlight.owns_lock())
        return Unavailable{Error::Busy, "Video worker capacity reached"};
    const std::unique_lock lock(state_->retainedMutex);
    if (state_->active.fetch_add(1) != 0) {
        state_->active.fetch_sub(1);
        return Unavailable{Error::Busy, "Video worker capacity reached"};
    }
    struct Slot final {
        std::atomic<std::size_t>& count;
        ~Slot() { count.fetch_sub(1); }
    } slot{state_->active};
    const auto cancelled = [&] { return state_->stopping.load() || (cancel && cancel()); };
    // Nothing was exchanged yet, so a retained worker keeps its decoder state (#474). Only a
    // cancellation that interrupts an exchange leaves the protocol unknown and retires it.
    if (cancelled()) {
        if (state_->stopping.load())
            state_->retained.reset();
        return Unavailable{Error::Cancelled, "Video worker call cancelled"};
    }
    const bool retain =
        retainWorker && state_->options.retainedVideo && snapshot &&
        (request.capability.role == Role::Probe || request.capability.role == Role::VideoDecode ||
         request.capability.role == Role::DemuxIndex ||
         request.capability.role == Role::AudioDecode);
    try {
        if (state_->lost) {
            auto lost = std::move(*state_->lost);
            state_->lost.reset();
            if (retain && lost.snapshot == snapshot)
                return lost.error;
        }
        if (!retain) {
            state_->retained.reset();
            return execute(state_->options, request, snapshot, cancelled, state_.get());
        }
        // The inherited immutable source handle is fixed at process launch. A newly captured
        // generation, even with the same digest, must get a fresh worker and decoder state.
        const auto now = Clock::now();
        // A retained worker launched for a smaller stream (or a probe) is relaunched with the
        // address space this stream needs.
        const auto addressSpace = std::max({state_->options.process.addressSpaceBytes,
                                            workerAddressSpaceBytes(request.width, request.height),
                                            state_->options.addressSpaceFloor});
        // Only a frame larger than the held arena relaunches the worker for a larger one.
        const auto arenaBytes = state_->arenaUnavailable ? 0U : frameArenaBytes(request);
        const auto arenaHeld = [&] {
            return state_->retained->arena ? state_->retained->arena->size() : 0U;
        };
        if (state_->retained &&
            (state_->retained->snapshot != snapshot ||
             state_->retained->addressSpace < addressSpace || arenaHeld() < arenaBytes ||
             now - state_->retained->started >= std::chrono::minutes(5) ||
             now - state_->retained->lastUsed >= std::chrono::seconds(30) ||
             state_->retained->calls >= kRetainedCallLimit))
            state_->retained.reset();
        if (!state_->retained) {
            auto options = state_->options.process;
            options.inputSnapshot = snapshot;
            options.addressSpaceBytes = addressSpace;
            options.arguments.emplace_back("--retained-video");
            std::shared_ptr<const platform::SharedFrameArena> arena;
            if (arenaBytes != 0) {
                auto created = platform::SharedFrameArena::create(arenaBytes);
                if (auto* made = std::get_if<std::unique_ptr<platform::SharedFrameArena>>(&created))
                    arena = std::move(*made);
                else
                    state_->arenaUnavailable = true;
            }
            if (arena) {
                // The worker maps the arena inside its address-space limit.
                options.addressSpaceBytes = addressSpace + arena->size();
                options.frameArena = arena;
                options.arguments.emplace_back("--frame-arena");
            }
            auto launched = platform::ProcessSupervisor::launch(options);
            auto* process = std::get_if<std::unique_ptr<platform::ProcessSupervisor>>(&launched);
            if (!process)
                return failure(std::get<platform::ProcessFailure>(launched));
            if (state_->options.launched)
                state_->options.launched((*process)->processId());
            const auto id = newSessionId();
            const auto pid = (*process)->processId();
            state_->retained = std::make_unique<State::Retained>(std::move(*process), snapshot, id,
                                                                 state_->options.expected);
            state_->retained->listed = core::telemetry::HelperProcessRegistration(
                pid, kRetainedMediaWorkerName, state_.get());
            state_->startMonitor();
            state_->retained->addressSpace = addressSpace;
            state_->retained->arena = std::move(arena);
            if (state_->retained->arena)
                state_->retained->host.acceptArenaFrames();
            Connection connection(*state_->retained->process, now + state_->options.timeout,
                                  cancelled);
            auto hello =
                connection.exchange({MessageKind::Handshake, id, 1, state_->options.expected},
                                    state_->retained->host, MessageKind::Handshake);
            if (const auto* error = std::get_if<Unavailable>(&hello)) {
                state_->retained.reset();
                return *error;
            }
        }
        auto& retained = *state_->retained;
        const auto sequence = ++retained.sequence;
        Connection connection(*retained.process, Clock::now() + state_->options.timeout, cancelled,
                              retained.snapshot.get());
        auto response =
            connection.exchange({MessageKind::Call, retained.session, sequence, request},
                                retained.host, expectedProductKind(request));
        if (const auto* error = std::get_if<Unavailable>(&response)) {
            state_->retained.reset();
            return *error;
        }
        auto message = std::get<Message>(std::move(response));
        if (const auto* error = std::get_if<Unavailable>(&message.payload)) {
            state_->retained.reset();
            return *error;
        }
        if (const auto* placed = std::get_if<ArenaFrameV1>(&message.payload)) {
            // Copied out now, before the next request lets the worker write the arena again.
            auto taken =
                retained.arena
                    ? takeArenaFrame(*placed, retained.arena->bytes())
                    : Result<FrameProduct>{Unavailable{Error::UnexpectedMessage, "No frame arena"}};
            if (const auto* error = std::get_if<Unavailable>(&taken)) {
                state_->retained.reset();
                return *error;
            }
            message.payload = std::get<FrameProduct>(std::move(taken));
        }
        auto product = validatedProduct(request, std::move(message));
        if (std::holds_alternative<Unavailable>(product)) {
            state_->retained.reset();
            return product;
        }
        retained.lastUsed = Clock::now();
        ++retained.calls;
        // The exchange completed, so the worker is consistent and stays: a cancellation now only
        // withholds this product (#474).
        if (cancelled()) {
            if (state_->stopping.load())
                state_->retained.reset();
            return Unavailable{Error::Cancelled, "Cancelled before video product publication"};
        }
        return product;
    } catch (const std::bad_alloc&) {
        state_->retained.reset();
        return Unavailable{Error::Oversized, "Worker allocation budget exhausted"};
    } catch (const std::exception&) {
        state_->retained.reset();
        return Unavailable{Error::Io, "Worker call failed unexpectedly"};
    }
}
WorkerTicket
MediaWorkerPool::submit(runtime::TaskScheduler& scheduler, runtime::TaskOwner owner,
                        const MediaAttempt& attempt, std::size_t step, CallRequest request,
                        std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot) {
    WorkerTicket ticket;
    ticket.state_ = std::make_shared<WorkerTicket::State>();
    auto reject = [&](Unavailable error) {
        ticket.state_->reply = std::make_shared<const WorkerReply>(std::move(error));
        ticket.submission.status = runtime::TaskSubmissionStatus::InvalidRequest;
    };
    if (request.snapshotBytes != 0 || state_->options.process.inputSnapshot) {
        reject({Error::IdentityMismatch,
                "Snapshot authority must be supplied by its owning host capability"});
        return ticket;
    }
    if (snapshot) {
        if (request.capability.role != Role::Probe && request.sourceDigest != snapshot->digest()) {
            reject({Error::SourceChanged, "Snapshot does not match the requested source revision"});
            return ticket;
        }
        request.sourceDigest = snapshot->digest();
        request.snapshotBytes = snapshot->size();
        request.progressiveSnapshot = snapshot->progressive();
    }
    if (state_->stopping.load()) {
        reject({Error::Shutdown, "Worker pool is shutting down"});
        return ticket;
    }
    if (state_->options.capacity == 0 || state_->options.capacity > 64 ||
        state_->options.timeout <= std::chrono::milliseconds(0) ||
        state_->options.timeout > std::chrono::minutes(5)) {
        reject({Error::InvalidValue, "Invalid worker pool limits"});
        return ticket;
    }
    if (state_->options.expected.execution.resourceProfile != "media-v1" ||
        state_->options.process.addressSpaceBytes != kWorkerAddressSpaceFloor ||
        state_->options.process.openFiles != 64) {
        reject({Error::IdentityMismatch, "Process limits do not match the execution profile"});
        return ticket;
    }
    if (!attempt.accepts(step, state_->options.expected.execution) ||
        attempt.pipeline().providers[step].capability != request.capability ||
        (request.capability.role != Role::Probe && request.capability.role != Role::VideoDecode &&
         request.capability.role != Role::DemuxIndex &&
         request.capability.role != Role::AudioDecode)) {
        reject({Error::Unavailable, "Call does not match pinned execution and capability"});
        return ticket;
    }
    if (std::ranges::find(state_->options.expected.declarations,
                          attempt.pipeline().providers[step]) ==
        state_->options.expected.declarations.end()) {
        reject({Error::IdentityMismatch, "Pinned qualification is absent from the handshake"});
        return ticket;
    }
    if (state_->active.load() >= state_->options.capacity) {
        reject({Error::Busy, "Worker pool capacity reached"});
        return ticket;
    }
    ticket.state_->snapshot = std::move(snapshot);
    // The scheduler bounds queued requests. Reserve process slots only while work runs:
    // terminal task history retains closures, and queued cancellation never invokes the body.
    runtime::TaskRequest task("Media worker", owner, runtime::TaskPriority::Foreground,
                              runtime::TaskExecutor::BlockingIo);
    ticket.submission = scheduler.submit<void>(
        std::move(task),
        [poolState = std::weak_ptr(state_), result = std::weak_ptr(ticket.state_), attempt, step,
         request = std::move(request)](runtime::TaskContext& context) mutable {
            auto state = poolState.lock();
            if (!state)
                return runtime::TaskResult<void>::cancelled();
            context.reportProgress({"Media worker", "Probe or decode", 0, std::nullopt});
            const auto cancel = [&] {
                return context.isCancellationRequested() || state->stopping.load();
            };
            std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
            if (auto mailbox = result.lock()) {
                std::lock_guard lock(mailbox->mutex);
                snapshot = mailbox->snapshot;
            } else
                return runtime::TaskResult<void>::cancelled();
            WorkerReply reply;
            {
                const auto active = state->active.fetch_add(1);
                struct Slot {
                    std::atomic<std::size_t>& count;
                    ~Slot() { count.fetch_sub(1); }
                } slot{state->active};
                try {
                    if (active >= state->options.capacity)
                        reply = Unavailable{Error::Busy, "Worker pool capacity reached"};
                    else if (!attempt.accepts(step, state->options.expected.execution))
                        reply =
                            Unavailable{Error::Unavailable, "Attempt terminated before execution"};
                    else
                        reply = execute(state->options, request, snapshot, cancel, state.get());
                } catch (const std::bad_alloc&) {
                    reply = Unavailable{Error::Oversized, "Worker allocation budget exhausted"};
                } catch (const std::exception&) {
                    reply = Unavailable{Error::Io, "Worker call failed unexpectedly"};
                }
            }
            snapshot.reset();
            const auto* error = std::get_if<Unavailable>(&reply);
            const auto outcome = error ? *error : Unavailable{};
            if (auto mailbox = result.lock()) {
                std::lock_guard lock(mailbox->mutex);
                mailbox->reply = std::make_shared<const WorkerReply>(std::move(reply));
                mailbox->snapshot.reset();
            }
            if (error) {
                attempt.terminate();
                runtime::TaskDiagnostic diagnostic{
                    diagnosticCode(outcome.reason), runtime::DiagnosticSeverity::Error,
                    outcome.detail, "", "Retry as a new qualified attempt"};
                if (outcome.reason == Error::Cancelled)
                    return runtime::TaskResult<void>::cancelled({diagnostic});
                return runtime::TaskResult<void>::failed(std::move(diagnostic));
            }
            context.reportProgress({"Media worker", "Complete", 1, 1});
            return runtime::TaskResult<void>::succeeded();
        });
    return ticket;
}
} // namespace bloom::media::provider
