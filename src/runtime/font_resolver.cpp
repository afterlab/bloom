#include <bloom/core/worker_thread.hpp>
#include <bloom/runtime/font_resolver.hpp>

#include <bloom/platform/cache_directory.hpp>
#include <bloom/core/safe_parse.hpp>
#include <bloom/runtime/google_fonts.hpp>
#include <bloom/runtime/variable_font_instance.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

namespace bloom::runtime::fonts {
namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t kMaximumStylesheetBytes = 1U << 20U;
constexpr std::uint64_t kMaximumFontBytes = 32U << 20U;
constexpr auto kFailureBackoff = 30s;

struct BuiltinFace final {
    std::string_view family;
    std::string_view style;
    std::int32_t weight;
};
constexpr std::array<BuiltinFace, 4> kBuiltinFaces{{{"DejaVu Sans", "Book", 400},
                                                     {"Inter", "Regular", 400},
                                                     {"Inter", "Medium", 500},
                                                     {"Inter", "SemiBold", 600}}};

[[nodiscard]] std::string lower(const std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool sameName(const std::string_view a, const std::string_view b) {
    return lower(a) == lower(b);
}

[[nodiscard]] std::optional<std::uint32_t> builtinIndexFor(const document::FontId& id) {
    for (std::size_t index = 0; index < kBuiltinFaces.size(); ++index)
        if (sameName(kBuiltinFaces[index].family, id.family) &&
            (sameName(kBuiltinFaces[index].style, id.style) ||
             (id.style.empty() && kBuiltinFaces[index].weight == id.weight && !id.italic)))
            return static_cast<std::uint32_t>(index);
    return std::nullopt;
}

[[nodiscard]] std::string slug(const std::string_view text) {
    std::string out;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        out += std::isalnum(c) ? static_cast<char>(std::tolower(c)) : '-';
    }
    return out;
}

[[nodiscard]] std::string axisSuffix(const document::FontId& id) {
    std::string out;
    for (const auto& axis : id.axes) {
        out += "-" + axis.tag + core::formatScalar(axis.value, std::int32_t{3}, true, std::int32_t{0});
    }
    std::ranges::replace_if(out, [](const char c) { return c == '.' || c == ' '; }, '_');
    return out;
}

[[nodiscard]] bool looksLikeFont(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 12)
        return false;
    const std::string_view tag(reinterpret_cast<const char*>(bytes.data()), 4);
    return tag == std::string_view("\0\1\0\0", 4) || tag == "OTTO" || tag == "true" || tag == "ttcf";
}

[[nodiscard]] long versionNumber(const std::string_view version) {
    if (version.size() < 2 || version.front() != 'v')
        return -1;
    long value = 0;
    for (const char c : version.substr(1)) {
        if (c < '0' || c > '9')
            return -1;
        value = value * 10 + (c - '0');
    }
    return value;
}

[[nodiscard]] std::string quoteName(const std::string_view text) { return "\"" + std::string(text) + "\""; }

[[nodiscard]] std::string failureSentence(const FontMissingReason reason, const std::string& detail) {
    switch (reason) {
    case FontMissingReason::Offline:
        return "no network connection";
    case FontMissingReason::UnknownFamily:
        return "Google Fonts has no such family";
    case FontMissingReason::DownloadUnavailable:
        return "downloads are not available here";
    case FontMissingReason::Cancelled:
        return "the download was cancelled";
    default:
        return detail.empty() ? "the download failed" : detail;
    }
}

} // namespace

FontResolver::FontResolver() : FontResolver(Options{}) {}

FontResolver::FontResolver(Options options)
    : root_(options.cacheDirectory.empty() ? std::filesystem::path{}
                                           : options.cacheDirectory / "fonts"),
      endpoint_(options.stylesheetEndpoint.empty() ? std::string(kGoogleFontsStylesheetEndpoint)
                                                   : std::move(options.stylesheetEndpoint)),
      transport_(std::move(options.transport)) {}

FontResolver::~FontResolver() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& [key, job] : jobs_)
            job->cancel = true;
    }
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void FontResolver::setTransport(std::shared_ptr<FontTransport> transport) {
    std::lock_guard lock(mutex_);
    transport_ = std::move(transport);
}

bool FontResolver::hasTransport() const {
    std::lock_guard lock(mutex_);
    return transport_ != nullptr;
}

void FontResolver::setNoticeSink(std::function<void(const FontNotice&)> sink) {
    std::lock_guard lock(mutex_);
    noticeSink_ = std::move(sink);
}

void FontResolver::setArrivalSink(std::function<void(bool)> sink) {
    std::lock_guard lock(mutex_);
    arrivalSink_ = std::move(sink);
}

void FontResolver::notify(FontNotice notice) {
    std::function<void(const FontNotice&)> sink;
    {
        std::lock_guard lock(mutex_);
        sink = noticeSink_;
    }
    if (sink)
        sink(notice);
}

std::filesystem::path FontResolver::faceFile(const document::FontId& id, const bool preview) const {
    if (root_.empty())
        return {};
    const auto directory = root_ / "google" / slug(id.family);
    if (preview)
        return directory / "preview.ttf";
    return directory / (std::to_string(id.weight) + (id.italic ? "i" : "n") + axisSuffix(id) + ".ttf");
}

std::filesystem::path FontResolver::cachePathFor(const document::FontId& id) const {
    return faceFile(id, false);
}

std::string FontResolver::jobKey(const document::FontId& id, const bool preview) const {
    return slug(id.family) + (preview ? "|preview" : "|" + std::to_string(id.weight) +
                                                          (id.italic ? "i" : "n") + axisSuffix(id));
}

std::shared_ptr<const std::vector<std::uint8_t>>
FontResolver::loadFile(const std::filesystem::path& path) const {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumFontBytes)
        return nullptr;
    try {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(size));
        std::ifstream input(path, std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(bytes->data()),
                        static_cast<std::streamsize>(bytes->size())))
            return nullptr;
        return bytes;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

std::optional<FontResolver::CachedFace> FontResolver::readCache(const std::filesystem::path& file) const {
    if (file.empty())
        return std::nullopt;
    const auto bytes = loadFile(file);
    if (!bytes || !looksLikeFont(*bytes))
        return std::nullopt;
    CachedFace face;
    face.bytes = bytes;
    std::ifstream meta(file.string() + ".meta");
    std::string line;
    while (std::getline(meta, line))
        if (line.starts_with("version="))
            face.version = line.substr(8);
    return face;
}

std::shared_ptr<FontResolver::Job> FontResolver::enqueue(const document::FontId& id, const bool preview,
                                                         const bool retryFailed) {
    const auto key = jobKey(id, preview);
    std::shared_ptr<Job> job;
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
            return nullptr;
        auto found = jobs_.find(key);
        if (found != jobs_.end()) {
            const auto state = found->second->state;
            const bool settled = state == Job::State::Failed || state == Job::State::Succeeded ||
                                 found->second->cancel.load();
            const bool backedOff = state == Job::State::Failed && !retryFailed &&
                                   std::chrono::steady_clock::now() - found->second->finished <
                                       kFailureBackoff;
            if (!settled || backedOff)
                return found->second;
            jobs_.erase(found);
        }
        job = std::make_shared<Job>();
        job->key = key;
        job->preview = preview;
        job->id = id;
        jobs_[key] = job;
        queue_.push_back(job);
        if (!worker_.joinable())
            worker_ = bloom::core::WorkerThread([this] { workerLoop(); });
    }
    wake_.notify_all();
    return job;
}

void FontResolver::workerLoop() {
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
                return;
            job = queue_.front();
            queue_.pop_front();
            if (job->cancel) {
                job->state = Job::State::Failed;
                job->failure = FontMissingReason::Cancelled;
                job->finished = std::chrono::steady_clock::now();
                wake_.notify_all();
                continue;
            }
            job->state = Job::State::Running;
        }
        runJob(*job);
        wake_.notify_all();
    }
}

void FontResolver::runJob(Job& job) {
    std::shared_ptr<FontTransport> transport;
    {
        std::lock_guard lock(mutex_);
        transport = transport_;
    }
    const auto fail = [&](const FontMissingReason reason, std::string error) {
        std::lock_guard lock(mutex_);
        job.state = Job::State::Failed;
        job.failure = reason;
        job.error = std::move(error);
        job.finished = std::chrono::steady_clock::now();
    };
    const auto cancelled = [&] {
        std::lock_guard lock(mutex_);
        return job.cancel.load() || stopping_;
    };
    if (!transport) {
        fail(FontMissingReason::DownloadUnavailable, "no network transport");
        return;
    }
    const auto* indexed = googleCatalogue().find(job.id.family);
    const auto display = fontDisplayName(job.id);
    if (!job.preview)
        notify({FontNoticeKind::DownloadStarted, job.id.family,
                "Downloading " + quoteName(display) + " from Google Fonts"});
    const auto progress = [&](const std::uint64_t received, const std::uint64_t total) {
        std::lock_guard lock(mutex_);
        job.received = received;
        job.total = total;
    };
    const auto classify = [&](const FontHttpResult& result, const std::string& what) {
        switch (result.status) {
        case FontHttpStatus::Offline:
            fail(FontMissingReason::Offline, what + ": offline");
            break;
        case FontHttpStatus::Cancelled:
            fail(FontMissingReason::Cancelled, what + ": cancelled");
            break;
        case FontHttpStatus::HttpError:
            fail(result.httpStatus == 400 || result.httpStatus == 404
                     ? FontMissingReason::UnknownFamily
                     : FontMissingReason::DownloadFailed,
                 what + ": server answered " + std::to_string(result.httpStatus));
            break;
        default:
            fail(FontMissingReason::DownloadFailed,
                 what + (result.error.empty() ? "" : ": " + result.error));
            break;
        }
    };

    // The stylesheet names the file and its family version.
    auto url = googleStylesheetUrl(job.id, indexed, job.preview ? job.id.family : std::string_view{},
                                   endpoint_);
    auto sheet = transport->get(url, kMaximumStylesheetBytes, cancelled, {});
    if (sheet.status == FontHttpStatus::HttpError && indexed == nullptr) {
        // A family the baked index lacks: ask with the least specific request before giving up.
        url = googleFamilyStylesheetUrl(job.id.family, endpoint_);
        sheet = transport->get(url, kMaximumStylesheetBytes, cancelled, {});
    }
    if (sheet.status != FontHttpStatus::Ok) {
        classify(sheet, "stylesheet");
    } else {
        const auto faces =
            parseGoogleStylesheet(std::string_view(reinterpret_cast<const char*>(sheet.body.data()),
                                                   sheet.body.size()));
        const auto* face = pickGoogleStylesheetFace(faces, job.id.weight, job.id.italic);
        if (face == nullptr) {
            fail(FontMissingReason::UnknownFamily, "the stylesheet names no font file");
        } else {
            auto file = transport->get(face->url, kMaximumFontBytes, cancelled, progress);
            if (file.status != FontHttpStatus::Ok) {
                classify(file, "font file");
            } else if (!looksLikeFont(file.body)) {
                fail(FontMissingReason::DownloadFailed, "the reply is not a font file");
            } else {
                const auto path = faceFile(job.id, job.preview);
                auto stored = std::make_shared<std::vector<std::uint8_t>>(std::move(file.body));
                if (!path.empty()) {
                    std::error_code error;
                    std::filesystem::create_directories(path.parent_path(), error);
                    const auto temporary = std::filesystem::path(path.string() + ".part");
                    {
                        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                        output.write(reinterpret_cast<const char*>(stored->data()),
                                     static_cast<std::streamsize>(stored->size()));
                    }
                    std::filesystem::rename(temporary, path, error);
                    if (!error) {
                        std::ofstream meta(path.string() + ".meta", std::ios::trunc);
                        meta << "version=" << face->version << "\nfamily=" << job.id.family << "\n";
                    } else {
                        std::filesystem::remove(temporary, error);
                    }
                }
                {
                    std::lock_guard lock(mutex_);
                    memory_[job.key] = {stored, face->version};
                    job.version = face->version;
                    job.state = Job::State::Succeeded;
                    job.finished = std::chrono::steady_clock::now();
                }
                (job.preview ? previewEpoch_ : epoch_).fetch_add(1);
                std::function<void(bool)> arrived;
                {
                    std::lock_guard lock(mutex_);
                    arrived = arrivalSink_;
                }
                if (!job.preview)
                    notify({FontNoticeKind::DownloadFinished, job.id.family,
                            "Downloaded " + quoteName(display) + " from Google Fonts"});
                if (arrived)
                    arrived(job.preview);
                return;
            }
        }
    }
    FontMissingReason reason{};
    std::string error;
    {
        std::lock_guard lock(mutex_);
        reason = job.failure;
        error = job.error;
    }
    if (!job.preview && reason != FontMissingReason::Cancelled)
        notify({FontNoticeKind::DownloadFailed, job.id.family,
                quoteName(display) + " could not be downloaded: " + failureSentence(reason, error)});
}

std::optional<FontResolver::LoadedFace>
FontResolver::loadSystemFace(const platform::FontFace& face,
                             const std::vector<document::FontAxisValue>& axes) {
    // fontconfig and FreeType put a variable font's named instance (1-based) in the high 16 bits of
    // the face index and the face within a collection in the low 16.
    const auto instance = face.faceIndex >> 16U;
    const auto collectionIndex = face.faceIndex & 0xFFFFU;
    const bool needsInstance = instance != 0 || !axes.empty();
    std::string key;
    if (needsInstance) {
        key = face.path.string() + "|" + std::to_string(face.faceIndex);
        for (const auto& axis : axes)
            key += "|" + axis.tag + core::formatScalar(axis.value, std::int32_t{3}, true, std::int32_t{0});
        std::lock_guard lock(mutex_);
        if (const auto found = instances_.find(key); found != instances_.end())
            return found->second;
    }
    auto bytes = loadFile(face.path);
    if (!bytes)
        return std::nullopt;
    LoadedFace loaded;
    loaded.faceIndex = collectionIndex;
    if (needsInstance) {
        std::vector<VariableFontAxisSetting> settings;
        settings.reserve(axes.size());
        for (const auto& axis : axes)
            settings.push_back({axis.tag, axis.value});
        auto made = instantiateVariableFont(*bytes, instance, settings);
        if (made.status == VariableFontInstanceStatus::Instanced) {
            bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(made.bytes));
            loaded.faceIndex = 0;
        }
    }
    const auto digest = core::Sha256Hasher::hash(std::as_bytes(std::span(*bytes)));
    if (!digest)
        return std::nullopt;
    loaded.bytes = std::move(bytes);
    loaded.digest = *digest;
    if (needsInstance) {
        std::lock_guard lock(mutex_);
        // A handful of instances is the working set (one per styled text layer); the cap only stops
        // a long session that sweeps an axis from growing without bound.
        if (instances_.size() >= 32)
            instances_.clear();
        instances_[key] = loaded;
    }
    return loaded;
}

FontResolution FontResolver::fallbackFor(const document::FontId& id, const FontMissingReason reason,
                                         const std::function<bool()>& cancelled) {
    FontResolution result;
    result.kind = FontResolutionKind::Fallback;
    result.reason = reason;
    const auto category = categoryForMissingFont(id);
    const auto snapshot = system_.snapshotBlocking(cancelled);
    if (snapshot)
        for (const auto family : fallbackFamilies(category)) {
            const auto* face = closestSystemFace(*snapshot, family, id.weight, id.italic);
            if (face == nullptr)
                continue;
            auto loaded = loadSystemFace(*face, {});
            if (!loaded)
                continue;
            result.bytes = std::move(loaded->bytes);
            result.digest = loaded->digest;
            result.faceIndex = loaded->faceIndex;
            result.usedFamily = face->family;
            result.usedStyle = face->style;
            break;
        }
    if (!result.bytes) {
        result.builtinFace = 0;
        result.usedFamily = std::string(kBuiltinFaces[0].family);
        result.usedStyle = std::string(kBuiltinFaces[0].style);
    }
    const auto used = result.usedFamily + (result.usedStyle.empty() ? "" : " " + result.usedStyle);
    std::string why;
    switch (reason) {
    case FontMissingReason::Downloading:
        result.diagnostic = "Font " + quoteName(fontDisplayName(id)) + " is downloading; " + used +
                            " is shown until it arrives.";
        return result;
    case FontMissingReason::Offline:
        why = " could not be downloaded (no network connection)";
        break;
    case FontMissingReason::UnknownFamily:
        why = " is not installed and Google Fonts has no such family";
        break;
    case FontMissingReason::DownloadFailed:
        why = " could not be downloaded";
        break;
    case FontMissingReason::DownloadUnavailable:
        why = " is not installed and cannot be downloaded here";
        break;
    case FontMissingReason::Cancelled:
        why = " was not downloaded (cancelled)";
        break;
    default:
        why = " is not installed";
        break;
    }
    result.diagnostic = "Font " + quoteName(fontDisplayName(id)) + why + "; " + used + " is used instead.";
    return result;
}

FontResolution FontResolver::resolve(const document::FontId& id, const FontFetch fetch,
                                     const std::function<bool()>& cancelled) {
    // 1. A built-in face.
    if (id.source == document::FontSource::Builtin)
        if (const auto index = builtinIndexFor(id)) {
            FontResolution result;
            result.kind = FontResolutionKind::Builtin;
            result.builtinFace = *index;
            result.usedFamily = std::string(kBuiltinFaces[*index].family);
            result.usedStyle = std::string(kBuiltinFaces[*index].style);
            return result;
        }

    // 2. An installed system font.
    const auto snapshot = system_.snapshotBlocking(cancelled);
    if (snapshot)
        if (const auto* face = matchSystemFace(*snapshot, id)) {
            if (auto loaded = loadSystemFace(*face, id.axes)) {
                FontResolution result;
                result.kind = FontResolutionKind::System;
                result.bytes = std::move(loaded->bytes);
                result.digest = loaded->digest;
                result.faceIndex = loaded->faceIndex;
                result.usedFamily = face->family;
                result.usedStyle = face->style;
                return result;
            }
        }
    if (cancelled && cancelled())
        return fallbackFor(id, FontMissingReason::Cancelled, {});

    // 3. The per-user cache, then a download from Google Fonts.
    const auto* indexed = googleCatalogue().find(id.family);
    const bool googleFamily = id.source == document::FontSource::Google || indexed != nullptr;
    if (!googleFamily)
        return fallbackFor(id, FontMissingReason::NotInstalled, cancelled);

    const auto file = faceFile(id, false);
    const auto key = jobKey(id, false);
    std::optional<CachedFace> cached;
    {
        std::lock_guard lock(mutex_);
        if (const auto found = memory_.find(key); found != memory_.end())
            cached = found->second;
    }
    if (!cached)
        cached = readCache(file);
    // A cached file older than the family version this build's index knows is refreshed when the
    // network allows; the older file is kept as the answer if it does not.
    const bool stale = cached && indexed != nullptr && versionNumber(cached->version) >= 0 &&
                       versionNumber(indexed->version) > versionNumber(cached->version);
    if (!cached || (stale && fetch != FontFetch::CachedOnly)) {
        FontMissingReason reason = FontMissingReason::DownloadUnavailable;
        if (fetch != FontFetch::CachedOnly && hasTransport()) {
            const auto job = enqueue(id, false, fetch == FontFetch::Wait);
            if (job && fetch == FontFetch::Background) {
                std::lock_guard lock(mutex_);
                reason = job->state == Job::State::Failed ? job->failure
                                                          : FontMissingReason::Downloading;
            } else if (job) {
                std::unique_lock lock(mutex_);
                while (job->state == Job::State::Queued || job->state == Job::State::Running) {
                    if (cancelled && cancelled()) {
                        job->cancel = true;
                        break;
                    }
                    wake_.wait_for(lock, 50ms);
                }
                reason = job->state == Job::State::Succeeded
                             ? FontMissingReason::None
                             : (job->cancel ? FontMissingReason::Cancelled : job->failure);
            }
        }
        if (reason == FontMissingReason::None) {
            std::lock_guard lock(mutex_);
            if (const auto found = memory_.find(key); found != memory_.end())
                cached = found->second;
        } else if (!cached) {
            return fallbackFor(id, reason, cancelled);
        }
    }
    FontResolution result;
    result.kind = FontResolutionKind::Cache;
    result.bytes = cached->bytes;
    const auto digest = core::Sha256Hasher::hash(std::as_bytes(std::span(*cached->bytes)));
    if (!digest)
        return fallbackFor(id, FontMissingReason::DownloadFailed, cancelled);
    result.digest = *digest;
    result.usedFamily = id.family;
    result.usedStyle = id.style;
    result.familyVersion = cached->version;
    result.fetchedInsteadOfSystem = id.source == document::FontSource::System;
    result.versionChanged = id.source == document::FontSource::Google && !id.familyVersion.empty() &&
                            !cached->version.empty() && id.familyVersion != cached->version;
    const auto announce = [&](const FontNoticeKind kind, const std::string& message) {
        const auto note = key + "|" + std::to_string(static_cast<int>(kind)) + "|" + cached->version;
        {
            std::lock_guard lock(mutex_);
            if (std::ranges::find(announced_, note) != announced_.end())
                return;
            announced_.push_back(note);
        }
        notify({kind, id.family, message});
    };
    if (result.versionChanged)
        announce(FontNoticeKind::VersionChanged,
                 "Font version changed: " + quoteName(id.family) + " is now " + cached->version +
                     " (the project used " + id.familyVersion + ")");
    if (result.fetchedInsteadOfSystem)
        announce(FontNoticeKind::FetchedInsteadOfSystem,
                 quoteName(fontDisplayName(id)) + " is not installed; using the copy from Google Fonts");
    return result;
}

FontStatus FontResolver::status(const document::FontId& id, const bool requestFetch) {
    FontStatus status;
    (void)system_.poll();
    if (id.source == document::FontSource::Builtin && builtinIndexFor(id)) {
        status.state = FontStatus::State::Available;
        return status;
    }
    const auto snapshot = system_.snapshot();
    if (!snapshot) {
        system_.refresh();
        status.state = FontStatus::State::Checking;
        return status;
    }
    if (matchSystemFace(*snapshot, id) != nullptr) {
        status.state = FontStatus::State::Available;
        return status;
    }
    const auto* indexed = googleCatalogue().find(id.family);
    if (id.source != document::FontSource::Google && indexed == nullptr) {
        status.state = FontStatus::State::Missing;
        status.reason = FontMissingReason::NotInstalled;
        status.detail = quoteName(fontDisplayName(id)) + " is not installed.";
        return status;
    }
    const auto key = jobKey(id, false);
    {
        std::lock_guard lock(mutex_);
        if (memory_.contains(key)) {
            status.state = FontStatus::State::Available;
            return status;
        }
    }
    std::error_code error;
    if (const auto file = faceFile(id, false); !file.empty() && std::filesystem::exists(file, error)) {
        status.state = FontStatus::State::Available;
        return status;
    }
    {
        std::lock_guard lock(mutex_);
        if (const auto found = jobs_.find(key); found != jobs_.end()) {
            const auto& job = *found->second;
            if (job.state == Job::State::Queued || job.state == Job::State::Running) {
                status.state = FontStatus::State::Downloading;
                return status;
            }
            if (job.state == Job::State::Failed &&
                std::chrono::steady_clock::now() - job.finished < kFailureBackoff) {
                status.state = FontStatus::State::Missing;
                status.reason = job.failure;
                status.detail = quoteName(fontDisplayName(id)) + " could not be downloaded: " +
                                failureSentence(job.failure, job.error) + ".";
                return status;
            }
        }
        if (!transport_) {
            status.state = FontStatus::State::Missing;
            status.reason = FontMissingReason::DownloadUnavailable;
            status.detail = quoteName(fontDisplayName(id)) + " is not installed and cannot be downloaded here.";
            return status;
        }
    }
    if (requestFetch)
        (void)enqueue(id, false, false);
    status.state = requestFetch ? FontStatus::State::Downloading : FontStatus::State::Checking;
    return status;
}

std::shared_ptr<const std::vector<std::uint8_t>> FontResolver::previewSubset(const std::string_view family,
                                                                             const bool request) {
    document::FontId id;
    id.source = document::FontSource::Google;
    id.family = std::string(family);
    const auto key = jobKey(id, true);
    {
        std::lock_guard lock(mutex_);
        if (const auto found = memory_.find(key); found != memory_.end())
            return found->second.bytes;
    }
    if (auto cached = readCache(faceFile(id, true))) {
        std::lock_guard lock(mutex_);
        memory_[key] = *cached;
        return cached->bytes;
    }
    if (request && hasTransport())
        (void)enqueue(id, true, false);
    return nullptr;
}

void FontResolver::prefetch(const document::FontId& id) {
    if (hasTransport())
        (void)enqueue(id, false, false);
}

std::vector<FontDownloadState> FontResolver::downloads() const {
    std::vector<FontDownloadState> states;
    std::lock_guard lock(mutex_);
    for (const auto& [key, job] : jobs_)
        if (!job->preview &&
            (job->state == Job::State::Queued || job->state == Job::State::Running))
            states.push_back({job->id.family, job->id.style, job->received, job->total});
    return states;
}

void FontResolver::cancelDownloads() {
    std::lock_guard lock(mutex_);
    for (auto& [key, job] : jobs_)
        job->cancel = true;
    wake_.notify_all();
}

FontResolver& processFontResolver() {
    static FontResolver resolver([] {
        FontResolver::Options options;
        if (const auto root = platform::userCacheDirectory())
            options.cacheDirectory = *root;
        return options;
    }());
    return resolver;
}

} // namespace bloom::runtime::fonts
