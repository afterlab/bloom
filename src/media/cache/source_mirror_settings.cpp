#include <bloom/media/cache/source_mirror_settings.hpp>

#include <bloom/platform/cache_directory.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace bloom::media::cache {

namespace {
constexpr std::string_view kHeader = "bloom-source-mirror-settings 1";

[[nodiscard]] std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
[[nodiscard]] std::filesystem::path fromUtf8(const std::string_view text) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
// A path is the rest of its line: only a line break cannot be written, so such a path is left
// out rather than corrupting the lines after it.
[[nodiscard]] bool writable(const std::string& text) {
    return !text.empty() && text.find_first_of("\r\n") == std::string::npos;
}
} // namespace

std::optional<std::filesystem::path> defaultSourceMirrorDirectory() {
    if (const auto root = platform::userCacheDirectory("bloom"))
        return *root / "media";
    return std::nullopt;
}

bool writeSourceMirrorSettings(const std::filesystem::path& file,
                               const SourceMirrorUserSettings& settings) {
    std::ostringstream text;
    text << kHeader << '\n';
    text << "enabled " << (settings.enabled ? 1 : 0) << '\n';
    text << "budget " << settings.byteBudget << '\n';
    if (const auto directory = utf8(settings.directory);
        settings.directory.is_absolute() && writable(directory))
        text << "directory " << directory << '\n';
    text << "copies "
         << (settings.policy.preference == LocalCopyPreference::Always  ? "always"
             : settings.policy.preference == LocalCopyPreference::Never ? "never"
                                                                        : "automatic")
         << '\n';
    for (const auto& rule : settings.policy.folders)
        if (const auto folder = utf8(rule.folder); rule.folder.is_absolute() && writable(folder))
            text << (rule.keep ? "keep " : "skip ") << folder << '\n';
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    static std::atomic<std::uint64_t> counter{0};
    const auto temporary =
        file.parent_path() /
        (file.filename().string() + ".tmp-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
         std::to_string(counter.fetch_add(1)));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const auto data = text.str();
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

SourceMirrorUserSettings readSourceMirrorSettings(const std::filesystem::path& file) {
    SourceMirrorUserSettings settings;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line != kHeader)
        return settings;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto space = line.find(' ');
        if (space == std::string::npos)
            continue;
        const std::string_view key(line.data(), space);
        const std::string_view value(line.data() + space + 1, line.size() - space - 1);
        if (key == "enabled") {
            settings.enabled = value != "0";
        } else if (key == "budget") {
            std::uint64_t bytes = 0;
            bool digits = !value.empty();
            for (const auto character : value) {
                if (character < '0' || character > '9' || bytes > (UINT64_MAX - 9) / 10) {
                    digits = false;
                    break;
                }
                bytes = bytes * 10 + static_cast<std::uint64_t>(character - '0');
            }
            if (digits)
                settings.byteBudget = bytes;
        } else if (key == "directory") {
            if (auto directory = fromUtf8(value); directory.is_absolute())
                settings.directory = std::move(directory);
        } else if (key == "copies") {
            settings.policy.preference = value == "always"  ? LocalCopyPreference::Always
                                         : value == "never" ? LocalCopyPreference::Never
                                                            : LocalCopyPreference::Automatic;
        } else if (key == "keep" || key == "skip") {
            if (auto folder = fromUtf8(value); folder.is_absolute())
                settings.policy.folders.push_back({std::move(folder), key == "keep"});
        }
    }
    return settings;
}

bool publishSourceMirrorSettings(const SourceMirrorUserSettings& settings) {
    const auto directory = defaultSourceMirrorDirectory();
    return directory && writeSourceMirrorSettings(*directory / "settings.v1", settings);
}

SourceMirrorUserSettings publishedSourceMirrorSettings() {
    const auto directory = defaultSourceMirrorDirectory();
    return directory ? readSourceMirrorSettings(*directory / "settings.v1")
                     : SourceMirrorUserSettings{};
}

std::shared_ptr<SourceMirror> makeSourceMirror(const SourceMirrorUserSettings& settings) {
    if (!settings.enabled)
        return nullptr;
    auto directory = settings.directory;
    if (directory.empty() || !directory.is_absolute()) {
        const auto fallback = defaultSourceMirrorDirectory();
        if (!fallback)
            return nullptr;
        directory = *fallback;
    }
    SourceMirrorConfig config;
    config.rootDirectory = directory;
    config.byteBudget =
        settings.byteBudget != 0 ? settings.byteBudget : defaultSourceMirrorByteBudget(directory);
    config.policy = settings.policy;
    return std::make_shared<SourceMirror>(std::move(config));
}

std::shared_ptr<SourceMirror> openUserSourceMirror() {
    // One store per process: every evaluator and export queue of a tool shares its index and
    // its single writer.
    static const std::shared_ptr<SourceMirror> mirror =
        makeSourceMirror(publishedSourceMirrorSettings());
    // It is the process's store from here on: every evaluator made afterwards reads through it.
    // The process store lets go of it before `mirror` above does, so the mirror is released
    // (and saves its index) where it always was: when that static goes.
    struct Published final {
        explicit Published(const std::shared_ptr<SourceMirror>& store) {
            platform::setProcessSourceStore(store);
        }
        Published(const Published&) = delete;
        Published& operator=(const Published&) = delete;
        ~Published() { platform::setProcessSourceStore(nullptr); }
    };
    static const Published published(mirror);
    return mirror;
}

} // namespace bloom::media::cache
