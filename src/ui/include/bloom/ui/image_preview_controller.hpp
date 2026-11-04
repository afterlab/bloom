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

  signals:
    // A prepared image arrived, or the document or the time moved: redraw every preview.
    void changed();
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
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct Converted final {
        QImage image;
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
        bool cpu = false;
        std::uint64_t used = 0;
    };
    struct Shown final {
        std::optional<Key> key;
        QImage image;
        bool cpu = false;
        std::optional<Key> failed;
        QString failure;
    };

    [[nodiscard]] std::optional<Key> keyFor(document::NodeId node, QSize devicePixels) const;
    void want(document::NodeId node, const Key& key);
    void submit(document::NodeId node, Key key);
    // False when the request could not start (fail() has said why); the caller drops it.
    [[nodiscard]] bool startFrame(document::NodeId node, Pending& pending);
    [[nodiscard]] bool startConvert(document::NodeId node, Pending& pending,
                                    const PreviewPreparationResultHandle& result);
    void fail(document::NodeId node, const Key& key, QString message);
    void poll();
    void resetForProject();

    CompositionSession& session_;
    CompositionPreviewController& preview_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    document::ProjectId project_{};
    std::map<document::NodeId, Pending> pending_;
    std::map<document::NodeId, Key> wanted_;
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
    std::uint64_t submissions_ = 0;
    std::uint64_t cancellations_ = 0;
    std::uint64_t uses_ = 0;
    std::optional<std::thread::id> lastWorkerThread_;
};

} // namespace bloom::ui
