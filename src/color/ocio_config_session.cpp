#include "ocio_config_session.hpp"

#include "lut_resource_budget.hpp"
#include "lut_worker_ipc.hpp"
#include "lut_worker_protocol.hpp"

#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/platform/process_supervisor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>

namespace bloom::color::detail {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] OcioHelperError helperError(const LutError error) noexcept {
    switch (error) {
    case LutError::None:
        return OcioHelperError::None;
    case LutError::HelperUnavailable:
        return OcioHelperError::HelperUnavailable;
    case LutError::HelperMemoryLimit:
        return OcioHelperError::HelperMemoryLimit;
    case LutError::HelperDeadline:
        return OcioHelperError::HelperDeadline;
    case LutError::HelperCancelled:
        return OcioHelperError::HelperCancelled;
    case LutError::HelperTerminated:
        return OcioHelperError::HelperTerminated;
    default:
        return OcioHelperError::HelperProtocolViolation;
    }
}

#ifdef __linux__
[[nodiscard]] LutError processError(const platform::ProcessFailure failure) noexcept {
    switch (failure.code) {
    case platform::ProcessError::Unavailable:
    case platform::ProcessError::Spawn:
        return LutError::HelperUnavailable;
    case platform::ProcessError::ResourceLimit:
        return LutError::HelperMemoryLimit;
    case platform::ProcessError::Timeout:
        return LutError::HelperDeadline;
    case platform::ProcessError::Cancelled:
        return LutError::HelperCancelled;
    case platform::ProcessError::Crashed:
        return LutError::HelperTerminated;
    case platform::ProcessError::Io:
        return LutError::HelperProtocolViolation;
    }
    return LutError::HelperProtocolViolation;
}

[[nodiscard]] std::string workerPath() {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        const auto installed = executable.parent_path() / "../libexec/bloom/bloom-color-worker";
        if (std::filesystem::is_regular_file(installed, error))
            return installed.lexically_normal().string();
    }
#ifdef BLOOM_COLOR_WORKER
    return BLOOM_COLOR_WORKER;
#else
    return {};
#endif
}

// A sealed shared file holding exactly `bytes`.
[[nodiscard]] int sealedFile(const std::span<const std::byte> bytes) {
    const int fd = sharedFile();
    if (fd < 0)
        return -1;
    if (!writeFd(fd, bytes) || !sealInput(fd)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// A shared file of a fixed extent for the helper to write a reply into.
[[nodiscard]] int replyFile(const std::uint64_t extent) {
    const int fd = sharedFile();
    if (fd < 0)
        return -1;
    if (::ftruncate(fd, static_cast<off_t>(extent)) != 0 || !sealExtent(fd)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

[[nodiscard]] std::optional<std::vector<std::byte>> readReply(const int fd,
                                                              const std::uint64_t bytes) {
    std::vector<std::byte> value(static_cast<std::size_t>(bytes));
    if (::lseek(fd, 0, SEEK_SET) != 0 || !readFd(fd, value))
        return std::nullopt;
    return value;
}
#endif

// One token of a session. It keeps the session, and so the helper, alive.
class SessionProcessor final : public RemoteRgbProcessor {
  public:
    SessionProcessor(std::shared_ptr<const ConfigSession> session, const std::uint64_t token)
        : session_(std::move(session)), token_(token) {}
    [[nodiscard]] bool apply(const std::span<float> rgba,
                             const bool packed) const noexcept override {
        return session_->apply(token_, rgba, packed);
    }

  private:
    std::shared_ptr<const ConfigSession> session_;
    std::uint64_t token_;
};

} // namespace

struct ConfigSession::Impl final {
    std::mutex mutex;
    std::unique_ptr<platform::ProcessSupervisor> worker;
    // Listed for the Profiler while the colour worker lives (#423).
    core::telemetry::HelperProcessRegistration listed;
    std::shared_ptr<const LutReservation> reservation;
    int descriptorSocket = -1;
    std::uint64_t nonce = 1;
    std::atomic<LutError> failure{LutError::None};

    ~Impl() {
#ifdef __linux__
        if (descriptorSocket >= 0)
            ::close(descriptorSocket);
#endif
        if (worker)
            worker->stop();
    }

#ifdef __linux__
    // One command and its reply. The reply must echo the command, its nonce and every request
    // field; the helper's status and payload come back in fields 5 and 6. Any failure ends the
    // session.
    [[nodiscard]] LutError exchange(LutPacket& message, const std::chrono::seconds deadline,
                                    const std::function<bool()>& cancellation) {
        if (failure != LutError::None)
            return failure;
        const auto expiry = Clock::now() + deadline;
        const auto sent = message;
        const auto written = worker->write(std::as_bytes(std::span(message)), expiry, cancellation);
        if (const auto* error = std::get_if<platform::ProcessFailure>(&written)) {
            failure = processError(*error);
        } else {
            const auto read =
                worker->read(std::as_writable_bytes(std::span(message)), expiry, cancellation);
            if (const auto* readError = std::get_if<platform::ProcessFailure>(&read))
                failure = processError(*readError);
            else if (!validPacket(message, sent[2], sent[3]) || message[4] != sent[4] ||
                     message[5] > static_cast<std::uint64_t>(LutError::InvalidPixel) ||
                     !std::equal(message.begin() + 7, message.end(), sent.begin() + 7))
                failure = LutError::HelperProtocolViolation;
            else
                failure = static_cast<LutError>(message[5]);
        }
        if (failure != LutError::None)
            worker->stop();
        return failure;
    }

    void fail(const LutError error) {
        failure = error;
        worker->stop();
    }

    // Sends a sealed request record and a reply file with one command and returns the reply
    // bytes. Caller holds the mutex.
    [[nodiscard]] std::optional<std::vector<std::byte>>
    request(const std::uint64_t command, const std::span<const std::byte> record,
            const std::uint64_t replyExtent, const std::chrono::seconds deadline,
            const std::function<bool()>& cancellation) {
        if (failure != LutError::None)
            return std::nullopt;
        const LutFd input(sealedFile(record));
        const LutFd output(replyFile(replyExtent));
        if (input.get() < 0 || output.get() < 0)
            return std::nullopt;
        const std::array descriptors{input.get(), output.get()};
        if (!sendDescriptors(descriptorSocket, descriptors)) {
            fail(LutError::HelperProtocolViolation);
            return std::nullopt;
        }
        auto message = packet(command, nonce++);
        message[4] = record.size();
        message[7] = inode(input.get());
        message[8] = replyExtent;
        message[9] = inode(output.get());
        if (exchange(message, deadline, cancellation) != LutError::None)
            return std::nullopt;
        if (message[6] == 0 || message[6] > replyExtent) {
            fail(LutError::HelperProtocolViolation);
            return std::nullopt;
        }
        auto reply = readReply(output.get(), message[6]);
        if (!reply)
            fail(LutError::HelperProtocolViolation);
        return reply;
    }
#endif
};

ConfigSession::ConfigSession(std::unique_ptr<Impl> impl, ConfigParameters parameters)
    : impl_(std::move(impl)), parameters_(std::move(parameters)) {}
ConfigSession::~ConfigSession() = default;

std::int64_t ConfigSession::processId() const noexcept {
    return impl_->worker ? impl_->worker->processId() : 0;
}

OcioHelperError ConfigSession::failure() const noexcept {
    return helperError(impl_->failure.load());
}

ConfigSession::Opened ConfigSession::open(const ConfigParameters& parameters, const int resource,
                                          const std::uint64_t resourceBytes,
                                          const std::function<bool()>& cancellation) {
    Opened result;
#ifndef __linux__
    // No confinement is qualified for the config group on this target: external configs are
    // unavailable here and are never parsed in-process.
    (void)parameters;
    (void)resource;
    (void)resourceBytes;
    (void)cancellation;
    result.helperError = OcioHelperError::HelperUnavailable;
    return result;
#else
    const auto failed = [&](const LutError error) {
        result.session.reset();
        result.reply.reset();
        result.helperError = helperError(error);
        return result;
    };
    if (cancellation && cancellation())
        return failed(LutError::HelperCancelled);
    auto impl = std::make_unique<Impl>();
    // The sealed resource stays resident while the helper lives, beside the session's own
    // records and slabs.
    impl->reservation =
        LutReservation::acquire(static_cast<std::size_t>(resourceBytes) + 2 * kLutSlabBytes);
    if (!impl->reservation)
        return failed(LutError::HelperMemoryLimit);
    const auto generation = nextLutGeneration();
    const LutFd listener(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    const auto address = lutSocketAddress(static_cast<std::uint64_t>(::getpid()), generation);
    if (listener.get() < 0 ||
        ::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), 1) != 0)
        return failed(LutError::HelperUnavailable);
    platform::ProcessOptions options;
    options.executable = workerPath();
    options.addressSpaceBytes = std::uint64_t{512} * 1024U * 1024U;
    options.messageGrace = std::chrono::milliseconds(50);
    options.termGrace = std::chrono::milliseconds(100);
    auto launched = platform::ProcessSupervisor::launch(options);
    if (const auto* failure = std::get_if<platform::ProcessFailure>(&launched))
        return failed(processError(*failure));
    impl->worker = std::move(std::get<std::unique_ptr<platform::ProcessSupervisor>>(launched));
    impl->listed = core::telemetry::HelperProcessRegistration(impl->worker->processId(),
                                                              "Colour worker", impl.get());
    auto hello = packet(kLutCommandHello, impl->nonce++);
    hello[5] = static_cast<std::uint64_t>(::getpid());
    hello[6] = generation;
    if (const auto error = impl->exchange(hello, std::chrono::seconds(5), cancellation);
        error != LutError::None)
        return failed(error);
    impl->descriptorSocket =
        ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    ucred peer{};
    socklen_t peerSize = sizeof(peer);
    if (impl->descriptorSocket < 0 ||
        ::getsockopt(impl->descriptorSocket, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) != 0 ||
        peer.pid != impl->worker->processId())
        return failed(LutError::HelperProtocolViolation);

    // Begin: the parameters, after which the helper is confined.
    const auto record = encodeConfigParameters(parameters);
    {
        const LutFd file(sealedFile(record));
        if (file.get() < 0)
            return failed(LutError::HelperMemoryLimit);
        const std::array descriptors{file.get()};
        if (!sendDescriptors(impl->descriptorSocket, descriptors))
            return failed(LutError::HelperProtocolViolation);
        auto message = packet(kConfigCommandBegin, impl->nonce++);
        message[4] = record.size();
        message[7] = inode(file.get());
        if (const auto error = impl->exchange(message, std::chrono::seconds(5), cancellation);
            error != LutError::None)
            return failed(error);
    }
    // Open: the sealed archive or resource table in, the description out.
    std::optional<std::vector<std::byte>> replyBytes;
    {
        const LutFd output(replyFile(kConfigMaximumReplyBytes));
        if (output.get() < 0 || !validSharedFile(resource, inode(resource), resourceBytes, true))
            return failed(LutError::HelperMemoryLimit);
        const std::array descriptors{resource, output.get()};
        if (!sendDescriptors(impl->descriptorSocket, descriptors))
            return failed(LutError::HelperProtocolViolation);
        auto message = packet(kConfigCommandOpen, impl->nonce++);
        message[4] = resourceBytes;
        message[7] = inode(resource);
        message[8] = kConfigMaximumReplyBytes;
        message[9] = inode(output.get());
        if (const auto error = impl->exchange(message, std::chrono::seconds(30), cancellation);
            error != LutError::None)
            return failed(error);
        if (message[6] == 0 || message[6] > kConfigMaximumReplyBytes)
            return failed(LutError::HelperProtocolViolation);
        replyBytes = readReply(output.get(), message[6]);
    }
    auto reply = replyBytes ? decodeConfigOpenReply(*replyBytes) : std::nullopt;
    if (!reply)
        return failed(LutError::HelperProtocolViolation);
    result.session = std::shared_ptr<ConfigSession>(new ConfigSession(std::move(impl), parameters));
    result.session->description_ = reply->description;
    result.reply = std::move(reply);
    return result;
#endif
}

RemoteProcessorBuild ConfigSession::buildProcessor(const std::uint32_t kind,
                                                   const std::string_view a,
                                                   const std::string_view b) const noexcept {
    RemoteProcessorBuild build;
    build.error = kRemoteHelperFailed;
#ifdef __linux__
    try {
        const auto record = encodeConfigProcessorRecipe(
            {static_cast<ConfigProcessorKind>(kind), std::string(a), std::string(b)});
        std::optional<std::vector<std::byte>> bytes;
        {
            const std::lock_guard lock(impl_->mutex);
            bytes = impl_->request(kConfigCommandBuildProcessor, record, kConfigMaximumReplyBytes,
                                   std::chrono::seconds(30), {});
        }
        const auto reply = bytes ? decodeConfigProcessorReply(*bytes) : std::nullopt;
        if (!reply) {
            if (bytes) {
                const std::lock_guard lock(impl_->mutex);
                impl_->fail(LutError::HelperProtocolViolation);
            }
            return build;
        }
        build.error = static_cast<std::uint32_t>(reply->error);
        build.identity = reply->identity;
        build.cacheId = reply->cacheId;
        if (reply->error == ConfigProcessorError::None && !reply->identity) {
            if (reply->token == 0 || reply->token > kConfigMaximumProcessors) {
                const std::lock_guard lock(impl_->mutex);
                impl_->fail(LutError::HelperProtocolViolation);
                build.error = kRemoteHelperFailed;
                return build;
            }
            build.processor = std::make_shared<SessionProcessor>(shared_from_this(), reply->token);
        }
    } catch (const std::exception&) {
        build.processor.reset();
        build.error = kRemoteHelperFailed;
    }
#else
    (void)kind;
    (void)a;
    (void)b;
#endif
    return build;
}

std::vector<std::byte> ConfigSession::extractGpuProgram(
    const std::uint32_t kind, const std::string_view a, const std::string_view b,
    const render::OcioGpuProgramStage stage, const std::string_view semanticsId,
    const render::OcioGpuProgramLimits& limits, render::OcioGpuProgramError& error) const noexcept {
    error = render::OcioGpuProgramError::GpuProcessorUnavailable;
#ifdef __linux__
    try {
        const auto reservation = LutReservation::acquire(kMaximumGpuProgramBytes);
        if (!reservation) {
            error = render::OcioGpuProgramError::ResourceLimitExceeded;
            return {};
        }
        ConfigGpuProgramRecipe recipe;
        recipe.processor = {static_cast<ConfigProcessorKind>(kind), std::string(a), std::string(b)};
        recipe.stage = stage;
        recipe.semanticsId = semanticsId;
        recipe.limits = limits;
        const auto record = encodeConfigGpuProgramRecipe(recipe);
        std::optional<std::vector<std::byte>> bytes;
        {
            const std::lock_guard lock(impl_->mutex);
            bytes = impl_->request(kConfigCommandExtractGpuProgram, record, kMaximumGpuProgramBytes,
                                   std::chrono::seconds(30), {});
        }
        if (!bytes) {
            error = render::OcioGpuProgramError::ShaderExtractionFailed;
            return {};
        }
        ConfigReader reader(*bytes);
        const auto code = reader.u32();
        if (!reader.ok() ||
            code > static_cast<std::uint32_t>(render::OcioGpuProgramError::Cancelled) ||
            (code == 0) != (bytes->size() > sizeof(std::uint32_t))) {
            const std::lock_guard lock(impl_->mutex);
            impl_->fail(LutError::HelperProtocolViolation);
            error = render::OcioGpuProgramError::ShaderExtractionFailed;
            return {};
        }
        error = static_cast<render::OcioGpuProgramError>(code);
        if (code != 0)
            return {};
        return {bytes->begin() + sizeof(std::uint32_t), bytes->end()};
    } catch (const std::bad_alloc&) {
        error = render::OcioGpuProgramError::ResourceLimitExceeded;
    } catch (const std::exception&) {
        error = render::OcioGpuProgramError::ShaderExtractionFailed;
    }
#else
    (void)kind;
    (void)a;
    (void)b;
    (void)stage;
    (void)semanticsId;
    (void)limits;
#endif
    return {};
}

bool ConfigSession::apply(const std::uint64_t token, std::span<float> rgba,
                          const bool packed) const noexcept {
#ifndef __linux__
    (void)token;
    (void)rgba;
    (void)packed;
    return false;
#else
    try {
        if (rgba.size() % 4 != 0)
            return false;
        while (!rgba.empty()) {
            const auto slab = rgba.first(std::min(rgba.size(), kLutSlabBytes / sizeof(float)));
            const LutFd input(sealedFile(std::as_bytes(slab)));
            const LutFd output(replyFile(slab.size_bytes()));
            if (input.get() < 0 || output.get() < 0)
                return false;
            const std::lock_guard lock(impl_->mutex);
            if (impl_->failure != LutError::None)
                return false;
            const std::array descriptors{input.get(), output.get()};
            if (!sendDescriptors(impl_->descriptorSocket, descriptors)) {
                impl_->fail(LutError::HelperProtocolViolation);
                return false;
            }
            auto message = packet(kConfigCommandApplySlab, impl_->nonce++);
            message[4] = slab.size_bytes();
            message[7] = inode(input.get());
            message[9] = inode(output.get());
            message[10] = token;
            message[11] = static_cast<std::uint64_t>(packed ? ConfigApplyMode::Packed
                                                            : ConfigApplyMode::PerPixel);
            if (impl_->exchange(message, std::chrono::seconds(10), {}) != LutError::None)
                return false;
            if (message[6] > 1) {
                impl_->fail(LutError::HelperProtocolViolation);
                return false;
            }
            // 1: a non-finite input or result; the helper keeps serving.
            if (message[6] == 1)
                return false;
            // Nothing is published until the whole slab was validated here.
            std::vector<float> result(slab.size());
            if (!sealInput(output.get()) || ::lseek(output.get(), 0, SEEK_SET) != 0 ||
                !readFd(output.get(), std::as_writable_bytes(std::span(result)))) {
                impl_->fail(LutError::HelperProtocolViolation);
                return false;
            }
            for (std::size_t index = 0; index < result.size(); index += 4) {
                if (!std::isfinite(result[index]) || !std::isfinite(result[index + 1]) ||
                    !std::isfinite(result[index + 2]))
                    return false;
                // Alpha never crosses back: the helper's fourth lane is not trusted.
                result[index + 3] = slab[index + 3];
            }
            std::ranges::copy(result, slab.begin());
            rgba = rgba.subspan(slab.size());
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
#endif
}

} // namespace bloom::color::detail
