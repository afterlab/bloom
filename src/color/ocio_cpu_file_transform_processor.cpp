#include "lut_resource_budget.hpp"
#include "lut_worker_ipc.hpp"
#include "lut_worker_protocol.hpp"
#include <algorithm>
#include <atomic>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <mutex>
#include <thread>

namespace bloom::color {
namespace {
using Clock = std::chrono::steady_clock;
LutError processError(const platform::ProcessFailure failure) {
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
#if defined(__linux__) || defined(__APPLE__)
std::string workerPath() {
    std::error_code error;
#ifdef __linux__
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        const auto installed = executable.parent_path() / "../libexec/bloom/bloom-color-worker";
        if (std::filesystem::is_regular_file(installed, error))
            return installed.lexically_normal().string();
    }
#else
    // Beside the executable: an application bundle's Contents/MacOS, or a tool's own directory.
    std::uint32_t size = 0;
    static_cast<void>(::_NSGetExecutablePath(nullptr, &size));
    std::string buffer(size + 1U, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) == 0) {
        buffer.resize(std::char_traits<char>::length(buffer.c_str()));
        const auto executable = std::filesystem::weakly_canonical(buffer, error);
        if (!error) {
            const auto installed = executable.parent_path() / "bloom-color-worker";
            if (std::filesystem::is_regular_file(installed, error))
                return installed.string();
        }
    }
#endif
#ifdef BLOOM_COLOR_WORKER
    return BLOOM_COLOR_WORKER;
#else
    return {};
#endif
}
#endif
} // namespace

class CpuFileTransformProcessor::Impl final {
  public:
    std::shared_ptr<const detail::LutReservation> reservation;
    std::unique_ptr<platform::ProcessSupervisor> worker;
    // Listed for the Profiler while the colour worker lives (#423).
    core::telemetry::HelperProcessRegistration listed;
    mutable std::atomic_flag busy = ATOMIC_FLAG_INIT;
#if defined(__linux__) || defined(__APPLE__)
    int descriptorSocket = -1;
    ~Impl() {
        if (descriptorSocket >= 0)
            ::close(descriptorSocket);
    }
#endif
    std::uint64_t nonce = 1;
    bool identity = false;
    std::atomic<LutError> failure{LutError::None};
    LutError exchange(detail::LutPacket& message, const std::chrono::seconds deadline,
                      const std::function<bool()>& cancellation) {
        if (failure != LutError::None)
            return failure;
        const auto expiry = Clock::now() + deadline;
        const auto command = message[2], expectedNonce = message[3], bytes = message[4];
        const auto sentPacket = message;
        const auto sent = worker->write(std::as_bytes(std::span(message)), expiry, cancellation);
        if (const auto* error = std::get_if<platform::ProcessFailure>(&sent))
            failure = processError(*error);
        else {
            const auto read =
                worker->read(std::as_writable_bytes(std::span(message)), expiry, cancellation);
            if (const auto* readError = std::get_if<platform::ProcessFailure>(&read))
                failure = processError(*readError);
            else if (!detail::validPacket(message, command, expectedNonce) || message[4] != bytes ||
                     message[5] > static_cast<std::uint64_t>(LutError::InvalidPixel) ||
                     message[6] > 1 ||
                     !std::equal(message.begin() + 7, message.end(), sentPacket.begin() + 7))
                failure = LutError::HelperProtocolViolation;
            else
                failure = static_cast<LutError>(message[5]);
        }
        if (failure != LutError::None)
            worker->stop();
        return failure;
    }
};
CpuFileTransformProcessor::CpuFileTransformProcessor(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CpuFileTransformProcessor::~CpuFileTransformProcessor() = default;
bool CpuFileTransformProcessor::isAvailable() const noexcept {
    return impl_->failure.load() == LutError::None;
}
bool CpuFileTransformProcessor::isIdentity() const noexcept { return impl_->identity; }

CpuFileTransformProcessor::Result
CpuFileTransformProcessor::prepare(const LutFile& file, const LutInterpolation interpolation,
                                   const LutDirection direction,
                                   const std::function<bool()>& cancellation) {
    if (file.error != LutError::None)
        return {{}, file.error};
    if (file.bytes.empty() || file.bytes.size() > kMaximumLutBytes || file.format < 1 ||
        file.format > 4 || interpolation > LutInterpolation::Best ||
        direction > LutDirection::Inverse)
        return {{}, LutError::MalformedFile};
    if (cancellation && cancellation())
        return {{}, LutError::HelperCancelled};
#if !defined(__linux__) && !defined(__APPLE__)
    return {{}, LutError::HelperUnavailable};
#else
    const auto scratch = detail::LutReservation::acquire(kMaximumLutBytes);
    if (!scratch)
        return {{}, LutError::HelperMemoryLimit};
    const auto digest = core::Sha256Hasher::hash(file.bytes);
    if (!digest || *digest != file.digest)
        return {{}, LutError::ChangedFile};
    auto impl = std::make_unique<Impl>();
    impl->reservation = detail::LutReservation::acquire(std::size_t{1024} * 1024U);
    if (!impl->reservation)
        return {{}, LutError::HelperMemoryLimit};
    static std::atomic<std::uint64_t> generations{0};
    const auto generation = ++generations;
    detail::LutListener listener(static_cast<std::uint64_t>(::getpid()), generation);
    if (!listener.valid())
        return {{}, LutError::HelperUnavailable};
    platform::ProcessOptions options;
    options.executable = workerPath();
    options.addressSpaceBytes = std::uint64_t{512} * 1024U * 1024U;
    options.messageGrace = std::chrono::milliseconds(50);
    options.termGrace = std::chrono::milliseconds(100);
    auto launched = platform::ProcessSupervisor::launch(options);
    if (const auto* failure = std::get_if<platform::ProcessFailure>(&launched))
        return {{}, processError(*failure)};
    impl->worker = std::move(std::get<std::unique_ptr<platform::ProcessSupervisor>>(launched));
    impl->listed = core::telemetry::HelperProcessRegistration(impl->worker->processId(),
                                                              "Colour worker", impl.get());
    auto hello = detail::packet(0, impl->nonce++);
    hello[5] = static_cast<std::uint64_t>(::getpid());
    hello[6] = generation;
    if (const auto error = impl->exchange(hello, std::chrono::seconds(5), cancellation);
        error != LutError::None)
        return {{}, error};
    impl->descriptorSocket = listener.accept();
    if (impl->descriptorSocket < 0 ||
        !detail::lutPeerIs(impl->descriptorSocket, impl->worker->processId()))
        return {{}, LutError::HelperProtocolViolation};
    detail::LutFd resource(detail::sharedFile());
    if (resource.get() < 0 || !detail::writeFd(resource.get(), file.bytes) ||
        !detail::sealInput(resource.get()))
        return {{}, LutError::HelperMemoryLimit};
    const std::array resourceDescriptors{resource.get()};
    if (!detail::sendDescriptors(impl->descriptorSocket, resourceDescriptors))
        return {{}, LutError::HelperProtocolViolation};
    auto message = detail::packet(1, impl->nonce++);
    message[4] = file.bytes.size();
    message[5] = static_cast<std::uint64_t>(::getpid());
    message[6] = static_cast<std::uint64_t>(resource.get());
    message[7] = detail::inode(resource.get());
    message[8] = file.format;
    message[9] = static_cast<std::uint64_t>(interpolation);
    message[10] = static_cast<std::uint64_t>(direction);
    if (const auto error = impl->exchange(message, std::chrono::seconds(30), cancellation);
        error != LutError::None)
        return {{}, error};
    impl->identity = message[6] == 1;
    return {std::shared_ptr<const CpuFileTransformProcessor>(
                new CpuFileTransformProcessor(std::move(impl))),
            LutError::None};
#endif
}

LutError CpuFileTransformProcessor::apply(std::span<std::array<float, 4>> pixels,
                                          const std::function<bool()>& cancellation) const {
#if !defined(__linux__) && !defined(__APPLE__)
    (void)pixels;
    (void)cancellation;
    return LutError::HelperUnavailable;
#else
    const auto reservation = detail::LutReservation::acquire(2 * detail::kLutSlabBytes);
    if (!reservation)
        return LutError::HelperMemoryLimit;
    while (!pixels.empty()) {
        if (cancellation && cancellation())
            return LutError::HelperCancelled;
        const auto slab =
            pixels.first(std::min(pixels.size(), detail::kLutSlabBytes / sizeof(pixels[0])));
        detail::LutFd input(detail::sharedFile()), output(detail::sharedFile());
        if (input.get() < 0 || output.get() < 0 ||
            !detail::writeFd(input.get(), std::as_bytes(slab)) || !detail::sealInput(input.get()) ||
            ::ftruncate(output.get(), static_cast<off_t>(slab.size_bytes())) != 0 ||
            !detail::sealExtent(output.get()))
            return LutError::HelperMemoryLimit;
        while (impl_->busy.test_and_set(std::memory_order_acquire)) {
            if (cancellation && cancellation())
                return LutError::HelperCancelled;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        struct Release final {
            std::atomic_flag& flag;
            ~Release() { flag.clear(std::memory_order_release); }
        } release{impl_->busy};
        const std::array slabDescriptors{input.get(), output.get()};
        if (!detail::sendDescriptors(impl_->descriptorSocket, slabDescriptors))
            return LutError::HelperProtocolViolation;
        auto message = detail::packet(2, impl_->nonce++);
        message[4] = slab.size_bytes();
        message[5] = static_cast<std::uint64_t>(::getpid());
        message[6] = static_cast<std::uint64_t>(input.get());
        message[7] = detail::inode(input.get());
        message[8] = static_cast<std::uint64_t>(output.get());
        message[9] = detail::inode(output.get());
        if (const auto error = impl_->exchange(message, std::chrono::seconds(10), cancellation);
            error != LutError::None)
            return error;
        if (!detail::sealInput(output.get()) ||
            !detail::readFd(output.get(), std::as_writable_bytes(slab)))
            return LutError::HelperProtocolViolation;
        for (const auto& pixel : slab)
            if (!std::ranges::all_of(pixel, [](const float value) { return std::isfinite(value); }))
                return LutError::InvalidPixel;
        pixels = pixels.subspan(slab.size());
    }
    return LutError::None;
#endif
}
} // namespace bloom::color
