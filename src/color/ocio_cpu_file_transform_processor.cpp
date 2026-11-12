#include "lut_resource_budget.hpp"
#include "lut_worker_ipc.hpp"
#include "lut_worker_protocol.hpp"
#include "ocio_gpu_program_worker.hpp"
#include <algorithm>
#include <atomic>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
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
} // namespace

class CpuFileTransformProcessor::Impl final {
  public:
    std::shared_ptr<const detail::LutReservation> reservation;
    std::unique_ptr<platform::ProcessSupervisor> worker;
    // Listed for the Profiler while the colour worker lives (#423).
    core::telemetry::HelperProcessRegistration listed;
    mutable std::atomic_flag busy = ATOMIC_FLAG_INIT;
    // How shared files reach the helper.
    detail::LutFileChannel channel;
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
    if (!impl->channel.valid())
        return {{}, LutError::HelperUnavailable};
    platform::ProcessOptions options;
    options.executable = detail::lutWorkerPath();
    options.confined = detail::kLutLaunchConfined;
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
    impl->channel.announce(hello);
    if (const auto error = impl->exchange(hello, std::chrono::seconds(5), cancellation);
        error != LutError::None)
        return {{}, error};
    if (!impl->channel.attach(*impl->worker))
        return {{}, LutError::HelperProtocolViolation};
    const detail::LutSharedFile resource{std::span<const std::byte>(file.bytes)};
    if (!resource.valid())
        return {{}, LutError::HelperMemoryLimit};
    const auto shared = impl->channel.send(resource);
    if (!shared)
        return {{}, LutError::HelperProtocolViolation};
    auto message = detail::packet(1, impl->nonce++);
    message[4] = file.bytes.size();
    message[7] = (*shared)[0];
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
}

LutError CpuFileTransformProcessor::apply(std::span<std::array<float, 4>> pixels,
                                          const std::function<bool()>& cancellation) const {
    const auto reservation = detail::LutReservation::acquire(2 * detail::kLutSlabBytes);
    if (!reservation)
        return LutError::HelperMemoryLimit;
    while (!pixels.empty()) {
        if (cancellation && cancellation())
            return LutError::HelperCancelled;
        const auto slab =
            pixels.first(std::min(pixels.size(), detail::kLutSlabBytes / sizeof(pixels[0])));
        const detail::LutSharedFile input{std::as_bytes(slab)};
        const detail::LutSharedFile output{std::uint64_t{slab.size_bytes()}};
        if (!input.valid() || !output.valid())
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
        const auto shared = impl_->channel.send(input, &output);
        if (!shared)
            return LutError::HelperProtocolViolation;
        auto message = detail::packet(2, impl_->nonce++);
        message[4] = slab.size_bytes();
        message[7] = (*shared)[0];
        message[9] = (*shared)[1];
        if (const auto error = impl_->exchange(message, std::chrono::seconds(10), cancellation);
            error != LutError::None)
            return error;
        if (!output.freeze() || !output.read(std::as_writable_bytes(slab)))
            return LutError::HelperProtocolViolation;
        for (const auto& pixel : slab)
            if (!std::ranges::all_of(pixel, [](const float value) { return std::isfinite(value); }))
                return LutError::InvalidPixel;
        pixels = pixels.subspan(slab.size());
    }
    return LutError::None;
}
} // namespace bloom::color
