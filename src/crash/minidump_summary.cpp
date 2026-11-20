#include <bloom/crash/minidump_summary.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>

namespace bloom::crash {
namespace {

// The minidump format (minidumpapiset.h): a header, a stream directory and the streams.
constexpr std::uint32_t kSignature = 0x504d444dU; // "MDMP"
constexpr std::uint32_t kThreadListStream = 3;
constexpr std::uint32_t kExceptionStream = 6;
constexpr std::uint32_t kSystemInfoStream = 7;
constexpr std::uint32_t kMiscInfoStream = 15;
constexpr std::uint32_t kMiscInfoProcessId = 0x1;
constexpr std::size_t kThreadRecordBytes = 48;
// A minidump larger than this is not read: the summary needs only its small streams, and a
// bounded read keeps a damaged file from asking for an unbounded buffer.
constexpr std::uintmax_t kMaximumBytes = std::uintmax_t{1} << 31;

class Reader final {
  public:
    explicit Reader(std::vector<unsigned char> bytes) : bytes_(std::move(bytes)) {}

    [[nodiscard]] std::optional<std::uint32_t> u32(const std::size_t offset) const {
        if (offset > bytes_.size() || bytes_.size() - offset < 4)
            return std::nullopt;
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index)
            value |= static_cast<std::uint32_t>(bytes_[offset + index]) << (8 * index);
        return value;
    }
    [[nodiscard]] bool contains(const std::size_t offset, const std::size_t size) const {
        return offset <= bytes_.size() && bytes_.size() - offset >= size;
    }

  private:
    std::vector<unsigned char> bytes_;
};

[[nodiscard]] std::optional<std::vector<unsigned char>> readAll(const std::filesystem::path& file) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size < 32 || size > kMaximumBytes)
        return std::nullopt;
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return std::nullopt;
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
        return std::nullopt;
    return bytes;
}

[[nodiscard]] std::string linuxSignalName(const std::uint32_t signal) {
    switch (signal) {
    case 4:
        return "SIGILL";
    case 5:
        return "SIGTRAP";
    case 6:
        return "SIGABRT";
    case 7:
        return "SIGBUS";
    case 8:
        return "SIGFPE";
    case 11:
        return "SIGSEGV";
    case 31:
        return "SIGSYS";
    default:
        return "signal " + std::to_string(signal);
    }
}

[[nodiscard]] std::string macExceptionName(const std::uint32_t code) {
    switch (code) {
    case 1:
        return "EXC_BAD_ACCESS";
    case 2:
        return "EXC_BAD_INSTRUCTION";
    case 3:
        return "EXC_ARITHMETIC";
    case 6:
        return "EXC_BREAKPOINT";
    case 10:
        return "EXC_CRASH";
    case 11:
        return "EXC_RESOURCE";
    case 12:
        return "EXC_GUARD";
    default:
        return {};
    }
}

[[nodiscard]] std::string windowsExceptionName(const std::uint32_t code) {
    switch (code) {
    case 0xC0000005U:
        return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC00000FDU:
        return "EXCEPTION_STACK_OVERFLOW";
    case 0xC000001DU:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000094U:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC0000409U:
        return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xC0000374U:
        return "STATUS_HEAP_CORRUPTION";
    case 0x80000003U:
        return "EXCEPTION_BREAKPOINT";
    case 0x40000015U:
        return "STATUS_FATAL_APP_EXIT (abort)";
    default:
        return {};
    }
}

[[nodiscard]] std::string hex(const std::uint32_t value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4)
        text += digits[(value >> static_cast<unsigned>(shift)) & 0xFU];
    return text;
}

} // namespace

std::optional<MinidumpSummary> readMinidumpSummary(const std::filesystem::path& file) {
    auto bytes = readAll(file);
    if (!bytes)
        return std::nullopt;
    const Reader reader(std::move(*bytes));
    if (reader.u32(0) != kSignature)
        return std::nullopt;
    const auto streamCount = reader.u32(8);
    const auto directory = reader.u32(12);
    if (!streamCount || !directory || *streamCount > 4096 ||
        !reader.contains(*directory, std::size_t{*streamCount} * 12))
        return std::nullopt;

    MinidumpSummary summary;
    for (std::uint32_t index = 0; index < *streamCount; ++index) {
        const std::size_t entry = std::size_t{*directory} + std::size_t{index} * 12;
        const auto type = reader.u32(entry);
        const auto size = reader.u32(entry + 4);
        const auto location = reader.u32(entry + 8);
        if (!type || !size || !location || !reader.contains(*location, *size))
            return std::nullopt;
        if (*type == kThreadListStream) {
            const auto count = reader.u32(*location);
            if (!count || !reader.contains(std::size_t{*location} + 4,
                                           std::size_t{*count} * kThreadRecordBytes))
                return std::nullopt;
            for (std::uint32_t thread = 0; thread < *count; ++thread) {
                const std::size_t record =
                    std::size_t{*location} + 4 + std::size_t{thread} * kThreadRecordBytes;
                // ThreadId at 0; the stack's memory descriptor at 24: its start (8 bytes), then
                // its location's DataSize at 32 and Rva at 36.
                summary.threads.push_back(
                    {.id = *reader.u32(record), .stackBytes = *reader.u32(record + 32)});
            }
        } else if (*type == kExceptionStream && *size >= 16) {
            // ThreadId, alignment, then the exception record: its code and its flags.
            summary.crashedThreadId = reader.u32(*location);
            summary.exceptionCode = reader.u32(std::size_t{*location} + 8);
            summary.exceptionFlags = reader.u32(std::size_t{*location} + 12);
        } else if (*type == kSystemInfoStream && *size >= 24) {
            switch (reader.u32(std::size_t{*location} + 20).value_or(0)) {
            case 2:
                summary.platform = "windows";
                break;
            case 0x8101:
                summary.platform = "macos";
                break;
            case 0x8201:
                summary.platform = "linux";
                break;
            default:
                break;
            }
        } else if (*type == kMiscInfoStream && *size >= 12) {
            const auto flags = reader.u32(std::size_t{*location} + 4).value_or(0);
            if ((flags & kMiscInfoProcessId) != 0)
                summary.processId = reader.u32(std::size_t{*location} + 8);
        }
    }
    return summary;
}

std::string describeException(const MinidumpSummary& summary) {
    if (!summary.exceptionCode)
        return "no exception recorded";
    const auto code = *summary.exceptionCode;
    std::string name;
    if (summary.platform == "linux")
        name = linuxSignalName(code);
    else if (summary.platform == "macos" && code == 0 && summary.exceptionFlags)
        // A crash a signal caused, with no machine exception: Crashpad records the signal.
        return "EXC_CRASH from " + linuxSignalName(*summary.exceptionFlags);
    else if (summary.platform == "macos")
        name = macExceptionName(code);
    else if (summary.platform == "windows")
        name = windowsExceptionName(code);
    if (name.empty())
        return "exception " + hex(code);
    return name + " (" + hex(code) + ")";
}

} // namespace bloom::crash
