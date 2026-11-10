#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// #645 Viewer picking on evaluated geometry (docs/user-guide/viewer.md, "Drilling into groups").
//
// A click inside an entered group asks which of the group's children is under the pointer at the
// displayed frame. The answer is the outline the frame draws, not a bounding box: a Path-kind or
// primitive Shape hits on its fill area (its fill rule) when its fill is on and within half its
// stroke width plus `tolerance` of its outline when its stroke is on; any other leaf hits on its
// evaluated bounds quad; a layer with path masks hits only where they cover half or more (their
// drawn edge; a matte mask, whose shape is pixels, does not narrow a pick); a layer with a track
// matte hits only where its matte also hits (inverted: where it does not); a group hits where any of
// its non-matte descendants hits. Children are tried topmost first and the first hit wins.
//
// Placement comes from the displayed frame's own evaluated bounds (one content-to-parent affine
// per layer, the same frame the artist sees, after transforms, parents and animation); outlines
// come from the plan's value graph evaluated at the frame's time (keys, Path nodes, morphs, path
// operators). No pixel is evaluated, so nothing here is a render route of the GPU coverage
// contract.
//
// Heavy work: compile (through the caller's shared plan cache) and value-graph evaluation. Never
// call it on the UI thread; the Viewer runs it as a task.

namespace bloom::runtime {

struct ViewerPickRequest final {
    document::Snapshot snapshot;
    document::CompositionId composition;
    core::RationalTime time;
    // The entered groups, outermost first; empty picks among the top level's layers.
    std::vector<document::LayerId> level;
    // Composition pixels.
    document::Vec2d point;
    // Extra reach around an outline, in composition pixels (half a gizmo handle on screen).
    double tolerance = 0.0;
    // Every layer's evaluated bounds in the displayed frame (CompositionPreviewController::
    // currentLayerBounds): `polygon` is the content box in the space of the Merge the layer is
    // drawn into.
    std::vector<EvaluatedOperationBounds> bounds;
    // The displayed frame's own plan. Used, with no compile at all, when it was compiled from
    // `snapshot`'s revision of `composition`; otherwise the picker compiles.
    std::shared_ptr<const CompiledCompositionPlan> plan;
};

enum class ViewerPickStatus : std::uint8_t {
    Picked,
    Cancelled,
    // The level no longer resolves or the composition does not compile; `diagnostic` says why.
    Unsupported,
    Failed,
};

struct ViewerPickResult final {
    ViewerPickStatus status = ViewerPickStatus::Failed;
    // The level's child under the point, topmost first; empty when the point hits nothing.
    std::optional<document::LayerId> child;
    // The deepest leaf layer that hit inside `child` (equal to it for a leaf child).
    std::optional<document::LayerId> leaf;
    std::string diagnostic;
};

// Compiles one composition of a snapshot (the application passes its shared plan cache, so a
// revision the preview already compiled is not compiled again).
using ViewerPickCompile =
    std::function<SnapshotCompileResult(const SnapshotCompileRequest&, const CancellationToken&)>;

// The picking service. Thread-safe; one per Viewer controller. It keeps the outlines it resolved
// for the last frame (snapshot, composition and time), so further clicks on the same frame reuse
// that geometry instead of evaluating it again.
class ViewerPicker final {
  public:
    struct Statistics final {
        // Value-graph evaluations performed, picks answered from the frame's kept outlines, and
        // compiles a pick asked for (zero while the displayed frame's plan is current).
        std::uint64_t evaluations = 0;
        std::uint64_t reused = 0;
        std::uint64_t compiles = 0;
        // Picks run (any status).
        std::uint64_t picks = 0;
    };

    explicit ViewerPicker(ViewerPickCompile compile);

    [[nodiscard]] ViewerPickResult pick(const ViewerPickRequest& request,
                                        const CancellationToken& cancellation) const;
    [[nodiscard]] Statistics statistics() const;

    // A test seam: called at the start of every pick on the thread that runs it.
    void setPickObserverForTesting(std::function<void()> observer);

    // The kept geometry of one frame (defined in viewer_pick.cpp).
    struct FrameGeometry;

  private:
    ViewerPickCompile compile_;
    mutable std::mutex mutex_;
    mutable std::shared_ptr<const FrameGeometry> frame_;
    mutable Statistics statistics_;
    std::function<void()> observer_;
};

} // namespace bloom::runtime
