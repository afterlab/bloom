#pragma once
#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_drop_operations.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/image.hpp>
#include <bloom/platform/storage_class.hpp>
#include <bloom/runtime/audio_stream_key.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/memory_pool_counters.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

class QWidget;
namespace bloom::media::cache {
class SourceMirror;
} // namespace bloom::media::cache
namespace bloom::runtime {
class GpuProcessingPhasePolicy;
}
namespace bloom::ui::detail {
class ThumbnailProbeMemo;
} // namespace bloom::ui::detail
namespace bloom::ui {
class CompositionSession;
class ProjectHost;
class TaskUiBridge;
struct AudioProcessingAdmission final {
    std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy;
    // Explicit allowance for standalone hosts/tests without a sampled processing root.
    std::uint64_t standaloneByteAllowance = 0;
    // Retention target. A larger admitted buffer can still cross the playback handoff.
    std::size_t cacheByteCapacity = std::size_t{2} * 1024 * 1024 * 1024;
};
class AssetController final : public QObject {
    Q_OBJECT
  public:
    // One item a batch import skipped, with its plain reason (#373).
    struct ImportSkip final {
        QString file;
        QString reason;
    };
    // How a batch import ended (#377). Imported items were committed as one undoable transaction.
    struct ImportReport final {
        std::size_t imported = 0;
        std::vector<ImportSkip> skipped;
        // Items never finished because the artist cancelled, including the one in progress.
        std::size_t notImported = 0;
        bool cancelled = false;
        // The import stopped for the application (closing the project or quitting); nothing
        // was added and no summary is due.
        bool discarded = false;
        // Why nothing at all was added when no single file is to blame; empty otherwise.
        QString failure;
        // Every asset the committed items resolved to, in import order: the ones just added
        // and the ones the project already held.
        std::vector<document::AssetId> assets;
        // What the import's follow-up did with them ("Added 2 layers"), for the status line and
        // the summary; empty when there was none.
        QString followUp;
    };
    // Runs once the import has committed, with ImportReport::assets, as its own undoable step
    // (a drop on the Timeline adds layers). Returns a plain sentence for the report, or nothing.
    using ImportFollowUp = std::function<QString(const std::vector<document::AssetId>&)>;
    // Where the running batch import stands, read from the import worker's latest report (see
    // commands::ImportProgress: items, work units, and the sequence member being read).
    struct ImportProgressState final {
        // Searching folders or listing sequence members: the work total is not known yet.
        bool finding = true;
        std::size_t completed = 0;
        std::size_t total = 0;
        std::uint64_t workDone = 0;
        std::uint64_t workTotal = 0;
        // The current file's name (or folder's, while finding).
        QString current;
        std::uint64_t found = 0;
        bool findingFrames = false;
        std::uint64_t framesRead = 0;
        std::uint64_t frames = 0;
        bool stopping = false;
        // The thread preparing the import, so tests can prove it is not the interface thread.
        std::optional<std::thread::id> workerThread;
    };
    // `mediaDiskCache` is the same content-addressed disk cache the runtime evaluator consults
    // (docs/architecture/media-io.md "Disk cache"): proxy/thumbnail decodes go through it too, so
    // an asset the evaluator already decoded to disk never re-decodes here, and vice versa. Null
    // (the default) disables the disk-cache stage; nothing about the existing QImage proxy cache
    // changes either way.
    AssetController(CompositionSession& session, ProjectHost& host,
                    runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                    media::cache::SourceMirror* mediaDiskCache = nullptr, QObject* parent = nullptr,
                    AudioProcessingAdmission audioAdmission = AudioProcessingAdmission{});
    ~AssetController() override;
    // The Import Media dialog, with its import options (#381): sequences or single images, a frame
    // range, skipping missing frames, and Create a composition, whose follow-up makes one from the
    // imported footage.
    void requestImport(QWidget* parent = nullptr);
    // The same dialog, with `followUp` run once the import it starts commits (the Viewer's Add >
    // Import and Add… adds the imported media as layers); it then offers no composition.
    void requestImport(QWidget* parent, ImportFollowUp followUp,
                       std::optional<commands::LayerDropPlacement> svgPlacement = std::nullopt);
    // Imports into the current import folder (see setImportFolder).
    void importFiles(const QStringList& paths);
    // Imports into `folder`; nullopt is the Assets root. `followUp` runs after the commit. Every
    // import reads the Import preferences (sequence frame rate, still duration) into `options`.
    // `svgPlacement`: where an SVG file among `paths` joins the active composition (a drop on the
    // Timeline or the Viewer), as the footage of the same drop does through `followUp`. Without it
    // the SVG's artwork goes on top at the start.
    void importFiles(const QStringList& paths, std::optional<document::AssetFolderId> folder,
                     ImportFollowUp followUp = {}, commands::ImportOptions options = {},
                     std::optional<commands::LayerDropPlacement> svgPlacement = std::nullopt);
    // #642: how the SVG files of one import or drop batch import. Asks once for the batch: the
    // remembered answer (Preferences > Import) is used without a prompt; otherwise the chooser
    // opens over the active window with As one layer selected and a "Remember my choice" check.
    // `done` runs once, with the mode, or nullopt when the artist cancelled; it runs at once when
    // no prompt is needed or no window can show one (the default, As one layer, then applies). The
    // drop routes and Add to Composition share this one call.
    void chooseSvgImportMode(std::size_t count,
                             std::function<void(std::optional<commands::SvgImportMode>)> done);
    // Test seam: answers the chooser without opening it, with the number of SVG files it is asked
    // about. Unset, the chooser opens as it does for the artist.
    void setSvgImportChooserForTesting(
        std::function<std::optional<commands::SvgImportMode>(std::size_t)> chooser) {
        svgChooser_ = std::move(chooser);
    }
    // #646: adds assets and compositions to the active composition as layers at `placement` (a drop
    // on the Viewer or the Timeline, the Assets panel's Add to Composition). An imported SVG asset
    // is not footage: its artwork is built through the SVG import, in the mode the chooser gives
    // (asked once for the whole batch), on the asset's own record, from its file or the project's
    // stored copy (read on the import worker). Everything is one undo step. A batch with no SVG
    // commits at once. `committed` runs with the result of a successful commit. A missing SVG is
    // refused by name and nothing is added.
    void addToComposition(std::vector<commands::LayerDropItem> items,
                          commands::LayerDropPlacement placement,
                          std::function<void(const commands::CommandResult&)> committed = {});
    // #646: the node editor's route: an SVG asset dropped at `position` becomes unconnected artwork
    // nodes (a Vector Artwork node, or the artwork's group), other footage its source node.
    void addAssetNode(document::AssetId asset, document::Vec2d position,
                      std::function<void(const commands::CommandResult&)> committed = {});
    // #646: New Composition from Selection for `assets`, SVG assets included (their artwork).
    // `committed` runs with the result of a successful commit.
    void
    createCompositionFromAssets(std::vector<document::AssetId> assets,
                                std::optional<document::CompositionId> templateComposition,
                                std::string fallbackName,
                                std::optional<document::AssetFolderId> folder,
                                std::function<void(const commands::CommandResult&)> committed = {});
    // #642 Break Apart: turns the Vector Artwork `layer` holds into editable shapes in place, as
    // one undo step. The held SVG is parsed (and fonts matched) on the blocking-I/O worker; the
    // edit commits when it is ready. Refused, with a diagnostic, while another import runs.
    void breakApartSvg(document::CompositionId composition, document::LayerId layer);
    // The same for several layers: all of them, in one undo step. A layer that is scaled past 100%
    // or turned is named in the job log: the editable shapes are resampled and may look softer.
    void breakApartSvg(document::CompositionId composition, std::vector<document::LayerId> layers);
    // The selected layer when it holds an SVG as one Vector Artwork layer (the Composition menu's
    // Break Apart SVG acts on it); nullopt otherwise.
    [[nodiscard]] std::optional<document::LayerId> selectedVectorArtworkLayer() const;
    // Every selected layer that holds one, in selection order.
    [[nodiscard]] std::vector<document::LayerId> selectedVectorArtworkLayers() const;
    // Whether `layer` of the current composition holds an SVG as one Vector Artwork layer.
    [[nodiscard]] bool holdsVectorArtwork(document::LayerId layer) const;
    // New Composition from Selection for the assets an import resolved to, opened once made.
    [[nodiscard]] ImportFollowUp compositionFollowUp();
    // Session state (#378): the Assets folder an import lands in when no drop target names one.
    // The Assets panel keeps it on the selected folder, or the folder of the selected item.
    void setImportFolder(std::optional<document::AssetFolderId> folder) noexcept {
        importFolder_ = folder;
    }
    [[nodiscard]] const std::optional<document::AssetFolderId>& importFolder() const noexcept {
        return importFolder_;
    }
    void relink(document::AssetId id, QWidget* parent = nullptr);
    // Reload Footage (#378): probes the asset's own file again on the import worker, as a relink to
    // the same path, so changed bytes update the record and unchanged ones leave it alone.
    void reload(document::AssetId id);
    // Where the asset's file (a sequence's first member) is, worked out from its locator and the
    // project folder without touching the filesystem. Empty when the locator names no file.
    [[nodiscard]] std::filesystem::path sourcePath(document::AssetId id) const;
    void remove(document::AssetId id);
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    // True while a batch import (not a relink or reload) is being prepared.
    [[nodiscard]] bool importing() const noexcept { return busy_ && importShared_ != nullptr; }
    [[nodiscard]] ImportProgressState importProgress() const;
    // Cancel from the import dialog: the worker stops at its next safe point and the items
    // already prepared are still imported, as one undoable step. cancel() discards instead.
    void stopImport();
    // Test seam: runs on the import worker after each progress report, with that report.
    void
    setImportProgressHookForTesting(std::function<void(const commands::ImportProgress&)> hook) {
        importProgressHook_ = std::move(hook);
    }
    [[nodiscard]] bool missing(document::AssetId id) const;
    // Present but not displayable as it is (empty when fine or missing); see Preview::problem.
    [[nodiscard]] QString problem(document::AssetId id) const;
    // A data-only image (no colour layer) -- valid, used through its channels.
    [[nodiscard]] bool dataOnly(document::AssetId id) const;
    // The source's bytes on disk (a sequence's members summed), measured by the thumbnail worker;
    // empty until it has run, for a missing source, or for a locator that names no file.
    [[nodiscard]] std::optional<std::uint64_t> fileSize(document::AssetId id) const;
    // #718: where the source lives and what the disk cache holds of it, as one phrase ("Network ·
    // local copy kept"); empty until the thumbnail worker has classified the file, for a missing
    // source, or for a locator that names no file. The storage class was read on the worker; the
    // cached state is read now (an in-memory index lookup), so it is always current.
    [[nodiscard]] QString sourceStorageText(document::AssetId id) const;
    // One line per file-backed asset, "<name>: <sourceStorageText>", in project order: at most
    // `limit` of them, then one "and N more" line.
    [[nodiscard]] QStringList sourceStorageSummary(int limit) const;
    [[nodiscard]] QString audioDiagnostic(document::AssetId id) const;
    [[nodiscard]] QImage thumbnail(document::AssetId id) const;
    [[nodiscard]] QString inputColorSpaceDisplay(document::AssetId id) const;
    [[nodiscard]] QString inputColorSpaceWarning(document::AssetId id) const;
    // The resolved input colour space by its plain name ("Linear Rec.709", "sRGB", or the
    // config's own name), with nothing added; empty until the worker has resolved it.
    [[nodiscard]] QString inputColorSpaceName(document::AssetId id) const;
    // True when Auto chose the space from the media tags (or assumed it).
    [[nodiscard]] bool inputColorSpaceAutomatic(document::AssetId id) const;
    // True when Auto resolved an untagged or partly tagged video by the size convention; the
    // warning is then the visible "Color tags missing — assumed …" note.
    [[nodiscard]] bool inputColorSpaceAssumed(document::AssetId id) const;
    void requestInputColorSpaces();
    // ADR 0026: the part and layer pickers read the image probe through this worker (identity
    // verified, header only). The asset record persists no part list; the result is session
    // state keyed by the asset's content digest. changed() is emitted when it arrives.
    void requestImageParts(document::AssetId id);
    [[nodiscard]] const std::vector<media::ImagePartInfo>* imageParts(document::AssetId id) const;
    [[nodiscard]] const std::vector<color::OcioColorSpaceInfo>* inputColorSpaces() const noexcept;
    [[nodiscard]] QString inputColorCatalogDiagnostic() const;
    [[nodiscard]] std::shared_ptr<const media::audio::WaveformSummary>
    waveform(document::AssetId id) const;
    // The decoded buffer of one audio stream (0-based among the asset's audio streams).
    [[nodiscard]] std::shared_ptr<const media::audio::AudioBuffer>
    audioBuffer(document::AssetId id, std::uint32_t stream = 0) const;
    // Called by the playback session after it has adopted the active mix's buffers.
    void releaseAudioDelivery();
    // The playback session's mix plays an audio stream this controller holds no buffer for (a
    // delivered buffer lives only while a mix pins it): decode the project's audio again. The
    // asset job otherwise runs only when its inputs change (#718), and a layer that is unmuted,
    // or placed after its import, changes none of them.
    void requestAudioDelivery();
    void retryAudioAfterCapacitySample();
    [[nodiscard]] std::filesystem::path baseDirectory() const;
    // CACHEFIX-1 accounting counters (no UI): the bytes this controller's two in-memory caches
    // hold. Both are hard-capped rather than budgeted -- they are small next to the frame caches,
    // but "small" has to be a measured claim, not an assumption in a comment.
    [[nodiscard]] std::size_t proxyCacheBytes() const noexcept { return proxyCacheBytes_; }
    [[nodiscard]] std::size_t decodedAudioBytes() const noexcept { return decodedAudioBytes_; }
    [[nodiscard]] bool thumbnailsPending() const noexcept {
        return previewPending_ || previewDirty_;
    }
    // How many thumbnail jobs have been handed to the worker. A document revision decides at once,
    // on the UI thread, whether it needs one (refresh()), so an edit that needs none leaves this
    // and thumbnailsPending() as they were the moment the edit returns.
    [[nodiscard]] std::uint64_t thumbnailJobsStarted() const noexcept {
        return thumbnailJobsStarted_;
    }
    void cancel();
    // Prevent thumbnail decode submissions while an application cache purge drains its old
    // generation. Import/probe work does not access the shared decoded-frame store.
    void setCachePurgeGate(bool gated);
    [[nodiscard]] bool acceptsEdits() const;
    [[nodiscard]] QByteArray dragToken() const { return dragToken_; }
    // Verify footage (#495): hashes the project's still images and unverified image-sequence
    // members once, in the background (BlockingIo, Background priority, idle OS priority), so the
    // ADR 0025 memo learns their digests and the next save records them. Started when a project is
    // installed and after an import, Relink or Reload; a newer request supersedes (cancels) the
    // older one. Waits while an import or a save/open is running (one blocking-I/O worker), and a
    // save or import that starts meanwhile stops it at once; it starts again when they finish.
    void verifyFootage();
    [[nodiscard]] bool verifyingFootage() const noexcept { return verifying_; }
    // What the last verification pass that ran to the end found (files, hashed, changed).
    struct FootageVerificationSummary final {
        std::size_t files = 0;
        std::size_t hashed = 0;
        std::size_t changed = 0;
    };
    [[nodiscard]] const std::optional<FootageVerificationSummary>&
    lastFootageVerification() const noexcept {
        return lastVerification_;
    }
  signals:
    void changed();
    void activityChanged();
    void diagnostic(QString message);
    void importStarted();
    void importFinished(const bloom::ui::AssetController::ImportReport& report);
    // A verification pass found bytes that differ from their recorded digest (#495). The
    // application purges the decoded media and finished-frame caches through the media purge, so
    // nothing rendered from the changed bytes stays cached.
    void footageChanged();
    // #645: Break Apart committed: each of `layers` now holds its SVG as editable shapes.
    void brokenApart(bloom::document::CompositionId composition,
                     std::vector<bloom::document::LayerId> layers);

  private:
    using OperationHandle = std::shared_ptr<std::unique_ptr<commands::Operation>>;
    // Shared between a batch import's worker and this controller: the worker's latest progress
    // and final per-item outcome, and the artist's stop request.
    struct ImportShared final {
        mutable std::mutex mutex;
        commands::ImportProgress progress;
        std::optional<std::thread::id> workerThread;
        std::vector<commands::ImportFailure> failures;
        std::size_t notImported = 0;
        bool cancelled = false;
        std::atomic<bool> stop{false};
    };
    struct Preview {
        QImage image;
        std::string key;
        std::string inputColorSpaceName;
        std::string inputColorSpaceWarning;
        std::string audioDiagnostic;
        bool inputColorSpaceAutomatic = false;
        bool inputColorSpaceAssumed = false;
        // The source file is absent or no longer the imported bytes (Relink fixes it).
        bool missing = false;
        // The source is present but cannot be shown as it is (an uninterpretable colour tag, an
        // undecodable frame); the text says what to do. Never a Relink prompt.
        std::string problem;
        // The image has no colour layer: its pixels are data channels (Cryptomatte, AOVs). A
        // valid asset whose thumbnail is the Cryptomatte ID colours or its first data channel.
        bool dataOnly = false;
        // The source's bytes on disk, measured on the worker (see fileSize()).
        std::optional<std::uint64_t> fileBytes;
        // #718: the storage class and volume of the source's first file, and every file of the
        // source resolved to its path (a sequence's members), all found on the worker.
        std::optional<platform::StorageVolume> storage;
        std::shared_ptr<const std::vector<std::filesystem::path>> sourcePaths;
    };
    using Previews = std::map<document::AssetId, Preview>;
    using Waveforms =
        std::map<document::AssetId, std::shared_ptr<const media::audio::WaveformSummary>>;
    using AudioBuffers =
        std::map<runtime::AudioStreamKey, std::shared_ptr<const media::audio::AudioBuffer>>;
    struct Thumbnails {
        Previews assets;
        std::map<std::string, QImage> cache;
        Waveforms waveforms;
        AudioBuffers audioBuffers;
        AudioBuffers audioDeliveryBuffers;
        // CACHEFIX-1 accounting: what these two maps actually hold, carried back from the worker
        // so the controller can report it without walking every entry on the interface thread.
        std::size_t proxyBytes = 0;
        std::size_t audioBytes = 0;
        bool retryAudioAfterCapacitySample = false;
    };
    struct InputColorCatalog {
        document::ColorSettings settings;
        std::string workingColorSpaceId;
        std::vector<color::OcioColorSpaceInfo> spaces;
        QString diagnostic;
    };
    void prepare(const QStringList& paths, document::AssetId relinkId = {},
                 std::optional<document::AssetFolderId> folder = {},
                 commands::ImportOptions options = {},
                 commands::SvgImportMode svgMode = commands::kDefaultSvgImportMode,
                 std::optional<commands::LayerDropPlacement> svgPlacement = std::nullopt);
    void refresh();
    void startThumbnails();
    void poll();
    // One status notice for an import job's diagnostics; the Jobs panel keeps each one.
    void emitImportFailures(const std::vector<runtime::TaskDiagnostic>& issues);
    // Cancels a running verification pass; `resume` starts it again once the worker is free.
    void stopFootageVerification(bool resume);
    void finishFootageVerification();
    void finishImport(runtime::TaskResult<OperationHandle>& result);
    CompositionSession& session_;
    std::optional<document::AssetFolderId> importFolder_;
    // Reload Footage requests waiting for the import worker, run one after another.
    std::deque<document::AssetId> pendingReloads_;
    std::shared_ptr<ImportShared> importShared_;
    ImportFollowUp importFollowUp_;
    // #343: SVG files of a batch wait their turn; each imports as its own native-layer artwork.
    struct PendingSvg final {
        std::filesystem::path path;
        commands::SvgImportMode mode = commands::kDefaultSvgImportMode;
        std::optional<commands::LayerDropPlacement> placement;
    };
    std::vector<PendingSvg> pendingSvg_;
    void startNextSvg();
    std::function<std::optional<commands::SvgImportMode>(std::size_t)> svgChooser_;
    // A Break Apart job running on the import worker, committed by finishBreakApart.
    struct BreakApartJob final {
        document::CompositionId composition;
        std::vector<document::LayerId> layers;
    };
    std::optional<BreakApartJob> breakApart_;
    // #646: an add of SVG assets running on the import worker, committed by finishSvgPlacement.
    using SvgOperationBuilder =
        std::function<std::unique_ptr<commands::Operation>(commands::SvgAssetSources&&)>;
    struct SvgPlacementJob final {
        std::string label;
        std::function<void(const commands::CommandResult&)> committed;
    };
    std::optional<SvgPlacementJob> svgPlacement_;
    void startSvgPlacement(std::string label, std::vector<document::AssetId> svgAssets,
                           SvgOperationBuilder build,
                           std::function<void(const commands::CommandResult&)> committed);
    void finishSvgPlacement(runtime::TaskResult<OperationHandle>& result);
    void finishBreakApart(runtime::TaskResult<OperationHandle>& result);
    std::function<void(const commands::ImportProgress&)> importProgressHook_;
    ProjectHost& host_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    media::cache::SourceMirror* mediaDiskCache_ = nullptr;
    AudioProcessingAdmission audioAdmission_;
    runtime::TaskHandle<OperationHandle> import_;
    struct FootageVerificationResult;
    runtime::TaskHandle<std::shared_ptr<FootageVerificationResult>> verification_;
    std::optional<FootageVerificationSummary> lastVerification_;
    bool verifying_ = false;
    // A pass was asked for (or stopped for other blocking I/O) and is still due.
    bool verificationDue_ = false;
    runtime::TaskHandle<std::shared_ptr<Thumbnails>> preview_;
    runtime::TaskHandle<std::shared_ptr<InputColorCatalog>> inputColorCatalogTask_;
    std::shared_ptr<InputColorCatalog> inputColorCatalog_;
    struct ImageParts final {
        core::Sha256Digest digest;
        std::vector<media::ImagePartInfo> parts;
    };
    std::map<document::AssetId, ImageParts> imageParts_;
    std::map<document::AssetId, runtime::TaskHandle<std::shared_ptr<ImageParts>>> imagePartTasks_;
    std::optional<document::ColorSettings> inputColorCatalogRequestedSettings_;
    std::string inputColorCatalogRequestedWorking_;
    bool inputColorCatalogPending_ = false;
    std::optional<document::Snapshot> base_;
    Previews previews_;
    std::map<std::string, QImage> thumbnailCache_;
    // #718: what the thumbnail worker learned about each source file, kept between its runs, and
    // the inputs of the run last started (a document edit that changes none of them starts none).
    std::shared_ptr<detail::ThumbnailProbeMemo> thumbnailProbes_;
    struct ThumbnailInputs final {
        document::Snapshot snapshot; // keeps `assets` alive
        std::span<const document::AssetRecord> assets;
        runtime::EvaluationColorIntent color;
        document::FrameRate rate;
        std::filesystem::path directory;
    };
    std::optional<ThumbnailInputs> thumbnailInputs_;
    Waveforms waveforms_;
    AudioBuffers audioBuffers_;
    AudioBuffers audioDeliveryBuffers_;
    std::map<runtime::AudioStreamKey, std::weak_ptr<const media::audio::AudioBuffer>>
        adoptedAudioBuffers_;
    std::size_t proxyCacheBytes_ = 0;
    std::size_t decodedAudioBytes_ = 0;
    // The Profiler's counters for the two pools (#423); shared with the preview worker.
    std::shared_ptr<runtime::MemoryPoolCounters> proxyCounters_ =
        std::make_shared<runtime::MemoryPoolCounters>();
    std::shared_ptr<runtime::MemoryPoolCounters> audioCounters_ =
        std::make_shared<runtime::MemoryPoolCounters>();
    QByteArray dragToken_;
    bool busy_ = false;
    bool previewPending_ = false;
    bool previewDirty_ = false;
    bool cachePurgeGate_ = false;
    bool retryAudioAfterCapacitySample_ = false;
    bool assetsDirty_ = true;
    bool previewIncludesAssets_ = false;
    std::uint64_t contentGeneration_ = 0;
    std::uint64_t thumbnailJobsStarted_ = 0;
    std::uint64_t previewContentGeneration_ = 0;
};
} // namespace bloom::ui
