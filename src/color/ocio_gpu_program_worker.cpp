#include "ocio_gpu_program_worker.hpp"

#include "lut_resource_budget.hpp"
#include "lut_worker_ipc.hpp"
#include "lut_worker_protocol.hpp"
#include <bloom/core/telemetry_counters.hpp>

#include <bloom/platform/process_supervisor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace bloom::color::detail {
namespace {
using Clock = std::chrono::steady_clock;

[[nodiscard]] LutError processError(const platform::ProcessFailure failure) {
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

// Tracks the helper for the duration of one extraction and always reaps it, including every early
// return and any throw.
class WorkerGuard final {
  public:
    explicit WorkerGuard(std::unique_ptr<platform::ProcessSupervisor> worker)
        : worker_(std::move(worker)),
          listed_(worker_ ? worker_->processId() : 0, "Colour worker", this) {}
    ~WorkerGuard() {
        if (worker_) {
            worker_->stop();
        }
    }
    WorkerGuard(const WorkerGuard&) = delete;
    WorkerGuard& operator=(const WorkerGuard&) = delete;
    [[nodiscard]] platform::ProcessSupervisor* operator->() const noexcept { return worker_.get(); }
    [[nodiscard]] platform::ProcessSupervisor& operator*() const noexcept { return *worker_; }

  private:
    std::unique_ptr<platform::ProcessSupervisor> worker_;
    // Listed for the Profiler while the colour worker lives (#423).
    core::telemetry::HelperProcessRegistration listed_;
};
} // namespace

std::string lutWorkerPath() {
    std::error_code error;
#if defined(__linux__)
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        const auto installed = executable.parent_path() / "../libexec/bloom/bloom-color-worker";
        if (std::filesystem::is_regular_file(installed, error))
            return installed.lexically_normal().string();
    }
#elif defined(__APPLE__)
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
#elif defined(_WIN32)
    // Beside the executable, where the program's own libraries are.
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        const auto installed =
            std::filesystem::path(buffer).parent_path() / L"bloom-color-worker.exe";
        if (std::filesystem::is_regular_file(installed, error)) {
            const auto text = installed.u8string();
            return {reinterpret_cast<const char*>(text.data()), text.size()};
        }
    }
#endif
#ifdef BLOOM_COLOR_WORKER
    return BLOOM_COLOR_WORKER;
#else
    return {};
#endif
}

FileTransformGpuExtraction extractFileTransformGpuProgram(
    const std::span<const std::byte> lutBytes, const core::Sha256Digest& expectedDigest,
    const std::uint32_t lutFormat, const LutInterpolation interpolation,
    const LutDirection direction, const std::function<bool()>& cancellation) {
    FileTransformGpuExtraction result;
    if (lutBytes.empty() || lutBytes.size() > kMaximumLutBytes || lutFormat < 1 || lutFormat > 4 ||
        static_cast<std::uint32_t>(interpolation) >
            static_cast<std::uint32_t>(LutInterpolation::Best) ||
        static_cast<std::uint32_t>(direction) > static_cast<std::uint32_t>(LutDirection::Inverse) ||
        expectedDigest == core::Sha256Digest{}) {
        result.error = LutError::MalformedFile;
        return result;
    }
    if (cancellation && cancellation()) {
        result.error = LutError::HelperCancelled;
        return result;
    }
    const auto outputReservation = LutReservation::acquire(kMaximumGpuProgramBytes);
    if (!outputReservation) {
        result.error = LutError::HelperMemoryLimit;
        return result;
    }
    const auto digest = core::Sha256Hasher::hash(lutBytes);
    if (!digest || *digest != expectedDigest) {
        result.error = LutError::ChangedFile;
        return result;
    }
    LutFileChannel channel;
    if (!channel.valid()) {
        result.error = LutError::HelperUnavailable;
        return result;
    }
    platform::ProcessOptions options;
    options.executable = lutWorkerPath();
    options.confined = kLutLaunchConfined;
    options.addressSpaceBytes = std::uint64_t{512} * 1024U * 1024U;
    options.messageGrace = std::chrono::milliseconds(50);
    options.termGrace = std::chrono::milliseconds(100);
    auto launched = platform::ProcessSupervisor::launch(options);
    if (const auto* failure = std::get_if<platform::ProcessFailure>(&launched)) {
        result.error = processError(*failure);
        return result;
    }
    WorkerGuard worker(std::move(std::get<std::unique_ptr<platform::ProcessSupervisor>>(launched)));
    std::uint64_t nonce = 1;
    const auto exchange = [&](LutPacket& message, const std::chrono::seconds deadline) {
        const auto expiry = Clock::now() + deadline;
        const auto command = message[2];
        const auto expectedNonce = message[3];
        const auto sent = message;
        const auto written = worker->write(std::as_bytes(std::span(message)), expiry, cancellation);
        if (const auto* error = std::get_if<platform::ProcessFailure>(&written)) {
            return processError(*error);
        }
        const auto read =
            worker->read(std::as_writable_bytes(std::span(message)), expiry, cancellation);
        if (const auto* error = std::get_if<platform::ProcessFailure>(&read)) {
            return processError(*error);
        }
        if (!validPacket(message, command, expectedNonce) ||
            message[5] > static_cast<std::uint64_t>(LutError::InvalidPixel) ||
            !std::equal(message.begin() + 7, message.end(), sent.begin() + 7)) {
            return LutError::HelperProtocolViolation;
        }
        return static_cast<LutError>(message[5]);
    };
    auto hello = packet(kLutCommandHello, nonce++);
    channel.announce(hello);
    if (const auto error = exchange(hello, std::chrono::seconds(5)); error != LutError::None) {
        result.error = error;
        return result;
    }
    if (!channel.attach(*worker)) {
        result.error = LutError::HelperProtocolViolation;
        return result;
    }
    const LutSharedFile resource{lutBytes};
    const LutSharedFile programFile{std::uint64_t{kMaximumGpuProgramBytes}};
    if (!resource.valid() || !programFile.valid()) {
        result.error = LutError::HelperMemoryLimit;
        return result;
    }
    const auto shared = channel.send(resource, &programFile);
    if (!shared) {
        result.error = LutError::HelperProtocolViolation;
        return result;
    }
    const LutGpuProgramRequest request{
        .lutBytes = lutBytes.size(),
        .lutFormat = lutFormat,
        .interpolation = static_cast<std::uint64_t>(interpolation),
        .direction = static_cast<std::uint64_t>(direction),
        .lutInode = (*shared)[0],
        .programExtent = kMaximumGpuProgramBytes,
        .programInode = (*shared)[1],
    };
    auto message = request.encode(nonce++);
    if (const auto error = exchange(message, std::chrono::seconds(30)); error != LutError::None) {
        result.error = error;
        return result;
    }
    const LutGpuProgramReply reply{static_cast<LutError>(message[5]), message[6]};
    if (reply.programBytes == 0 || reply.programBytes > kMaximumGpuProgramBytes) {
        result.error = LutError::HelperProtocolViolation;
        return result;
    }
    try {
        result.bytes.resize(static_cast<std::size_t>(reply.programBytes));
    } catch (const std::bad_alloc&) {
        result.error = LutError::HelperMemoryLimit;
        return result;
    }
    if (!programFile.read(std::as_writable_bytes(std::span(result.bytes)))) {
        result.bytes.clear();
        result.error = LutError::HelperProtocolViolation;
        return result;
    }
    result.error = LutError::None;
    return result;
}

} // namespace bloom::color::detail
