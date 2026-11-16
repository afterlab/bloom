#include <bloom/update/install_kind.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <vector>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bloom::update {
namespace {

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
    constexpr std::string_view space = " \t\r";
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

[[nodiscard]] std::optional<std::string> readSmallFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string text;
    char character = 0;
    // A marker is a few lines; a file that is not one is not read to its end.
    while (text.size() < 4096 && file.get(character)) {
        text.push_back(character);
    }
    return text;
}

// Whether this user can replace `path` itself: rename it away and put another in its place,
// which is a change to the directory that holds it.
[[nodiscard]] bool canReplace(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return false;
    }
#if defined(_WIN32)
    // Opening the directory for adding entries asks the system the real question, access lists
    // included.
    const HANDLE handle = CreateFileW(parent.c_str(), FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(handle);
    return true;
#else
    return ::access(parent.c_str(), W_OK | X_OK) == 0;
#endif
}

} // namespace

std::string_view installKindName(const InstallKind kind) noexcept {
    switch (kind) {
    case InstallKind::Development:
        return "development";
    case InstallKind::MacBundle:
        return "mac-bundle";
    case InstallKind::WindowsInstaller:
        return "windows-installer";
    case InstallKind::WindowsPortable:
        return "windows-portable";
    case InstallKind::AppImage:
        return "appimage";
    case InstallKind::UserFolder:
        return "user-folder";
    case InstallKind::ReadOnlyFolder:
        return "read-only-folder";
    case InstallKind::PackageManager:
        return "package-manager";
    }
    return "development";
}

InstallMarker parseInstallMarker(std::string_view text) {
    InstallMarker marker;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = trimmed(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto equals = line.find('=');
        if (line.empty() || line.front() == '#' || equals == std::string_view::npos) {
            continue;
        }
        const auto name = trimmed(line.substr(0, equals));
        const auto value = trimmed(line.substr(equals + 1));
        if (name == "kind") {
            marker.kind = std::string(value);
        } else if (name == "manager") {
            marker.manager = std::string(value);
        }
    }
    return marker;
}

InstallKind classifyInstall(const InstallProbe& probe) {
    if (!probe.packageLayout) {
        return InstallKind::Development;
    }
    if (probe.platform == InstallPlatform::MacOS) {
        return InstallKind::MacBundle;
    }
    if (probe.marker.has_value()) {
        const auto marker = parseInstallMarker(*probe.marker);
        if (probe.platform == InstallPlatform::Windows) {
            return marker.kind == "windows-installer" ? InstallKind::WindowsInstaller
                                                      : InstallKind::PackageManager;
        }
        if (marker.kind != "script") {
            return InstallKind::PackageManager;
        }
        return probe.rootWritable ? InstallKind::UserFolder : InstallKind::ReadOnlyFolder;
    }
    if (probe.platform == InstallPlatform::Windows) {
        return InstallKind::WindowsPortable;
    }
    if (!probe.appImage.empty()) {
        return InstallKind::AppImage;
    }
    return probe.rootWritable ? InstallKind::UserFolder : InstallKind::ReadOnlyFolder;
}

InstallFacts detectInstall(const std::filesystem::path& executable,
                           const EnvironmentReader& environment) {
    InstallFacts facts;
    if (executable.empty()) {
        return facts;
    }
    InstallProbe probe;
    std::error_code error;
    const auto programDirectory = executable.parent_path();
    std::filesystem::path root;
    std::filesystem::path records;
#if defined(__APPLE__)
    probe.platform = InstallPlatform::MacOS;
    // Bloom.app/Contents/MacOS/<program>
    const auto contents = programDirectory.parent_path();
    root = contents.parent_path();
    records = contents / "Resources";
    probe.packageLayout = programDirectory.filename() == "MacOS" &&
                          contents.filename() == "Contents" && root.extension() == ".app";
#elif defined(_WIN32)
    probe.platform = InstallPlatform::Windows;
    root = programDirectory;
    records = programDirectory;
    probe.packageLayout = true;
#else
    probe.platform = InstallPlatform::Linux;
    // <root>/bin/<program>
    root = programDirectory.parent_path();
    records = root / "share" / "bloom";
    probe.packageLayout = programDirectory.filename() == "bin";
#endif
    // A package carries its license records; a build folder does not.
    probe.packageLayout =
        probe.packageLayout && std::filesystem::is_directory(records / "licenses", error);
    if (!probe.packageLayout) {
        return facts;
    }
    probe.marker = readSmallFile(records / "install-kind");
    if (environment) {
        if (const auto appImage = environment("APPIMAGE");
            appImage.has_value() && !appImage->empty() && !probe.marker.has_value()) {
            // Only when this program really runs from that file's mount: the variable is
            // inherited by every program an AppImage starts.
            const auto mount = environment("APPDIR");
            const auto mounted =
                mount.has_value() ? std::filesystem::weakly_canonical(*mount, error)
                                  : std::filesystem::path{};
            if (!mounted.empty() && std::filesystem::weakly_canonical(root, error) == mounted &&
                std::filesystem::is_regular_file(*appImage, error)) {
                probe.appImage = *appImage;
            }
        }
    }
    const auto replaced = probe.appImage.empty() ? root : probe.appImage;
    probe.rootWritable = canReplace(replaced);
    facts.kind = classifyInstall(probe);
    if (probe.marker.has_value()) {
        facts.manager = parseInstallMarker(*probe.marker).manager;
    }
    switch (facts.kind) {
    case InstallKind::MacBundle:
    case InstallKind::UserFolder:
    case InstallKind::AppImage:
    case InstallKind::WindowsInstaller:
    case InstallKind::WindowsPortable:
    case InstallKind::ReadOnlyFolder:
    case InstallKind::PackageManager:
        facts.root = replaced;
        break;
    case InstallKind::Development:
        break;
    }
    return facts;
}

std::filesystem::path currentExecutablePath() {
    std::error_code error;
#if defined(__linux__)
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path;
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size + 1U, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    auto canonical = std::filesystem::weakly_canonical(buffer, error);
    return error ? std::filesystem::path(buffer) : canonical;
#elif defined(_WIN32)
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const auto length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
#else
    (void)error;
    return {};
#endif
}

EnvironmentReader processEnvironment() {
    return [](const std::string_view name) -> std::optional<std::string> {
        const std::string key(name);
        // Read at start-up and from the interface thread only, as the environment is.
        const char* value = std::getenv(key.c_str()); // NOLINT(concurrency-mt-unsafe)
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    };
}

} // namespace bloom::update
