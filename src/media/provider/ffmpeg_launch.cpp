#include <bloom/media/provider/ffmpeg_launch.hpp>

#include <filesystem>
#include <string>
#include <system_error>

namespace bloom::media::provider {
void configureFfmpegWorkerEnvironment(platform::ProcessOptions& options,
                                      const std::string& executable) {
#if defined(__linux__)
    std::error_code error;
    const auto worker = std::filesystem::absolute(executable, error);
    if (!error)
        options.environment.push_back("LD_LIBRARY_PATH=" + (worker.parent_path() / "lib").string());
#elif defined(_WIN32)
    // The worker's environment starts empty, and without a temporary folder named in it the
    // system answers with its own folder, which an ordinary account cannot write. The worker
    // spools an encode there.
    static_cast<void>(executable);
    std::error_code error;
    const auto folder = std::filesystem::temp_directory_path(error);
    if (!error && !folder.empty()) {
        const auto bytes = folder.u8string();
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        options.environment.push_back("TEMP=" + text);
        options.environment.push_back("TMP=" + text);
    }
#else
    (void)options;
    (void)executable;
#endif
}
} // namespace bloom::media::provider
