#include "footage_verification.hpp"

#include <bloom/core/worker_thread.hpp>
#include <bloom/media/image.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/platform/thread_priority.hpp>

#include <exception>
#include <set>
#include <thread>
#include <utility>

namespace bloom::ui {
namespace {
std::string assetName(const document::AssetRecord& asset) {
    if (!asset.name.empty())
        return asset.name;
    const auto name = std::filesystem::path(asset.locator.path).filename().u8string();
    return {name.begin(), name.end()};
}
} // namespace

std::vector<FootageVerificationItem> footageVerificationWork(const document::Project& project,
                                                             const std::filesystem::path& directory,
                                                             const std::function<bool()>& cancel) {
    std::vector<FootageVerificationItem> work;
    std::set<std::string> seen;
    // One observation per file; the memo already vouching for the current identity means there is
    // nothing to learn from reading it again.
    const auto add = [&](const document::AssetLocator& locator, FootageVerificationItem item,
                         const std::optional<document::AssetFileStamp>& stamp,
                         const bool verified) {
        item.path = media::resolveImagePath(locator.path, locator.relinkHint, directory);
        if (item.path.empty() || !seen.insert(item.path.lexically_normal().string()).second)
            return;
        const auto observed = platform::observeSource(item.path);
        if (!observed.identity || observed.digest)
            return;
        const bool carriesStamp =
            stamp && observed.identity->size == stamp->size &&
            observed.identity->modifiedNanoseconds == stamp->modifiedNanoseconds;
        // A verified member still carrying its stamp is proven; a pending one no longer carrying
        // it can never be recorded (the foreground reports it changed), so neither is read.
        if (stamp && (verified ? carriesStamp : !carriesStamp))
            return;
        work.push_back(std::move(item));
    };
    for (const auto& asset : project.assets()) {
        if (cancel && cancel())
            return work;
        if (asset.kind == document::AssetKind::Image) {
            add(asset.locator,
                {.path = {},
                 .asset = assetName(asset),
                 .frame = std::nullopt,
                 .recorded = asset.contentDigest},
                std::nullopt, false);
            continue;
        }
        if (asset.kind != document::AssetKind::Sequence)
            continue;
        std::size_t index = 0;
        for (const auto& member : asset.manifest.members) {
            if (cancel && (++index % 256) == 0 && cancel())
                return work;
            add(member.locator,
                {.path = {},
                 .asset = assetName(asset),
                 .frame = member.frame,
                 .recorded = member.contentDigest},
                member.stamp, member.contentDigest.has_value() && member.stamp.has_value());
        }
    }
    return work;
}

namespace {
void hashAll(const std::vector<FootageVerificationItem>& items, const std::function<bool()>& cancel,
             const FootageVerificationProgress& progress, FootageVerificationReport& report);
} // namespace

FootageVerificationReport verifyFootageFiles(const std::vector<FootageVerificationItem>& items,
                                             const std::function<bool()>& cancel,
                                             const FootageVerificationProgress& progress) {
    FootageVerificationReport report;
    report.files = items.size();
    // The pass runs on its own thread, which exits with it, so Linux can put it in SCHED_IDLE
    // (an unprivileged thread cannot leave that policy, which rules out the shared I/O worker).
    std::exception_ptr failure;
    bloom::core::WorkerThread([&] {
        const platform::IdleThreadPriorityScope idle(true);
        try {
            hashAll(items, cancel, progress, report);
        } catch (...) {
            failure = std::current_exception();
        }
    }).join();
    if (failure)
        std::rethrow_exception(failure);
    return report;
}

namespace {
void hashAll(const std::vector<FootageVerificationItem>& items, const std::function<bool()>& cancel,
             const FootageVerificationProgress& progress, FootageVerificationReport& report) {
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (cancel && cancel()) {
            report.cancelled = true;
            return;
        }
        if (progress)
            progress(index, items.size(), item.path);
        const auto hash = platform::hashSourceFile(item.path, cancel);
        if (hash.error == platform::SourceHashError::Cancelled) {
            report.cancelled = true;
            return;
        }
        if (hash.error != platform::SourceHashError::None)
            continue;
        ++report.hashed;
        if (!item.recorded || hash.digest == *item.recorded)
            continue;
        ++report.changed;
        const auto what =
            item.frame ? item.asset + " frame " + std::to_string(*item.frame) : item.asset;
        const auto path = item.path.u8string();
        report.diagnostics.push_back(
            {.code = "bloom.media.footage-changed",
             .severity = runtime::DiagnosticSeverity::Warning,
             .summary = what + " changed since it was imported",
             .detail = std::string(path.begin(), path.end()),
             .suggestedAction = "Relink or reload the footage in Assets."});
    }
    if (progress)
        progress(items.size(), items.size(), {});
}
} // namespace

} // namespace bloom::ui
