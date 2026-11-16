#pragma once

// #733 Scopes: the Display signal tables of the frame the Viewer shows, counted at display time.
// docs/architecture/scopes.md is the specification.
//
// The Display signal of a shown frame is the frame's stored display cells themselves (4 bytes a
// pixel, render/display_pixel_packing.hpp), so its tables need no scene and no display transform:
// they are counted from the frame the Viewer already holds, for that frame only, and no frame
// stores them. A panel that opens after a RAM Preview is cached, and a change of region, table
// set or Ignore Transparent Pixels, count the shown frame again and render nothing.
//
// ShownFrameScopeMeasurer is the one call the Scopes panel and the headless, MCP and Python
// routes share. show() never blocks and may be called from any thread, the interface thread
// included: the measure runs on a scheduler worker and, when the preview display service has a
// device, as a job of that device (one transient upload of the cells, their unpacking, the CIE
// XYZ color step only with the Chromaticity table, the count kernel and one table readback).
// One measure is in flight at a time and the newest waiting request replaces an older one, so
// during playback the scopes follow at the rate the route can count while playback never waits,
// and the frame shown when playback stops is always counted. A frame the device does not take
// (frames are using it, or it has no room) is counted on the worker by the CPU reference, which
// gives the same tables and is labeled Cpu.

#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/preview_frame_scopes.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/task_types.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime {

class PreparedPreviewFrame;
class TaskScheduler;
struct GpuSceneOcioContext;

namespace detail {
struct ShownFrameScopeMeasurerState;
}

struct ShownFrameScopeRequest final {
    // The frame the Viewer shows. Its display, view and color config (the color objects) and its
    // region of interest are its own request's.
    std::shared_ptr<const PreparedPreviewFrame> frame;
    // The frame's composition, in pixels: `settings.region` is in these. Read with `frame`.
    std::uint32_t compositionWidth = 0;
    std::uint32_t compositionHeight = 0;
    // Instead of `frame`, a stored display frame no Viewer prepared (a headless read's): its
    // cells, the object that keeps them alive and the color objects of its Display signal.
    std::optional<ScopeStoredFrame> stored;
    std::shared_ptr<const void> storedOwner;
    std::shared_ptr<const ScopeSignalColor> storedColor;
    // `signal` must be ScopeSignal::Display and `axisMaximum` 1.
    ScopeMeasureSettings settings;
    // The size of the tables: what the panes draw (render/scope_table_layout.hpp).
    render::ScopeTableReduction reduction{};
    // The caller's own mark, handed back with the result.
    std::uint64_t tag = 0;
};

// The tables of one shown frame.
struct ShownFrameScopes final {
    std::shared_ptr<const PreparedPreviewFrame> frame;
    std::uint64_t tag = 0;
    render::ScopeTableReduction reduction{};
    // Never null: Measured with the tables (their layout carries the reduction), EmptyArea, or
    // Failed with the reason. `scopes->tables.route()` names the route that counted.
    std::shared_ptr<const PreviewFrameScopes> scopes;
    // The native work of a device measure (all 0 on the CPU route): frame uploads, color program
    // dispatches and table readbacks. No picture is read back on either route.
    std::uint64_t uploads = 0;
    std::uint64_t colorDispatches = 0;
    std::uint64_t tableReadbacks = 0;
    // Why the device did not count a frame the CPU reference counted; empty otherwise.
    std::string deviceMessage;
};

struct ShownFrameScopeCounters final {
    std::uint64_t requested = 0;
    // Waiting requests a newer one took the place of.
    std::uint64_t replaced = 0;
    // Results published, by the route that counted, and requests that ended with no tables.
    std::uint64_t deviceMeasures = 0;
    std::uint64_t cpuMeasures = 0;
    std::uint64_t unmeasured = 0;
    // Measures the device did not take or had no room for.
    std::uint64_t deviceDrops = 0;

    friend bool operator==(const ShownFrameScopeCounters&,
                           const ShownFrameScopeCounters&) = default;
};

class ShownFrameScopeMeasurer final {
  public:
    struct Options final {
        // Must outlive the measurer.
        TaskScheduler* scheduler = nullptr;
        TaskOwner owner{};
        std::shared_ptr<ScopeSignalColorCache> colors;
        // The shared OCIO context the device's CIE XYZ step is prepared with; empty or null and
        // a request with the Chromaticity table is counted by the CPU reference.
        std::function<std::shared_ptr<const GpuSceneOcioContext>()> ocio;
        // The preview display service's device; an empty port counts every frame on a worker.
        GpuShownFrameScopePort device;
        // Called, from a worker, when a result waits in take(). It must not block and must not
        // destroy the measurer; it is never called after the destructor returned.
        std::function<void()> resultReady;
        // Test seam: called on the worker before the CPU reference counts a frame.
        std::function<void()> beforeCpuCount;
    };

    explicit ShownFrameScopeMeasurer(Options options);
    ~ShownFrameScopeMeasurer();
    ShownFrameScopeMeasurer(const ShownFrameScopeMeasurer&) = delete;
    ShownFrameScopeMeasurer& operator=(const ShownFrameScopeMeasurer&) = delete;

    // Any thread; never blocks. Asks for the tables of `request.frame`. A request still waiting
    // is replaced.
    void show(ShownFrameScopeRequest request);
    // Any thread. Nothing is shown: the waiting request is dropped, the measure in flight is
    // cancelled where it can be, and its result and any result not yet taken are discarded.
    void clear();
    // Any thread. The device a later measure is posted to.
    void setDevice(GpuShownFrameScopePort device);
    // The newest result not yet taken.
    [[nodiscard]] std::optional<ShownFrameScopes> take();
    // True while a request waits or a measure is in flight.
    [[nodiscard]] bool busy() const;
    [[nodiscard]] ShownFrameScopeCounters counters() const;

  private:
    // Shared with the measures in flight, which may outlive the measurer.
    std::shared_ptr<detail::ShownFrameScopeMeasurerState> state_;
};

} // namespace bloom::runtime
