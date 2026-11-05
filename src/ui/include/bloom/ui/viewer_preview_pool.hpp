#pragma once

#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>
#include <vector>

namespace bloom::ui {

class CompositionPreviewController;

// #536: where a Viewer gets a preview controller of its own. A pinned Viewer, or a Viewer in a
// secondary workspace window, prepares its frames through its own controller -- built by the
// composition root on the same pipeline, shared frame cache, GPU display route and processing
// policy as the application's -- so it can show another composition, a node's output or its own
// display transform. Memory follows the same usable-capacity rule as every other preview: there is
// no per-window cache or cap. The Viewer owns the controller; the pool reaches every live one at
// shutdown.
class ViewerPreviewPool final : public QObject {
    Q_OBJECT

  public:
    using Factory = std::function<std::unique_ptr<CompositionPreviewController>()>;

    explicit ViewerPreviewPool(Factory factory, QObject* parent = nullptr);
    ~ViewerPreviewPool() override;

    // A new controller, or null when there is no factory or shutdown has begun.
    [[nodiscard]] std::unique_ptr<CompositionPreviewController> create();
    // Live controllers made by this pool (test and diagnostics seam).
    [[nodiscard]] std::size_t liveCount() const;

    // The shutdown contract of every controller made here, on the application controller's terms:
    // admission closes first, published frames are released once tasks and surfaces retired.
    void beginShutdown();
    void releasePublishedFramesForShutdown();

  private:
    Factory factory_;
    std::vector<QPointer<CompositionPreviewController>> live_;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
