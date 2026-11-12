#include "lut_preflight.hpp"
#include "lut_worker_ipc.hpp"
#include "lut_worker_protocol.hpp"
#include "ocio_config_protocol.hpp"
#include "ocio_config_worker.hpp"
#include "ocio_gpu_program_extract.hpp"
#include "ocio_gpu_program_serialize.hpp"
#include <OpenColorIO/OpenColorIO.h>
#include <algorithm>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#ifdef __linux__
#include <cerrno>
#include <cstddef>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <sandbox.h>
#include <sys/resource.h>
#endif
#if defined(__APPLE__) || defined(_WIN32)
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>
#endif

namespace {
using namespace bloom::color;
namespace OCIO = OCIO_NAMESPACE;
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#ifdef __linux__
// The helper receives only anonymous sealed files. Landlock denies access to the filesystem;
// seccomp denies networking, child creation, execution, and process-memory/signal access.
bool confine() {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return false;
    landlock_ruleset_attr attributes{};
    attributes.handled_access_fs = (1ULL << 13U) - 1;
    detail::LutFd rules(static_cast<int>(::syscall(SYS_landlock_create_ruleset, &attributes,
                                                   sizeof(attributes.handled_access_fs), 0)));
    if (rules.get() < 0 || ::syscall(SYS_landlock_restrict_self, rules.get(), 0) != 0)
        return false;
#if defined(__x86_64__)
    constexpr auto architecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr auto architecture = AUDIT_ARCH_AARCH64;
#else
    return false;
#endif
#if defined(__x86_64__) || defined(__aarch64__)
    std::vector<sock_filter> filters{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, architecture, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr))};
    const auto deny = [&](const std::uint32_t number) {
        filters.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, number, 0, 1));
        filters.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM));
    };
    for (const auto call :
         {SYS_socket, SYS_socketpair, SYS_connect, SYS_bind, SYS_listen, SYS_clone, SYS_clone3,
          SYS_execve, SYS_execveat, SYS_ptrace, SYS_process_vm_readv, SYS_process_vm_writev,
          SYS_kill, SYS_tkill, SYS_tgkill, SYS_io_uring_setup})
        deny(static_cast<std::uint32_t>(call));
#ifdef SYS_fork
    deny(SYS_fork);
    deny(SYS_vfork);
#endif
    // x32 uses a second syscall namespace on x86-64.
#if defined(__x86_64__)
    filters.push_back(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000U, 0, 1));
    filters.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
#endif
    filters.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    sock_fprog program{static_cast<unsigned short>(filters.size()), filters.data()};
    return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
#endif
}

// OpenColorIO reads a LUT by path; the sealed descriptor has one under /proc.
OCIO::ConstProcessorRcPtr lutProcessor(const detail::LutHelperFile& file,
                                       const std::vector<std::byte>&,
                                       const OCIO::FileTransformRcPtr& transform) {
    const auto path = "/proc/self/fd/" + std::to_string(file.get());
    transform->setSrc(path.c_str());
    return OCIO::Config::CreateRaw()->getProcessor(transform);
}
#else
#ifdef _WIN32
// The supervisor launched this process confined, and the worker bootstrap completed it before
// any input was read: the process token names no account, group or privilege, so no file, device,
// registry key or other process can be opened, and the job allows no second process. What stays
// possible is reading and writing the pipes and mapping the sections it was handed. This checks
// that the confinement is really in force, by trying what it must refuse: opening this program's
// own file for reading.
bool confine() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return false;
    const HANDLE file = CreateFileW(path.data(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return false;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;
}
#else
// The helper receives only unnamed files. The system sandbox's pure-computation profile then
// denies every file open, every connection, child creation and signals to other processes; what
// stays possible is reading, writing and mapping the descriptors it already holds and receiving
// more over the connected descriptor socket. The file-size limit bounds what it can write through
// them. sandbox_init is deprecated without a public replacement for a process that confines
// itself; it is the same entry the system's own sandboxed tools use.
bool confine() {
    const rlim_t largest = detail::kMaximumGpuProgramBytes;
    const rlimit fileSize{largest, largest};
    if (::setrlimit(RLIMIT_FSIZE, &fileSize) != 0)
        return false;
    char* error = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const bool confined = ::sandbox_init(kSBXProfilePureComputation, SANDBOX_NAMED, &error) == 0;
    if (error != nullptr)
        ::sandbox_free_error(error);
#pragma clang diagnostic pop
    return confined;
}
#endif

// The confined helper cannot open a path, so OpenColorIO gets the LUT through its own interface
// for configurations that are not on disk: a raw configuration whose one file is these bytes. The
// name has no extension, as the descriptor path on Linux has none, so OpenColorIO detects the
// format from the content in the same way.
class LutBytes final : public OCIO::ConfigIOProxy {
  public:
    LutBytes(std::string config, const std::vector<std::byte>& bytes) : config_(std::move(config)) {
        lut_.resize(bytes.size());
        std::memcpy(lut_.data(), bytes.data(), bytes.size());
        static std::atomic<std::uint64_t> serial{0};
        hash_ = "bloom-lut-" + std::to_string(++serial);
    }
    std::vector<std::uint8_t> getLutData(const char* const path) const override {
        return path != nullptr && std::string_view(path) == kName ? lut_
                                                                  : std::vector<std::uint8_t>{};
    }
    std::string getConfigData() const override { return config_; }
    std::string getFastLutFileHash(const char*) const override { return hash_; }
    static constexpr const char* kName = "bloom-lut";

  private:
    std::string config_;
    std::vector<std::uint8_t> lut_;
    std::string hash_;
};
OCIO::ConstProcessorRcPtr lutProcessor(const detail::LutHelperFile&,
                                       const std::vector<std::byte>& bytes,
                                       const OCIO::FileTransformRcPtr& transform) {
    std::ostringstream raw;
    OCIO::Config::CreateRaw()->serialize(raw);
    const auto config =
        OCIO::Config::CreateFromConfigIOProxy(std::make_shared<LutBytes>(raw.str(), bytes));
    transform->setSrc(LutBytes::kName);
    return config->getProcessor(transform);
}
#endif

LutError build(const detail::LutPacket& message, OCIO::ConstCPUProcessorRcPtr& cpu,
               const int descriptorSocket, bool& identity) {
    if (!message[4] || message[4] > kMaximumLutBytes || message[8] < 1 || message[8] > 4 ||
        message[9] > 2 || message[10] > 1)
        return LutError::HelperProtocolViolation;
    const auto files = detail::receiveFiles(descriptorSocket, 1, {message[7], 0});
    const auto& file = files[0];
    if (!detail::validHelperFile(file, message[7], message[4], true))
        return LutError::HelperProtocolViolation;
    if (!confine())
        return LutError::HelperUnavailable;
    try {
        std::vector<std::byte> bytes(static_cast<std::size_t>(message[4]));
        if (!detail::readHelperFile(file, bytes))
            return LutError::HelperProtocolViolation;
        const auto checked = detail::preflightLut(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
            static_cast<std::uint32_t>(message[8]), {}, &identity);
        if (checked != LutError::None)
            return checked;
        auto transform = OCIO::FileTransform::Create();
        constexpr std::array interpolation{OCIO::INTERP_LINEAR, OCIO::INTERP_TETRAHEDRAL,
                                           OCIO::INTERP_BEST};
        transform->setInterpolation(interpolation[message[9]]);
        transform->setDirection(message[10] == 0 ? OCIO::TRANSFORM_DIR_FORWARD
                                                 : OCIO::TRANSFORM_DIR_INVERSE);
        const auto processor = lutProcessor(file, bytes, transform);
        // OCIO auto-detects the sealed descriptor's format. Check its unoptimized operations
        // as well as the declared-format preflight before admitting a CPU processor.
        const auto group = processor->createGroupTransform();
        for (int index = 0; index < group->getNumTransforms(); ++index) {
            const auto& operation = group->getTransform(index);
            const auto* lut = dynamic_cast<const OCIO::Lut3DTransform*>(operation.get());
            if (lut && lut->getGridSize() > kMaximumLut3dEdge)
                return LutError::EdgeTooLarge;
        }
        cpu = processor->getDefaultCPUProcessor();
        return cpu ? LutError::None : LutError::TransformBuildFailed;
    } catch (const std::bad_alloc&) {
        return LutError::HelperMemoryLimit;
    } catch (const std::exception&) {
        return LutError::MalformedFile;
    }
}

// Reflects the executable OCIO GPU program for one FileTransform into the sealed output resource.
// The LUT is parsed only here, behind the same Landlock/seccomp confinement and structural
// preflight as the CPU path; the host revalidates the transported payload before accepting it.
LutError extractGpuProgram(const detail::LutPacket& message, const int descriptorSocket,
                           std::uint64_t& programBytes) {
    programBytes = 0;
    const auto request = detail::LutGpuProgramRequest::decode(message);
    if (!request.lutBytes || request.lutBytes > kMaximumLutBytes || request.lutFormat < 1 ||
        request.lutFormat > 4 || request.interpolation > 2 || request.direction > 1 ||
        request.programExtent == 0 || request.programExtent > detail::kMaximumGpuProgramBytes) {
        return LutError::HelperProtocolViolation;
    }
    const auto files =
        detail::receiveFiles(descriptorSocket, 2, {request.lutInode, request.programInode});
    const auto& input = files[0];
    const auto& output = files[1];
    if (!detail::validHelperFile(input, request.lutInode, request.lutBytes, true) ||
        !detail::validHelperFile(output, request.programInode, request.programExtent, false)) {
        return LutError::HelperProtocolViolation;
    }
    if (!confine()) {
        return LutError::HelperUnavailable;
    }
    try {
        std::vector<std::byte> bytes(static_cast<std::size_t>(request.lutBytes));
        if (!detail::readHelperFile(input, bytes)) {
            return LutError::HelperProtocolViolation;
        }
        const auto checked = detail::preflightLut(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
            static_cast<std::uint32_t>(request.lutFormat), {}, nullptr);
        if (checked != LutError::None) {
            return checked;
        }
        auto transform = OCIO::FileTransform::Create();
        constexpr std::array interpolation{OCIO::INTERP_LINEAR, OCIO::INTERP_TETRAHEDRAL,
                                           OCIO::INTERP_BEST};
        transform->setInterpolation(interpolation[request.interpolation]);
        transform->setDirection(request.direction == 0 ? OCIO::TRANSFORM_DIR_FORWARD
                                                       : OCIO::TRANSFORM_DIR_INVERSE);
        const auto processor = lutProcessor(input, bytes, transform);
        const auto group = processor->createGroupTransform();
        for (int index = 0; index < group->getNumTransforms(); ++index) {
            const auto& operation = group->getTransform(index);
            const auto* lut = dynamic_cast<const OCIO::Lut3DTransform*>(operation.get());
            if (lut && lut->getGridSize() > kMaximumLut3dEdge) {
                return LutError::EdgeTooLarge;
            }
        }
        auto extracted = bloom::color::detail::extractOcioGpuProgram(
            processor, bloom::render::OcioGpuProgramStage::ProcessEffect,
            bloom::color::kOcioGpuFileTransformSemanticsId, {});
        if (!extracted.succeeded() || extracted.reflected() == nullptr) {
            return LutError::TransformBuildFailed;
        }
        auto serialized = bloom::color::detail::serializeOcioGpuProgram(*extracted.reflected(),
                                                                        request.programExtent);
        if (!serialized) {
            return LutError::FileTooLarge;
        }
        if (!detail::writeHelperFile(output, std::span<const std::byte>(*serialized))) {
            return LutError::HelperProtocolViolation;
        }
        programBytes = serialized->size();
        return LutError::None;
    } catch (const std::bad_alloc&) {
        return LutError::HelperMemoryLimit;
    } catch (const std::exception&) {
        return LutError::MalformedFile;
    }
}

// A shared file's pixels in this process's memory.
#ifdef _WIN32
class Mapping final {
  public:
    Mapping(const detail::LutHelperFile& file, const std::size_t size, const bool writable)
        : view_(file.get(), size, writable), size_(size) {}
    [[nodiscard]] bool valid() const { return view_.valid(); }
    [[nodiscard]] std::span<std::array<float, 4>> pixels() const {
        return {static_cast<std::array<float, 4>*>(view_.data()),
                size_ / sizeof(std::array<float, 4>)};
    }

  private:
    detail::LutView view_;
    std::size_t size_;
};
#else
class Mapping final {
  public:
    Mapping(const detail::LutHelperFile& file, const std::size_t size, const bool writable)
        : data_(::mmap(nullptr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED,
                       file.get(), 0)),
          size_(size) {}
    ~Mapping() {
        if (data_ != MAP_FAILED)
            ::munmap(data_, size_);
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    [[nodiscard]] bool valid() const { return data_ != MAP_FAILED; }
    [[nodiscard]] std::span<std::array<float, 4>> pixels() const {
        return {static_cast<std::array<float, 4>*>(data_), size_ / sizeof(std::array<float, 4>)};
    }

  private:
    void* data_;
    std::size_t size_;
};
#endif

LutError applySlab(const detail::LutPacket& message, const OCIO::ConstCPUProcessorRcPtr& cpu,
                   const int descriptorSocket, const bool identity) {
    if (!cpu || !message[4] || message[4] > detail::kLutSlabBytes || message[4] % 16 != 0)
        return LutError::HelperProtocolViolation;
    const auto files = detail::receiveFiles(descriptorSocket, 2, {message[7], message[9]});
    const auto& input = files[0];
    const auto& output = files[1];
    if (!detail::validHelperFile(input, message[7], message[4], true) ||
        !detail::validHelperFile(output, message[9], message[4], false))
        return LutError::HelperProtocolViolation;
    const Mapping in(input, static_cast<std::size_t>(message[4]), false),
        out(output, static_cast<std::size_t>(message[4]), true);
    if (!in.valid() || !out.valid())
        return LutError::HelperMemoryLimit;
    try {
        const auto source = in.pixels(), destination = out.pixels();
        std::ranges::copy(source, destination.begin());
        for (auto& pixel : destination) {
            if (!std::ranges::all_of(pixel, [](const float f) { return std::isfinite(f); }))
                return LutError::InvalidPixel;
            if (!identity)
                cpu->applyRGB(pixel.data());
            if (!std::ranges::all_of(pixel, [](const float f) { return std::isfinite(f); }))
                return LutError::InvalidPixel;
        }
    } catch (const std::exception&) {
        return LutError::TransformBuildFailed;
    }
    return LutError::None;
}
#endif
} // namespace

int main() {
#if !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)
    return 1;
#else
    try {
        if (!bloom::platform::processWorkerBootstrap())
            return 1;
#ifdef _WIN32
        // Shared files arrive as handles named in each request; there is no descriptor socket.
        const HANDLE standardInput = GetStdHandle(STD_INPUT_HANDLE);
        const HANDLE standardOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        struct {
            [[nodiscard]] int get() const { return -1; }
        } descriptorSocket;
#else
        constexpr int standardInput = STDIN_FILENO;
        constexpr int standardOutput = STDOUT_FILENO;
        detail::LutFd descriptorSocket(detail::lutHelperSocket());
        if (descriptorSocket.get() < 0)
            return 1;
#endif
        OCIO::ConstCPUProcessorRcPtr cpu;
        bool identity = false;
        // The config request group (ocio_config_protocol.hpp). A helper serves one group: once a
        // config command arrived, the LUT commands below are refused, and the reverse.
        detail::ConfigWorker configs;
        bool servesConfig = false;
        std::uint64_t nonce = 0;
        while (true) {
            detail::LutPacket message{};
            if (!detail::readFd(standardInput, std::as_writable_bytes(std::span(message))))
                return 0;
            if (message[0] != detail::kLutProtocolMagic ||
                message[1] != detail::kLutProtocolVersion || message[3] != ++nonce)
                return 1;
            LutError error = LutError::HelperProtocolViolation;
            std::uint64_t payload = 0;
            if (message[2] == detail::kLutCommandHello && nonce == 1) {
#ifdef _WIN32
                error = LutError::None;
#else
                error = detail::lutConnect(descriptorSocket.get(), message[5], message[6])
                            ? LutError::None
                            : LutError::HelperUnavailable;
#endif
            } else if (message[2] >= detail::kConfigCommandBegin &&
                       message[2] <= detail::kConfigCommandLast && nonce >= 2 &&
                       (servesConfig || nonce == 2)) {
#ifdef _WIN32
                // Config requests are not served by the Windows helper; the host reports the
                // typed unavailability and never sends them.
                error = LutError::HelperUnavailable;
#else
                servesConfig = true;
                error = configs.handle(message, descriptorSocket.get(), &confine, payload);
#endif
            } else if (servesConfig) {
                error = LutError::HelperProtocolViolation;
            } else if (message[2] == detail::kLutCommandBuildCpu && nonce == 2) {
                error = build(message, cpu, descriptorSocket.get(), identity);
                payload = cpu && (identity || cpu->isIdentity()) ? 1 : 0;
            } else if (message[2] == detail::kLutCommandApplySlab && nonce > 2) {
                error = applySlab(message, cpu, descriptorSocket.get(), identity);
            } else if (message[2] == detail::kLutCommandExtractGpuProgram && nonce == 2) {
                error = extractGpuProgram(message, descriptorSocket.get(), payload);
            }
            message[5] = static_cast<std::uint64_t>(error);
            message[6] = payload;
            if (!detail::writeFd(standardOutput, std::as_bytes(std::span(message))))
                return 1;
            if (error != LutError::None)
                return 1;
        }
    } catch (const std::exception&) {
        return 1;
    }
#endif
}
