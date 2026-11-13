// The status bar's text and colour rules, kept free of widgets so tests and the segments read the
// same words (docs/ux/ui-grammar.md, "Status bar").
#include <bloom/ui/window_status_bar.hpp>

#include "window_status_bar_segments.hpp"

#include <bloom/media/cache/source_mirror.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <cmath>

namespace bloom::ui {

using statusbar::formatBytes;

PreviewColorState previewColorState(const CompositionPreviewState& preview) {
    if (preview.activity == PreviewActivity::Failed &&
        (!preview.viewerDisplayState || !preview.frame)) {
        return {preview.message.isEmpty() ? WindowStatusBar::tr("Color state unavailable")
                                          : preview.message,
                kit::Color::Error, WindowStatusBar::tr("None")};
    }
    const auto bufferView =
        preview.frame != nullptr ? preview.frame->displayBufferView() : std::nullopt;
    if (preview.frame != nullptr) {
        const auto provider = preview.frame->provenance().provider;
        // The resident arm keeps no CPU display buffer by construction (its pixels never crossed
        // the host boundary), so `bufferView` can never classify it and the reference fallback
        // below would falsely report the CPU "Reference (unqualified)" state. Read the immutable
        // provenance instead. A valid admission lease or an exact known owner-applied descriptor
        // identifies GPU pixels. Lease invalidation alone does not erase that applied metadata;
        // uncorrelated/blank displays remain unavailable and can never become CPU reference.
        if (provider == runtime::PreviewDisplayProvider::GpuResident) {
            const auto& resident = preview.frame->residentFrame();
            if (preview.ownerAppliedDisplay ||
                (resident != nullptr && resident->isDisplayValid())) {
                return {WindowStatusBar::tr("GPU resident"), kit::Color::Ok,
                        WindowStatusBar::tr("GPU")};
            }
            return {WindowStatusBar::tr("Color state unavailable"), kit::Color::Warn,
                    WindowStatusBar::tr("None")};
        }
        // GPU provenance is reported honestly on the existing color chip: a display-only frame the
        // fixed GPU Neutral operation produced says so, and the qualification report it carries is
        // PreviewOnly (RGB tolerance), so this never claims reference parity. Only its display
        // transform ran on the GPU: the scene was evaluated by the CPU stage, so the render path
        // follows the frame's evaluation provider and reads CPU (#452). CPU reference and
        // qualified-CPU frames keep their exact existing text.
        if (provider == runtime::PreviewDisplayProvider::GpuNeutral) {
            const bool gpuEvaluated = preview.frame->processIdentity().provider ==
                                      runtime::EvaluationProvider::GpuResident;
            return gpuEvaluated ? PreviewColorState{WindowStatusBar::tr("GPU display"),
                                                    kit::Color::Ok, WindowStatusBar::tr("GPU")}
                                : PreviewColorState{WindowStatusBar::tr("CPU render · GPU display"),
                                                    kit::Color::Ok, WindowStatusBar::tr("CPU")};
        }
        // The scene and its display transform both ran on the GPU; the finished image is then
        // handed to the window for painting, which is how every widget reaches the screen and
        // says nothing about how the frame was rendered. The words an artist reads are "GPU
        // render", with no "CPU" in them (#681); the architecture documents keep the precise
        // name of the route.
        if (provider == runtime::PreviewDisplayProvider::GpuPreparedPacked) {
            return {WindowStatusBar::tr("GPU render"), kit::Color::Ok, WindowStatusBar::tr("GPU")};
        }
    }
    if (bufferView.has_value() && bufferView->isOcioQualified) {
        return {WindowStatusBar::tr("Qualified · Bloom Neutral"), kit::Color::Ok,
                WindowStatusBar::tr("CPU")};
    }
    return {preview.frame ? WindowStatusBar::tr("Reference (unqualified)")
                          : WindowStatusBar::tr("No displayed frame"),
            kit::Color::Warn,
            preview.frame ? WindowStatusBar::tr("CPU") : WindowStatusBar::tr("None")};
}

QString previewActivityText(const CompositionPreviewState& preview) {
    if (preview.viewerDisplayState && !preview.viewerStatusText.isEmpty())
        return preview.viewerStatusText;
    if (preview.viewerDisplayState && !preview.message.isEmpty())
        return preview.message;
    if (!preview.message.isEmpty() && (preview.activity == PreviewActivity::Failed ||
                                       preview.freshness != FrameFreshness::Current))
        return preview.message;
    const bool stale = preview.freshness == FrameFreshness::Stale;
    switch (preview.activity) {
    case PreviewActivity::Rendering:
        return stale ? WindowStatusBar::tr("Rendering current frame · Previous frame shown")
                     : WindowStatusBar::tr("Rendering current frame");
    case PreviewActivity::Ready:
        if (!preview.diagnostics.empty() && !preview.diagnostics.front().summary.empty())
            return QString::fromStdString(preview.diagnostics.front().summary);
        return WindowStatusBar::tr("Current frame ready");
    case PreviewActivity::Unsupported:
        return stale ? WindowStatusBar::tr("Preview unsupported · Previous frame shown")
                     : WindowStatusBar::tr("Preview unsupported");
    case PreviewActivity::Cancelled:
        return stale ? WindowStatusBar::tr("Preview cancelled · Previous frame shown")
                     : WindowStatusBar::tr("Preview cancelled");
    case PreviewActivity::Failed:
        return stale ? WindowStatusBar::tr("Preview failed · Previous frame shown")
                     : WindowStatusBar::tr("Preview failed");
    }
    return WindowStatusBar::tr("Preview unavailable");
}

kit::Color previewActivityColor(const CompositionPreviewState& preview) {
    switch (preview.activity) {
    case PreviewActivity::Rendering:
        return kit::Color::Accent;
    case PreviewActivity::Ready:
        return kit::Color::Ok;
    case PreviewActivity::Unsupported:
        return kit::Color::Warn;
    case PreviewActivity::Cancelled:
        return kit::Color::Muted;
    case PreviewActivity::Failed:
        return kit::Color::Error;
    }
    return kit::Color::Muted;
}

QString droppedFrameText(const CompositionPreviewController& previewController) {
    if (!previewController.isCountingDroppedFrames()) {
        return {};
    }
    return WindowStatusBar::tr("%1 dropped").arg(previewController.droppedFrameCount());
}

QString previewCacheText(const CompositionPreviewController& previewController) {
    const auto& progress = previewController.ramPreviewProgress();
    if (progress.has_value()) {
        return WindowStatusBar::tr("Caching %1/%2")
            .arg(progress->cachedFrames)
            .arg(progress->totalFrames);
    }
    // No run is caching. What is left to report is what the cache actually holds -- which is also
    // the only honest account of BACKGROUND cache fill available: BackgroundPreviewController
    // publishes no progress signal, so a "filling in the background" claim would be invented.
    const auto& cache = previewController.frameCache();
    if (cache.size() == 0) {
        return {};
    }
    return WindowStatusBar::tr("Cache %1 frames · %2 / %3")
        .arg(cache.size())
        .arg(formatBytes(cache.residentBytes()), formatBytes(cache.byteBudget()));
}

QString operationCacheText(const runtime::OperationCache& operationCache) {
    const auto statistics = operationCache.statistics();
    if (statistics.hits == 0 && statistics.misses == 0 && operationCache.retainedBytes() == 0)
        return {};
    // #718: the decoded source media (source revisions, decoded samples and channels) is the RAM
    // tier in front of the disk cache; it shares this cache's budget, shown beside it.
    return WindowStatusBar::tr("Operation cache: %1 hits · %2 misses · %3 / %4 · Source media %5")
        .arg(statistics.hits)
        .arg(statistics.misses)
        .arg(formatBytes(operationCache.retainedBytes()), formatBytes(operationCache.byteBudget()),
             formatBytes(
                 operationCache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia)));
}

// #718 (docs/architecture/media-io.md "Disk cache"): the disk cell reports the state of the
// local copies of source media, which is true whether or not a lookup has happened: off, still
// reading its index, empty, or how much it holds. It is its own cell beside the RAM preview
// account because the two are independent budgets an artist reasons about separately.
QString mediaDiskCacheStatusText(const media::cache::SourceMirrorStatistics& stats,
                                 const bool enabled) {
    if (!enabled)
        return WindowStatusBar::tr("Disk cache off");
    if (!stats.indexLoaded)
        return WindowStatusBar::tr("Disk cache starting");
    if (stats.entryCount == 0)
        return WindowStatusBar::tr("Disk empty");
    return WindowStatusBar::tr("Disk %1 cached").arg(formatBytes(stats.storedBytes));
}

QString mediaDiskCacheTierText(const media::cache::SourceMirrorStatistics& stats,
                               const std::uint64_t memoryHits) {
    auto text = WindowStatusBar::tr("RAM hits %1 · Disk hits %2 · Network reads %3")
                    .arg(memoryHits)
                    .arg(stats.diskHits)
                    .arg(stats.networkReads);
    if (stats.otherSourceReads != 0)
        text += WindowStatusBar::tr(" · Other slow reads %1").arg(stats.otherSourceReads);
    return text;
}

} // namespace bloom::ui
