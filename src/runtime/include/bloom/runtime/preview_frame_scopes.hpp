#pragma once

// #730 Scopes: the scope tables a preview frame carries. docs/architecture/scopes.md is the
// specification.
//
// A preview request may ask its frame's preparation to count the scope tables too
// (PreviewRequestIdentity::scopes). The frame's own route counts them, once, while it prepares the
// frame: the preview display service's frame job on the device (the display and XYZ color steps
// and the count kernel run on the scene picture the job computed, between its display step and its
// final transfer), or the CPU route on the worker that prepared the frame (measureScopeFrameCpu of
// the frame's process picture). The tables then travel with the frame
// (PreparedPreviewFrame::scopes) into the preview frame cache, where they are charged to its byte
// budget and matched by the frame's cache key plus the scope settings.

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bloom::runtime {

struct GpuSceneOcioContext;
class ProcessFrame;
class PreparedGpuScene;
class CompiledCompositionPlan;
struct EvaluationRequest;
struct PreviewRequestIdentity;

// The color objects of the last few signals a session measured (the config, display and view of
// a frame, and the signal). Any thread; preparing one is blocking (OCIO), so colorFor is called on
// a worker, never the interface thread.
class ScopeSignalColorCache final {
  public:
    // Null, with `message`, when the frame's color config, display or view cannot give the
    // signal.
    [[nodiscard]] std::shared_ptr<const ScopeSignalColor>
    colorFor(const PreviewRequestIdentity& frame, ScopeSignal signal, std::string& message);

  private:
    struct Entry final {
        std::string key;
        std::shared_ptr<const ScopeSignalColor> color;
    };
    std::mutex mutex_;
    std::vector<Entry> entries_; // most recently used last
};

// What a preview request asks its frame's preparation to count.
struct PreviewScopeRequest final {
    ScopeMeasureSettings settings;
    std::shared_ptr<ScopeSignalColorCache> colors;
    // The shared OCIO context the device's color steps are prepared with; empty or null means the
    // device route cannot count the tables, and the frame takes the CPU route.
    std::function<std::shared_ptr<const GpuSceneOcioContext>()> ocio;
};

enum class PreviewFrameScopesStatus : std::uint8_t {
    // `tables` holds the counts of `area`.
    Measured,
    // The region lies outside the picture: nothing to count, the scopes draw an empty graticule.
    EmptyArea,
    // The frame's route could not count the tables (`message` says why); the scopes draw an
    // empty graticule for this frame. The record keeps the frame from being prepared again in a
    // loop.
    Failed,
};

// The scope tables of one frame, immutable once published.
struct PreviewFrameScopes final {
    ScopeMeasureSettings settings;
    PreviewFrameScopesStatus status = PreviewFrameScopesStatus::Failed;
    ScopeTables tables; // packed; names the route that counted
    ScopeMeasuredArea area;
    // The signal's color objects: the luma coefficients and the colorimetry the graticules draw.
    std::shared_ptr<const ScopeSignalColor> color;
    std::string message;
    // The bytes the device read back for these tables (0 on the CPU route).
    std::uint64_t tableReadbackBytes = 0;

    [[nodiscard]] std::size_t byteSize() const noexcept {
        return sizeof(PreviewFrameScopes) + tables.byteSize() + message.size();
    }
};

// The record of a measure: Measured with the packed tables, EmptyArea, or Failed with the reason
// (also for Unavailable and Cancelled).
[[nodiscard]] std::shared_ptr<const PreviewFrameScopes>
makePreviewFrameScopes(const ScopeMeasureSettings& settings, ScopeFrameMeasure measure,
                       std::shared_ptr<const ScopeSignalColor> color);

// The CPU route: counts the frame's process picture on the calling worker. Null when cancelled.
[[nodiscard]] std::shared_ptr<const PreviewFrameScopes>
measurePreviewFrameScopesCpu(const PreviewScopeRequest& request,
                             const PreviewRequestIdentity& identity, const ProcessFrame& frame,
                             const CancellationToken& cancellation);

// The device route's preparation, on the worker that prepared the frame's GPU scene: the measured
// area and the color programs for the scene's picture. The preview display service runs it on its
// device as part of the frame's job (gpu_preview_display_service_resident_jobs.cpp).
class PreparedFrameScopeMeasure;

struct FrameScopeMeasurePreparation final {
    // Set when the device has tables to count.
    std::shared_ptr<const PreparedFrameScopeMeasure> prepared;
    // Set when nothing is left to count (an empty area): the frame's tables as they are.
    std::shared_ptr<const PreviewFrameScopes> settled;
    // Neither set: the device route cannot count this frame's tables; `message` says why and the
    // frame takes the CPU route, which counts them.
    std::string message;
    bool cancelled = false;
};

[[nodiscard]] FrameScopeMeasurePreparation
prepareFrameScopeMeasure(const PreviewScopeRequest& request, const PreviewRequestIdentity& identity,
                         const PreparedGpuScene& scene, const CompiledCompositionPlan& plan,
                         const EvaluationRequest& evaluation,
                         const CancellationToken& cancellation);

} // namespace bloom::runtime
