#include "interpreter_home.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace bloom::scripting::python {

CarriedPythonLayout carriedPythonLayout(const std::filesystem::path& program) {
    const auto directory = program.parent_path();
#if defined(_WIN32)
    return {directory / "python", directory / "bloom-python"};
#elif defined(__APPLE__)
    const auto resources = directory.parent_path() / "Resources";
    return {resources / "python", resources / "bloom-python"};
#else
    const auto share = directory.parent_path() / "share" / "bloom";
    return {share / "python", share / "bloom-python"};
#endif
}

std::filesystem::path standardLibrary(const std::filesystem::path& home, const int major,
                                      const int minor) {
#if defined(_WIN32)
    static_cast<void>(major);
    static_cast<void>(minor);
    return home / "Lib";
#else
    return home / "lib" / ("python" + std::to_string(major) + "." + std::to_string(minor));
#endif
}

std::filesystem::path sitePackages(const std::filesystem::path& home, const int major,
                                   const int minor) {
    return standardLibrary(home, major, minor) / "site-packages";
}

namespace {
bool holdsStandardLibrary(const std::filesystem::path& home, const int major, const int minor) {
    std::error_code error;
    return !home.empty() &&
           std::filesystem::is_regular_file(standardLibrary(home, major, minor) / "os.py", error);
}
} // namespace

CarriedPython locateCarriedPython(const std::filesystem::path& program,
                                  const std::filesystem::path& buildHome, const int major,
                                  const int minor) {
    CarriedPython carried;
    if (!program.empty()) {
        const auto layout = carriedPythonLayout(program);
        if (holdsStandardLibrary(layout.home, major, minor)) {
            carried.home = layout.home;
            std::error_code error;
            if (std::filesystem::is_directory(layout.package / "bloom", error))
                carried.package = layout.package;
            return carried;
        }
    }
    if (holdsStandardLibrary(buildHome, major, minor))
        carried.home = buildHome;
    return carried;
}

std::filesystem::path runningProgram() {
    std::error_code error;
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    auto resolved = std::filesystem::canonical(std::filesystem::path(buffer), error);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    static_cast<void>(::_NSGetExecutablePath(nullptr, &size));
    std::string buffer(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    auto resolved = std::filesystem::canonical(std::filesystem::path(buffer.c_str()), error);
#else
    auto resolved = std::filesystem::canonical("/proc/self/exe", error);
#endif
    return error ? std::filesystem::path{} : resolved;
}

std::optional<std::filesystem::path> systemCertificateFile() {
#if defined(_WIN32)
    return std::nullopt;
#else
#if defined(__APPLE__)
    constexpr std::array candidates{"/etc/ssl/cert.pem"};
#else
    // Debian, Ubuntu and Arch; Fedora and its relatives (two names); openSUSE; Alpine.
    constexpr std::array candidates{
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/ca-bundle.pem",
        "/etc/ssl/cert.pem",
    };
#endif
    for (const auto* candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return std::filesystem::path(candidate);
    }
    return std::nullopt;
#endif
}

void trustSystemCertificates() {
#if !defined(_WIN32)
    if (std::getenv("SSL_CERT_FILE") != nullptr || std::getenv("SSL_CERT_DIR") != nullptr)
        return;
    if (const auto file = systemCertificateFile())
        static_cast<void>(::setenv("SSL_CERT_FILE", file->c_str(), 0));
#endif
}

} // namespace bloom::scripting::python
