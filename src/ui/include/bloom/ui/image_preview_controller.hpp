#pragma once

// #490: the Image Preview node's pixels -- its node-card thumbnail and the node editor background.
// A preview is the Viewer's own preview of a Viewer-only inspection plan rooted at the output the
// node's input is connected to (runtime::ChannelInspection), requested through
// CompositionPreviewController at Background priority: the GPU preview service's display route,
// with its own labelled CPU fallback, or -- for the Normalized view, which needs scene-linear
// pixels -- the Viewer's analysis route, as the pixel probe uses. The plan never enters the
// project, the composition's compiled plan, RAM Preview frames or any render or export.
//
// Each request is evaluated at the size it is drawn (a proxy extent of the composition format, so
// never full resolution unless the background is shown at 100%), on workers, one in flight per
// node; a change of input, time, size or display cancels it. Images are kept by the request's
// identity (the inspected output, revision, time, extent, display and view) so a node, a time or a
// size seen before is drawn without evaluating again. Which node is the background, and whether it
// is shown, is session state: never saved, never undoable.
//
// #493: the same requests draw a Curves node's input luminance histogram behind its curve graph:
// the input is prepared through the display route (on the GPU, its CPU fallback labelled) at a
// small proxy extent, and a worker bins its displayed luma. It is requested only by a visible
// graph's paint and never touches the render output.

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/image_preview.hpp>
#include <bloom/runtime/channel_inspection_listing.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

class ImagePreviewController final : public QObject {
    Q_OBJECT

  public:
    // Images kept across nodes, times and sizes; the least recently drawn is dropped beyond this.
    static constexpr std::size_t kCapacity = 48;
    // The smallest edge a request is made at, so a card zoomed far out still reads.
    static constexpr int kMinimumEdge = 16;

    enum class State : std::uint8_t {
        // The input is not connected (or ends at an unconnected reroute).
        Unconnected,
        // A frame for the drawn identity is being prepared; `image` is the newest one kept.
        Pending,
        // `image` answers the drawn identity.
        Ready,
        // The drawn identity could not be prepared; `message` says why.
        Failed,
    };

    struct Preview final {
        QImage image;
        State state = State::Unconnected;
        // The pixels were evaluated or displayed on the CPU (the Normalized view, the GPU
        // service's fallback, or no GPU service): the card labels it, never silently.
        bool cpu = false;
        QString message;
    };

    ImagePreviewController(CompositionSession& session, CompositionPreviewController& preview,
                           runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                           QObject* parent = nullptr);
    ~ImagePreviewController() override;

    // `node`'s preview drawn `devicePixels` large. Requests the frame when the newest one kept does
    // not answer the current input, time, size and display; never evaluates on the calling thread.
    [[nodiscard]] Preview preview(document::NodeId node, QSize devicePixels);

    // #493: a Curves node's input histogram: kHistogramBins counts of the displayed luma
    // (0.213 R + 0.715 G + 0.072 B, the node's own weights) over 0..1, scaled so the fullest bin is
    // one. Requested at kHistogramEdge on the longest side, like preview(): only when asked, from
    // a visible graph's paint, never on the calling thread.
    static constexpr std::size_t kHistogramBins = 128;
    static constexpr int kHistogramEdge = 256;
    struct Histogram final {
        std::vector<float> bins;
        State state = State::Unconnected;
        bool cpu = false;
        QString message;
    };
    [[nodiscard]] Histogram histogram(document::NodeId node);
    // #483: whether the image a Z Defocus or Depth Fog reads as depth looks like a colour picture
    // rather than a depth pass: the fraction (0..1) of its covered pixels whose displayed red,
    // green and blue differ by more than kDepthSpreadLevels of 255, read like histogram() at
    // kHistogramEdge from the node's `depth` input on a worker. A depth pass is grey (R = G = B),
    // so a large fraction means a colour image (a beauty or a normal pass) is connected.
    static constexpr int kDepthSpreadLevels = 24;
    struct ChannelSpread final {
        double fraction = 0.0;
        State state = State::Unconnected;
        bool cpu = false;
        QString message;
    };
    [[nodiscard]] ChannelSpread depthChannelSpread(document::NodeId node);
    // Session state, not project truth: whether curve graphs draw the histogram (on by default).
    [[nodiscard]] bool histogramsShown() const noexcept { return histogramsShown_; }
    void setHistogramsShown(bool shown);

    // The display an Image Preview node asks for (its `display` parameter).
    [[nodiscard]] document::ImagePreviewDisplay display(document::NodeId node) const;

    // Session state, not project truth: the Image Preview the node editor draws behind the graph.
    // One node at a time for the current composition; choosing another moves it.
    [[nodiscard]] std::optional<document::NodeId> backgroundNode() const;
    void setBackgroundNode(std::optional<document::NodeId> node);
    [[nodiscard]] bool backgroundVisible() const noexcept { return backgroundVisible_; }
    void setBackgroundVisible(bool visible);

    void cancel();

    // Test seams: frames submitted, requests cancelled because what is drawn changed, and the
    // thread the last image was made on.
    [[nodiscard]] std::uint64_t submissionCount() const noexcept { return submissions_; }
    [[nodiscard]] std::uint64_t cancellationCount() const noexcept { return cancellations_; }
    [[nodiscard]] std::optional<std::thread::id> lastWorkerThread() const noexcept {
        return lastWorkerThread_;
    }

    // #685: requests the picture a card last drawn for the current time and document, without a
    // redraw. A view calls it when the playhead moves, so the card repaints only if the new
    // picture differs from the one it shows, and at most once for the frame.
    void refresh(document::NodeId node);

  signals:
    // A prepared image arrived, or the document or the time moved: redraw every preview.
    void changed();
    // #685: what a card or the background shows may differ: the document moved, a different image
    // arrived, or a request failed. Unlike changed(), the playhead alone does not emit it.
    void previewChanged();
    // The background node or its visibility changed.
    void backgroundChanged();

  private:
    struct Key final {
        document::ProjectId project;
        document::CompositionId composition;
        document::Revision revision;
        core::RationalTime time;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        document::ImagePreviewDisplay display = document::ImagePreviewDisplay::Rgba;
        document::OutputPortRef source;
        bool channelSet = false;
        std::string displayName;
        std::string viewName;
        bool showLook = true;
        // #493: a Curves node's input histogram rather than an Image Preview's picture.
        bool histogram = false;
        // #483: a depth effect's depth-input channel spread (in `bins`, one value).
        bool channelSpread = false;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct Converted final {
        QImage image;
        std::vector<float> bins;
        std::thread::id thread;
    };
    using ConvertedHandle = std::shared_ptr<const Converted>;
    using ListingHandle = std::shared_ptr<const runtime::ChannelInspectionListing>;
    struct Pending final {
        Key key;
        std::optional<runtime::TaskHandle<ListingHandle>> listing;
        std::optional<runtime::TaskHandle<PreviewPreparationResultHandle>> frame;
        std::optional<runtime::TaskHandle<ConvertedHandle>> convert;
        bool cpu = false;
        bool cancelled = false;
    };
    struct Kept final {
        Key key;
        QImage image;
        std::vector<float> bins;
        bool cpu = false;
        std::uint64_t used = 0;
    };
    struct Shown final {
        std::optional<Key> key;
        QImage image;
        std::vector<float> bins;
        bool cpu = false;
        std::optional<Key> failed;
        QString failure;
    };

    [[nodiscard]] std::optional<Key> keyFor(document::NodeId node, QSize devicePixels,
                                            bool histogram = false,
                                            bool channelSpread = false) const;
    void want(document::NodeId node, const Key& key);
    // Cancels the node's request in flight and forgets what it wanted (its input went away).
    void drop(document::NodeId node);
    void submit(document::NodeId node, Key key);
    // False when the request could not start (fail() has said why); the caller drops it.
    [[nodiscard]] bool startFrame(document::NodeId node, Pending& pending);
    [[nodiscard]] bool startConvert(document::NodeId node, Pending& pending,
                                    const PreviewPreparationResultHandle& result);
    void fail(document::NodeId node, const Key& key, QString message);
    void notify();
    void poll();
    void resetForProject();

    CompositionSession& session_;
    CompositionPreviewController& preview_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    document::ProjectId project_{};
    std::map<document::NodeId, Pending> pending_;
    std::map<document::NodeId, Key> wanted_;
    // The device size each node was last drawn at, so refresh() asks for the same identity.
    std::map<document::NodeId, QSize> drawnSize_;
    std::map<document::NodeId, Shown> shown_;
    std::vector<Kept> kept_;
    // The default colour channels (R, G, B, A) of a ChannelSet source, per revision and source.
    struct Channels final {
        document::Revision revision;
        document::OutputPortRef source;
        std::vector<std::string> names;
    };
    std::vector<Channels> channels_;
    std::optional<document::CompositionId> backgroundComposition_;
    std::optional<document::NodeId> backgroundNode_;
    bool backgroundVisible_ = true;
    bool histogramsShown_ = true;
    std::uint64_t submissions_ = 0;
    std::uint64_t cancellations_ = 0;
    std::uint64_t uses_ = 0;
    std::optional<std::thread::id> lastWorkerThread_;
};

} // namespace bloom::ui
