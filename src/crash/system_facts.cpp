#include "crash_internal.hpp"

#include <bloom/core/version.hpp>

#include <array>
#include <charconv>
#include <ctime>
#include <fstream>
#include <system_error>
#include <tuple>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#ifndef BLOOM_CRASH_SOURCE_REVISION
#define BLOOM_CRASH_SOURCE_REVISION ""
#endif

namespace bloom::crash::detail {
namespace {

[[nodiscard]] std::string architecture() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::tm localTime(const std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    return parts;
}

#if defined(__linux__)
// PRETTY_NAME from os-release, unquoted.
[[nodiscard]] std::string distributionName() {
    for (const char* file : {"/etc/os-release", "/usr/lib/os-release"}) {
        std::ifstream stream(file);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.starts_with("PRETTY_NAME="))
                continue;
            std::string value = line.substr(12);
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
                value.back() == value.front())
                value = value.substr(1, value.size() - 2);
            return value;
        }
    }
    return "Linux";
}
#endif

#if defined(__APPLE__)
[[nodiscard]] std::string sysctlText(const char* name) {
    std::array<char, 256> buffer{};
    std::size_t size = buffer.size();
    if (sysctlbyname(name, buffer.data(), &size, nullptr, 0) != 0 || size == 0)
        return {};
    return std::string(buffer.data());
}
#endif

} // namespace

std::string platformName() {
#if defined(_WIN32)
    return "Windows " + architecture();
#elif defined(__APPLE__)
    return "macOS " + architecture();
#else
    return "Linux " + architecture();
#endif
}

std::string operatingSystemVersion() {
#if defined(_WIN32)
    // RtlGetVersion reports the real version; GetVersionEx answers what the manifest declares.
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    OSVERSIONINFOW information{};
    information.dwOSVersionInfoSize = sizeof(information);
    if (ntdll != nullptr) {
        const auto function = reinterpret_cast<RtlGetVersionFunction>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (function != nullptr && function(&information) == 0) {
            return "Windows " + std::to_string(information.dwMajorVersion) + "." +
                   std::to_string(information.dwMinorVersion) + " build " +
                   std::to_string(information.dwBuildNumber);
        }
    }
    return "Windows";
#elif defined(__APPLE__)
    const std::string product = sysctlText("kern.osproductversion");
    const std::string build = sysctlText("kern.osversion");
    std::string text = "macOS " + (product.empty() ? std::string("unknown") : product);
    if (!build.empty())
        text += " (" + build + ")";
    return text;
#else
    std::string text = distributionName();
    utsname name{};
    if (uname(&name) == 0)
        text += ", kernel " + std::string(name.release);
    return text;
#endif
}

std::string buildDescription() {
    std::string text(core::buildVersion());
#if defined(NDEBUG)
    text += ", release build";
#else
    text += ", debug build";
#endif
#if defined(__clang__)
    text += ", Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) +
            "." + std::to_string(__clang_patchlevel__);
#elif defined(_MSC_VER)
    text += ", MSVC " + std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
    text += ", GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#endif
    const std::string_view revision = BLOOM_CRASH_SOURCE_REVISION;
    if (!revision.empty())
        text += ", source " + std::string(revision);
    return text;
}

std::uint32_t currentProcessId() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

std::string readableTime(const std::chrono::system_clock::time_point time) {
    const std::tm parts = localTime(time);
    std::array<char, 64> buffer{};
    const std::size_t length =
        std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S %z", &parts);
    return std::string(buffer.data(), length);
}

std::string folderTime(const std::chrono::system_clock::time_point time) {
    const std::tm parts = localTime(time);
    std::array<char, 32> buffer{};
    const std::size_t length =
        std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d_%H-%M-%S", &parts);
    return std::string(buffer.data(), length);
}

std::optional<std::chrono::system_clock::time_point> parseFolderTime(const std::string_view text) {
    // YYYY-MM-DD_HH-MM-SS
    if (text.size() < 19)
        return std::nullopt;
    std::tm parts{};
    int year = 0;
    int month = 0;
    // Each field: its offset, its width and where it goes; the separators between them.
    const std::array<std::tuple<std::size_t, std::size_t, int*>, 6> fields{{
        {0, 4, &year},
        {5, 2, &month},
        {8, 2, &parts.tm_mday},
        {11, 2, &parts.tm_hour},
        {14, 2, &parts.tm_min},
        {17, 2, &parts.tm_sec},
    }};
    if (text[4] != '-' || text[7] != '-' || text[10] != '_' || text[13] != '-' || text[16] != '-')
        return std::nullopt;
    for (const auto& [offset, width, target] : fields) {
        const char* const first = text.data() + offset;
        const auto [end, error] = std::from_chars(first, first + width, *target);
        if (error != std::errc{} || end != first + width)
            return std::nullopt;
    }
    if (year < 1970 || month < 1 || month > 12 || parts.tm_mday < 1 || parts.tm_mday > 31 ||
        parts.tm_hour > 23 || parts.tm_min > 59 || parts.tm_sec > 60)
        return std::nullopt;
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_isdst = -1;
    const std::time_t seconds = std::mktime(&parts);
    if (seconds == static_cast<std::time_t>(-1))
        return std::nullopt;
    return std::chrono::system_clock::from_time_t(seconds);
}

} // namespace bloom::crash::detail
