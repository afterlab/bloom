#pragma once

#include <bloom/runtime/audio_sound.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_editor_probe.hpp>

#include <bloom/ui/compile_failure.hpp>

#include <QElapsedTimer>
#include <QPointer>
#include <QString>

#include <optional>
#include <vector>

#include <QStringList>

#include <bloom/ui/kit/surfaces.hpp>
#include <cstddef>
#include <cstdint>

class QLabel;
class QResizeEvent;
class QTimer;
class QToolButton;

namespace bloom::media::cache {
class SourceMirror;
struct SourceMirrorStatistics;
} // namespace bloom::media::cache

namespace bloom::ui::kit {
class KButton;
class KLabel;
class KProgressBar;
class KPopupWindow;
} // namespace bloom::ui::kit

namespace bloom::ui::statusbar {
class Segment;
class ValueLabel;
class ClickableValue;
} // namespace bloom::ui::statusbar

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;
class McpServerAdapter;
class ViewerEditor;
struct CompositionPreviewState;

// The application's one persistent reporting surface (task VIEW-1). Anything the artist needs to
// know that is NOT a property of the frame they are looking at belongs here rather than painted
// over the canvas: the colour-management state, how ready the preview is, how many frames playback
// dropped, what the cache holds, a transient notice, and the version of Bloom saying all of it.
//
// Not a QStatusBar. QMainWindow's status bar brings its own chrome, its own size grip, and its own
// item model, none of which the Afterlab language wants; this is a kit strip laid out under the
// workspace with the same Surface/hairline treatment every other Bloom bar has.

// The colour-state chip's text and palette role, resolved from the CURRENTLY DISPLAYED frame's own
// isOcioQualified() bit -- never from the request activity -- so a retained qualified frame keeps
// reading as qualified while a later, unrelated request is in flight. That is the "never a silent
// relabel" half of the contract in docs/architecture/color-management.md; the other half is that
// this chip is always on screen, which is what moving it from the viewer footer to the window
// status bar guarantees regardless of which panels an artist has open.
//
// A failed replacement reports an error separately while preserving a retained frame's provenance.
struct PreviewColorState final {
    QString text;
    kit::Color colorToken = kit::Color::Warn;
    // The plain-word render path the Render value shows: "GPU", "CPU" or "None".
    QString pathLabel;
};

[[nodiscard]] PreviewColorState previewColorState(const CompositionPreviewState& preview);

// "Current frame ready", "Rendering current frame · Previous frame shown", and so on: the readiness
// sentence the viewer canvas used to paint as a chip over the pixels.
[[nodiscard]] QString previewActivityText(const CompositionPreviewState& preview);
[[nodiscard]] kit::Color previewActivityColor(const CompositionPreviewState& preview);

// #630, #636: background work the activity cell reports when nothing more urgent is shown: a sound
// being rendered and measured for the audio analysis nodes, or a bake reading its frames. `text` is
// empty when neither is running; `total` 0 is work whose size is not known yet.
struct BackgroundWorkReadout final {
    QString text;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    friend bool operator==(const BackgroundWorkReadout&, const BackgroundWorkReadout&) = default;
};
[[nodiscard]] BackgroundWorkReadout backgroundWorkReadout(const runtime::AudioSoundActivity& sound,
                                                          std::uint64_t bakeFramesDone,
                                                          std::uint64_t bakeFramesTotal);

// The dropped-frame count, EMPTY unless a playback run is counting: outside a run this reports
// nothing rather than a stale or invented figure. Zero dropped frames during a run still reads
// "0 dropped", because silence would mean "not measured", which is a different statement.
[[nodiscard]] QString droppedFrameText(const CompositionPreviewController& previewController);

// What the preview cache is doing: a RAM preview run's own progress while one is caching, and
// otherwise how much the cache holds -- which is the only measured account of background cache fill
// that exists, since BackgroundPreviewController publishes no progress of its own. Empty when the
// cache is empty and nothing is caching.
[[nodiscard]] QString previewCacheText(const CompositionPreviewController& previewController);
[[nodiscard]] QString operationCacheText(const runtime::OperationCache& operationCache);

// The disk cache's own cell (#718, docs/architecture/media-io.md "Disk cache"), separate from
// `previewCacheText()`'s in-memory RAM preview account above. It states what is true now, with or
// without a lookup: "Disk cache off" when disabled, "Disk cache starting" until the index has
// been read (off the UI thread), "Disk empty", or "Disk 4.2 GB cached".
[[nodiscard]] QString mediaDiskCacheStatusText(const media::cache::SourceMirrorStatistics& stats,
                                               bool enabled);
// The tooltip's tier line: "RAM hits 12 · Disk hits 340 · Network reads 8". RAM hits are source
// revisions reused from memory, disk hits are sources served by their local copy, and network
// reads are whole reads of a source on a network volume.
[[nodiscard]] QString mediaDiskCacheTierText(const media::cache::SourceMirrorStatistics& stats,
                                             std::uint64_t memoryHits);

class WindowStatusBar final : public kit::KSurface {
    Q_OBJECT

  public:
    // `previewController` may be null (a window built without a preview pipeline, as several tests
    // do): the preview-derived cells then stay empty rather than claiming anything.
    // `mediaDiskCache` may also be null (disk cache disabled/unavailable for this session): the
    // disk-cache cell then reports "Disk cache off" instead of polling.
    // `operationCache` may also be null: the combined cache cell then shows only the RAM preview
    // account, with no operation-cache statistics appended.
    WindowStatusBar(CompositionSession& session, CompositionPreviewController* previewController,
                    media::cache::SourceMirror* mediaDiskCache = nullptr,
                    runtime::OperationCache* operationCache = nullptr, QWidget* parent = nullptr,
                    runtime::MemoryBudgetLedger& ledger = runtime::processMemoryBudgetLedger());

    // A notice that clears itself after five seconds -- a rejected command, an export that
    // finished, a cancellation. It takes precedence over the persistent message while it lasts.
    void showTransientMessage(const QString& message);
    void clearTransientMessage();
    // A message that stays until it is replaced or cleared: "Saving…", "Opening…", export range
    // progress. Passing an empty string clears it.
    void setPersistentMessage(const QString& message);
    // MainWindow owns selection among visible viewers. Cache/preview preparation remains shared.
    void setDisplayedViewer(ViewerEditor* viewer);

  public:
    // Injected memory samples and a monotonic clock keep pressure, recovery and notice tests
    // independent of the host. The legacy byte-only overload treats zero as unavailable.
    void pollMemoryPressureForTest(std::size_t availableBytes);
    void pollMemoryPressureForTest(runtime::MachineMemorySample sample,
                                   runtime::MemoryBudgetLedger::Clock::time_point now);
    [[nodiscard]] QString cacheToolTipForTest() const;
    [[nodiscard]] std::size_t memoryReserveBytesForTest() const noexcept {
        return memoryReserveBytes_;
    }

    // The MCP indicator (ADR 0034): shown only while the server runs or has failed, and a click
    // asks the adapter to reveal the MCP Server panel. Null removes it.
    void setMcpServer(McpServerAdapter* adapter);
    // The compile-failure disclosure (docs/ux/ui-grammar.md, "Compile failure details"): a small
    // button after the preview state, shown only while the preview failed to compile. It opens a
    // popup anchored to this bar. The ForTest hooks drive the same code the viewer's state does.
    void applyPreviewStateForTest(const CompositionPreviewState& preview);
    void openCompileFailureForTest() { openCompileFailure(); }
    [[nodiscard]] bool compileDetailsVisibleForTest() const;
    [[nodiscard]] QWidget* compileFailurePopupForTest() const;
    [[nodiscard]] QString mcpIndicatorTextForTest() const;
    [[nodiscard]] bool mcpIndicatorVisibleForTest() const;
    // Clicks the MCP text segment (it asks the adapter to reveal the MCP Server panel).
    void clickMcpIndicatorForTest();
    // The labelled segments, left to right: each one's visible label texts, joined by " | ".
    [[nodiscard]] QStringList segmentLabelsForTest() const;
    [[nodiscard]] bool segmentVisibleForTest(const QString& objectName) const;
    [[nodiscard]] QString renderPathTextForTest() const;
    [[nodiscard]] QString exposureTextForTest() const;
    [[nodiscard]] QString channelTextForTest() const;
    [[nodiscard]] QString activityTextForTest() const;
    [[nodiscard]] QString memoryTextForTest() const;
    [[nodiscard]] QString diskTextForTest() const;
    [[nodiscard]] QString diskToolTipForTest() const;

    // Test/diagnostic surface only, mirroring ViewerEditor's own *ForTest precedent.
    [[nodiscard]] QString colorChipTextForTest() const;
    [[nodiscard]] QString renderPathToolTipForTest() const;
    [[nodiscard]] QString renderPathAccessibleNameForTest() const;
    [[nodiscard]] QString previewStateTextForTest() const;
    [[nodiscard]] QString droppedFrameTextForTest() const;
    // The disk cache's plain status sentence (also the head of the Disk cache tooltip).
    [[nodiscard]] QString mediaDiskCacheTextForTest() const;
    [[nodiscard]] QString videoTextForTest() const;
    [[nodiscard]] QString videoToolTipForTest() const;
    // Re-reads the video decode reports now (the bar otherwise follows its 250 ms cadence).
    void refreshVideoCellForTest() { refreshVideoCell(); }
    // Whether the 250 ms preview/cache poll is currently running.
    [[nodiscard]] bool cachePollActiveForTest() const;
    // The poll's clock, in a test's hands: with manual on, the 250 ms timer is stopped and stays
    // stopped (showing or restoring the window does not restart it), and each
    // tickCachePollForTest() is one tick of it. Off puts the timer back under updateCachePoll().
    void setCachePollManualForTest(bool manual);
    void tickCachePollForTest() { refreshPreviewCells(); }
    [[nodiscard]] QString messageTextForTest() const;
    [[nodiscard]] QString versionTextForTest() const;
    [[nodiscard]] ViewerEditor* displayedViewerForTest() const noexcept;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  private:
    // The 250 ms poll only has an audience while the bar is on screen: it stops when the bar is
    // hidden or its window is minimised and refreshes once when it resumes.
    void updateCachePoll();
    void buildSegments();
    void refreshActivity();
    void setPreviewNotice(const CompositionPreviewState& preview);
    void setViewerCells(const PreviewColorState& state, const CompositionPreviewState* preview);
    void refreshMemoryCell();
    void applyNarrowLayout();
    void refreshPreviewCells();
    void refreshProbeCell(const ProbeReadout& readout);
    void showProbeReadout(const ProbeReadout& readout);
    void refreshMediaDiskCacheCell();
    void refreshMessage();
    void applyCompileFailure(std::optional<CompileFailure> failure);
    void openCompileFailure();
    void refreshMcpIndicator();
    // Observes the application-owned worker snapshot. This surface never samples the OS.
    void observeMemoryBudget();
    void reportMemoryState(const runtime::MemoryBudgetState& state);
    void applyMemoryPressure(runtime::MachineMemorySample sample,
                             runtime::MemoryBudgetLedger::Clock::time_point now);
    void refreshMemoryToolTip();
    // The active composition's video sources: the latest decode route (software, VA-API or the
    // software fallback and its reason) and any assumed-colour note, from the runtime's decode
    // reports. It is status, not an evaluation diagnostic, so cached frames stay cacheable.
    void refreshVideoCell();

    runtime::MemoryBudgetLedger& memoryLedger_;
    CompositionSession& session_;
    CompositionPreviewController* previewController_ = nullptr;
    QPointer<ViewerEditor> displayedViewer_;
    QMetaObject::Connection displayedViewerConnection_;
    QMetaObject::Connection displayedViewerProbeConnection_;
    QMetaObject::Connection displayedViewerDestroyedConnection_;
    runtime::OperationCache* operationCache_ = nullptr;
    media::cache::SourceMirror* mediaDiskCache_ = nullptr;
    // Segments, left to right: Viewer | Activity | Pixel | Memory | Disk cache | MCP | Version.
    statusbar::Segment* viewerSegment_ = nullptr;
    statusbar::Segment* activitySegment_ = nullptr;
    statusbar::Segment* probeSegment_ = nullptr;
    statusbar::Segment* memorySegment_ = nullptr;
    statusbar::Segment* diskSegment_ = nullptr;
    statusbar::Segment* mcpSegment_ = nullptr;
    statusbar::Segment* versionSegment_ = nullptr;
    std::vector<QWidget*> separators_;
    statusbar::ValueLabel* renderPath_ = nullptr;
    statusbar::ValueLabel* exposure_ = nullptr;
    statusbar::ValueLabel* channel_ = nullptr;
    statusbar::ValueLabel* droppedFrames_ = nullptr;
    statusbar::ValueLabel* video_ = nullptr;
    QWidget* videoGroup_ = nullptr;
    kit::KLabel* activityText_ = nullptr;
    kit::KProgressBar* activityProgress_ = nullptr;
    statusbar::ValueLabel* memory_ = nullptr;
    statusbar::ValueLabel* memoryFrames_ = nullptr;
    QString previewNotice_;
    kit::Color previewNoticeTone_ = kit::Color::Muted;
    statusbar::ValueLabel* mediaDiskCacheCell_ = nullptr;
    statusbar::ClickableValue* mcpIndicator_ = nullptr;
    kit::KButton* compileDetails_ = nullptr;
    QString colorStateText_;
    QString previewStateText_;
    QString videoText_;
    QString memoryToolTip_;
    QString diskStatusText_;
    bool mcpWanted_ = false;
    std::optional<CompileFailure> compileFailure_;
    QPointer<kit::KPopupWindow> compileFailurePopup_;
    QPointer<McpServerAdapter> mcpServer_;
    statusbar::ValueLabel* version_ = nullptr;
    kit::KLabel* probe_ = nullptr;
    QTimer* transientTimer_ = nullptr;
    QTimer* cacheRefreshTimer_ = nullptr;
    bool cachePollManual_ = false;
    // #685: a frame that renders in under kRenderingNoticeDelay never shows "Rendering" or dims the
    // readouts, so playback does not blink the status bar once per frame.
    QElapsedTimer renderingSince_;
    QTimer* renderingNoticeTimer_ = nullptr;
    bool holdRenderingNotice_ = true;
    QElapsedTimer behindSince_;
    [[nodiscard]] bool renderingNoticeHeld() const;
    QTimer* mediaDiskCacheTimer_ = nullptr;
    // #630, #636: the sound store and the bake have no Qt signal (they are Qt-free), so the
    // activity cell reads them on a light poll and repaints only when the readout changes.
    QTimer* backgroundWorkTimer_ = nullptr;
    BackgroundWorkReadout backgroundWork_;
    QString transientMessage_;
    QString persistentMessage_;
    // max(8 GiB, 40% of physical) -- what the memory ledger left to the rest of the machine, and
    // therefore the line below which the machine is short of memory rather than merely busy.
    std::size_t memoryReserveBytes_ = 0;
    // When swap starts the same episode, its distinct notice follows the ordinary trim notice.
    bool pendingSwapNotice_ = false;
    std::uint64_t observedMemorySerial_ = 0;
    ProbeReadout probeReadout_{};
    bool probeFloatFormat_ = false;
    // #715: a readout that only waits for the current frame or its sample, held while the shown
    // one stays up.
    std::optional<ProbeReadout> heldProbeReadout_;
    QTimer* probeHoldTimer_ = nullptr;
};

} // namespace bloom::ui
