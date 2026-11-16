#include <bloom/ui/media_disk_cache_settings.hpp>

#include <bloom/platform/cache_directory.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/application_preferences.hpp>

#include <QSettings>
#include <QString>
#include <QStringList>

namespace bloom::ui {

namespace {
constexpr auto kEnabledKey = "media/disk-cache-enabled";
constexpr auto kDirectoryKey = "media/disk-cache-directory";
constexpr auto kObsoleteBudgetBytesKey = "media/disk-cache-budget-bytes";
constexpr auto kHeadroomBytesKey = "media/disk-cache-headroom-bytes";
constexpr auto kLocalCopiesKey = "media/local-copies";
constexpr auto kLocalCopiesFoldersKey = "media/local-copies-folders";

[[nodiscard]] std::filesystem::path nativePathFromSettingsString(const QString& text) {
#if defined(_WIN32)
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(text.toStdString());
#endif
}
} // namespace

bool mediaDiskCacheEnabledFromSettings(const QSettings& settings) {
    return settings.value(QLatin1String(kEnabledKey), true).toBool();
}

std::filesystem::path mediaDiskCacheDirectoryFromSettings(const QSettings& settings) {
    const auto text = settings.value(QLatin1String(kDirectoryKey)).toString();
    if (text.isEmpty())
        return {};
    const auto path = nativePathFromSettingsString(text);
    return path.is_absolute() ? path : std::filesystem::path{};
}

std::uint64_t mediaDiskHeadroomFromSettings(const QSettings& settings) {
    bool parsed = false;
    const auto value =
        settings.value(QLatin1String(kHeadroomBytesKey)).toString().toULongLong(&parsed);
    if (!parsed || value == 0 || value > kMaximumMediaDiskHeadroomBytes)
        return 0;
    return value;
}

void dropObsoleteMediaDiskCacheSettings(QSettings& settings) {
    if (settings.contains(QLatin1String(kObsoleteBudgetBytesKey)))
        settings.remove(QLatin1String(kObsoleteBudgetBytesKey));
}

media::cache::LocalCopyPolicy localCopyPolicyFromSettings(const QSettings& settings) {
    media::cache::LocalCopyPolicy policy;
    const auto preference = settings.value(QLatin1String(kLocalCopiesKey)).toString();
    policy.preference =
        preference == QLatin1String("always")  ? media::cache::LocalCopyPreference::Always
        : preference == QLatin1String("never") ? media::cache::LocalCopyPreference::Never
                                               : media::cache::LocalCopyPreference::Automatic;
    const auto folders = settings.value(QLatin1String(kLocalCopiesFoldersKey)).toStringList();
    for (const auto& entry : folders) {
        const bool keep = entry.startsWith(QLatin1String("keep:"));
        if (!keep && !entry.startsWith(QLatin1String("skip:")))
            continue;
        const auto folder = nativePathFromSettingsString(entry.mid(5));
        if (folder.is_absolute())
            policy.folders.push_back({folder, keep});
    }
    return policy;
}

void writeLocalCopyPolicy(QSettings& settings, const media::cache::LocalCopyPolicy& policy) {
    switch (policy.preference) {
    case media::cache::LocalCopyPreference::Always:
        settings.setValue(QLatin1String(kLocalCopiesKey), QStringLiteral("always"));
        break;
    case media::cache::LocalCopyPreference::Never:
        settings.setValue(QLatin1String(kLocalCopiesKey), QStringLiteral("never"));
        break;
    case media::cache::LocalCopyPreference::Automatic:
        settings.remove(QLatin1String(kLocalCopiesKey));
        break;
    }
    QStringList folders;
    for (const auto& rule : policy.folders) {
#if defined(_WIN32)
        const auto text = QString::fromStdWString(rule.folder.wstring());
#else
        const auto text = QString::fromStdString(rule.folder.string());
#endif
        folders.push_back((rule.keep ? QStringLiteral("keep:") : QStringLiteral("skip:")) + text);
    }
    if (folders.isEmpty())
        settings.remove(QLatin1String(kLocalCopiesFoldersKey));
    else
        settings.setValue(QLatin1String(kLocalCopiesFoldersKey), folders);
}

media::cache::SourceMirrorUserSettings sourceMirrorSettingsFromSettings(const QSettings& settings) {
    media::cache::SourceMirrorUserSettings result;
    result.enabled = mediaDiskCacheEnabledFromSettings(settings);
    result.directory = mediaDiskCacheDirectoryFromSettings(settings);
    // #732: the mirror has no byte budget any more. It keeps a headroom free on the cache
    // volume instead, and an earlier build's budget value is not carried over.
    result.headroomBytes = mediaDiskHeadroomFromSettings(settings);
    result.policy = localCopyPolicyFromSettings(settings);
    return result;
}

std::shared_ptr<media::cache::SourceMirror>
makeMediaDiskCacheFromSettings(const QSettings& settings) {
    const auto mirrorSettings = sourceMirrorSettingsFromSettings(settings);
    // The headless tools read this copy; a platform without a cache folder publishes nothing.
    static_cast<void>(media::cache::publishSourceMirrorSettings(mirrorSettings));
    return media::cache::makeSourceMirror(mirrorSettings);
}

void applyMediaDiskCacheSettings(media::cache::SourceMirror* const cache,
                                 const QSettings& settings) {
    const auto mirrorSettings = sourceMirrorSettingsFromSettings(settings);
    if (cache != nullptr)
        cache->setHeadroom(mirrorSettings.headroomBytes);
    static_cast<void>(media::cache::publishSourceMirrorSettings(mirrorSettings));
}

bool purgeMediaCaches(media::cache::SourceMirror* const cache,
                      runtime::OperationCache* const operationCache) {
    bool cleared = false;
    if (cache != nullptr) {
        // Removes every local copy and resets the statistics. No source file is consulted.
        cache->clear();
        cleared = true;
    }
    if (operationCache != nullptr) {
        // The evaluator's in-memory decoded-still-image entries. Derived operation results are
        // deliberately outside "decoded media" here.
        operationCache->clearDecodedMedia();
        cleared = true;
    }
    return cleared;
}

} // namespace bloom::ui
