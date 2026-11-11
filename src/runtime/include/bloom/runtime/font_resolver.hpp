#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/core/worker_thread.hpp>
#include <bloom/document/font_id.hpp>
#include <bloom/runtime/font_fallback.hpp>
#include <bloom/runtime/font_fetch.hpp>
#include <bloom/runtime/font_transport.hpp>
#include <bloom/runtime/google_catalogue.hpp>
#include <bloom/runtime/system_font_index.hpp>
#include <bloom/runtime/variable_font_instance.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bloom::runtime::fonts {

enum class FontResolutionKind : std::uint8_t { System, Cache, Downloaded, Builtin, Fallback };

enum class FontMissingReason : std::uint8_t {
    None,
    // Not installed, and not a Google family.
    NotInstalled,
    // The family is not known to Google Fonts.
    UnknownFamily,
    Offline,
    DownloadFailed,
    // The font is on its way; the fallback is shown until it arrives.
    Downloading,
    // No network transport is available in this process, or the fetch mode forbade a download.
    DownloadUnavailable,
    Cancelled,
};

// The face a font ID resolves to, or the fallback standing in for it.
struct FontResolution final {
    FontResolutionKind kind = FontResolutionKind::Builtin;
    // The font file; null for a built-in face.
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    core::Sha256Digest digest;
    std::uint32_t faceIndex = 0;
    // The built-in face (0 DejaVu Sans Book, 1 Inter Regular, 2 Medium, 3 SemiBold) when bytes
    // is null.
    std::uint32_t builtinFace = 0;
    FontMissingReason reason = FontMissingReason::None;
    // What is actually drawn.
    std::string usedFamily;
    std::string usedStyle;
    // Google fonts: the family version of the file used.
    std::string familyVersion;
    // The cached or downloaded Google version differs from the one the project recorded.
    bool versionChanged = false;
    // A system-sourced ID that is not installed and was taken from Google instead.
    bool fetchedInsteadOfSystem = false;
    // For a fallback: one sentence naming the font and the face used instead.
    std::string diagnostic;

    [[nodiscard]] bool isFallback() const noexcept { return kind == FontResolutionKind::Fallback; }
    [[nodiscard]] bool pendingDownload() const noexcept {
        return isFallback() && reason == FontMissingReason::Downloading;
    }
    // A font that cannot be resolved (a download still on its way does not count).
    [[nodiscard]] bool missing() const noexcept { return isFallback() && !pendingDownload(); }
};

enum class FontNoticeKind : std::uint8_t {
    VersionChanged,
    FetchedInsteadOfSystem,
    DownloadStarted,
    DownloadFinished,
    DownloadFailed,
};

// A transient message for the status bar and the log; never project state.
struct FontNotice final {
    FontNoticeKind kind = FontNoticeKind::DownloadStarted;
    std::string family;
    // One sentence, American English.
    std::string message;
};

struct FontStatus final {
    enum class State : std::uint8_t { Checking, Available, Downloading, Missing };
    State state = State::Checking;
    FontMissingReason reason = FontMissingReason::None;
    // For a missing font, the same sentence the diagnostic carries.
    std::string detail;
};

struct FontDownloadState final {
    std::string family;
    std::string style;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
};

// Resolves font IDs to font files: an installed system font (PostScript name first, then family and
// style), the per-user Google Fonts cache, a download from Google Fonts, then the fallback table.
// One resolver serves a process; the cache is per user and shared by all projects.
//
// Thread-safe. Downloads run on one worker thread this class owns; nothing here is called on the UI
// thread except the quick, non-blocking status() and the accessors.
class FontResolver final {
  public:
    struct Options final {
        // The per-user cache root (the resolver adds "fonts"); empty keeps downloads in memory.
        std::filesystem::path cacheDirectory;
        std::shared_ptr<FontTransport> transport;
        // Where the stylesheet is read; tests point it elsewhere.
        std::string stylesheetEndpoint;
    };

    FontResolver();
    explicit FontResolver(Options options);
    ~FontResolver();
    FontResolver(const FontResolver&) = delete;
    FontResolver& operator=(const FontResolver&) = delete;

    [[nodiscard]] FontResolution resolve(const document::FontId& id, FontFetch fetch,
                                         const std::function<bool()>& cancelled = {});

    // A quick answer for the UI: never blocks and never reads a font file. `requestFetch` starts a
    // background download when the font is a Google family that is not cached yet.
    [[nodiscard]] FontStatus status(const document::FontId& id, bool requestFetch);

    // The name-only subset of a Google family the picker draws its row in: its bytes when cached,
    // otherwise null and (when `request`) a background fetch whose arrival bumps epoch().
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>>
    previewSubset(std::string_view family, bool request);

    // Starts fetching a Google face in the background (hover preview, a project that names it).
    void prefetch(const document::FontId& id);

    void setTransport(std::shared_ptr<FontTransport> transport);
    [[nodiscard]] bool hasTransport() const;
    void setNoticeSink(std::function<void(const FontNotice&)> sink);
    // Called on the worker thread whenever a font file arrives (not for failures); `preview` is true
    // for a picker row's name-only subset.
    void setArrivalSink(std::function<void(bool preview)> sink);

    // Counts arrivals of faces: a compiled plan built before the count changed may have used a
    // fallback. previewEpoch() counts the picker's name subsets, which no plan uses.
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_.load(); }
    [[nodiscard]] std::uint64_t previewEpoch() const noexcept { return previewEpoch_.load(); }
    [[nodiscard]] std::vector<FontDownloadState> downloads() const;
    // Cancels every queued and running download.
    void cancelDownloads();

    [[nodiscard]] SystemFontIndex& systemFonts() noexcept { return system_; }
    [[nodiscard]] const std::filesystem::path& cacheDirectory() const noexcept { return root_; }
    // The cache file of a Google face (it may not exist), for tests and diagnostics.
    [[nodiscard]] std::filesystem::path cachePathFor(const document::FontId& id) const;

  private:
    struct Job final {
        std::string key;
        bool preview = false;
        document::FontId id;
        enum class State : std::uint8_t { Queued, Running, Succeeded, Failed } state = State::Queued;
        FontMissingReason failure = FontMissingReason::None;
        std::string error;
        std::string version;
        std::uint64_t received = 0;
        std::uint64_t total = 0;
        std::atomic<bool> cancel{false};
        std::chrono::steady_clock::time_point finished{};
    };
    struct CachedFace final {
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        std::string version;
    };

    [[nodiscard]] std::optional<CachedFace> readCache(const std::filesystem::path& file) const;
    [[nodiscard]] std::filesystem::path faceFile(const document::FontId& id, bool preview) const;
    [[nodiscard]] std::string jobKey(const document::FontId& id, bool preview) const;
    // Queues (or finds) the job for a face; returns it. Caller holds no lock.
    [[nodiscard]] std::shared_ptr<Job> enqueue(const document::FontId& id, bool preview,
                                               bool retryFailed);
    void workerLoop();
    void runJob(Job& job);
    void notify(FontNotice notice);
    [[nodiscard]] FontResolution fallbackFor(const document::FontId& id, FontMissingReason reason,
                                             const std::function<bool()>& cancelled);
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>>
    loadFile(const std::filesystem::path& path) const;

    // An installed face's file ready to draw. A named instance of a variable font (the face index
    // carries it in its high 16 bits) and the font ID's variable-axis values are applied by writing
    // a static instance of the font; anything else is the file as it is.
    struct LoadedFace final {
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        core::Sha256Digest digest;
        std::uint32_t faceIndex = 0;
    };
    [[nodiscard]] std::optional<LoadedFace>
    loadSystemFace(const platform::FontFace& face, const std::vector<document::FontAxisValue>& axes);

    std::filesystem::path root_;
    std::string endpoint_;
    SystemFontIndex system_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::shared_ptr<FontTransport> transport_;
    std::function<void(const FontNotice&)> noticeSink_;
    std::function<void(bool)> arrivalSink_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::map<std::string, CachedFace> memory_;
    std::map<std::string, LoadedFace> instances_;
    std::vector<std::string> announced_;
    bloom::core::WorkerThread worker_;
    bool stopping_ = false;
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<std::uint64_t> previewEpoch_{0};
};

// The font resolver of this process. The desktop application, the command-line tools and tests
// share it so the per-user cache and its downloads are one thing; it has no network transport
// until the application installs one, and a test passes its own resolver through
// SnapshotCompileRequest instead.
[[nodiscard]] FontResolver& processFontResolver();

} // namespace bloom::runtime::fonts
