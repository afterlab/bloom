#pragma once

// #724 Scopes, section 8b: reading scopes from MCP and Python. One Qt-free read measures the
// frames of a composition through the same definitions, routes and CPU/GPU parity as the Scopes
// panel (runtime/scope_frame_measure.hpp) and returns the count tables, a summary computed from
// them, or a PNG of the scope drawn exactly as the panel draws it (runtime/scope_picture.hpp).
// MCP (`query` kind `scopes`) and Python (`bloom.scopes`) are two renderings of this one result.
//
// A read never uses an open Viewer's inspection settings: the Display signal goes through the
// color configuration's own default display and view unless the caller names one. Every blocking
// step (compile, evaluate, OCIO, device, drawing) runs as one task on the caller's scheduler, so
// nothing runs on an interface thread and the caller's cancellation reaches inside a frame.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/scope_picture.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/session.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::scripting {

// ---- Vocabulary (the names MCP and Python accept)

inline constexpr std::array<std::string_view, 5> kScopeNames{"waveform", "parade", "vectorscope",
                                                             "histogram", "chromaticity"};

enum class ScopeReadResolution : std::uint8_t { Full, Half, Quarter };
enum class ScopeReadOutput : std::uint8_t { Tables, Summary, Image };
enum class ScopeReadEncoding : std::uint8_t { Dense, Sparse };
// One of the five scopes, or all five stacked in the panel's order.
enum class ScopeReadImageKind : std::uint8_t {
    Waveform,
    Parade,
    Vectorscope,
    Histogram,
    Chromaticity,
    All
};

// Bounds of an image read, in pixels. 0 selects the default.
inline constexpr int kScopeImageMinimumSide = 128;
inline constexpr int kScopeImageMaximumWidth = 2048;
inline constexpr int kScopeImageMaximumHeight = 2048;
inline constexpr int kScopeImageDefaultWidth = 640;
inline constexpr std::size_t kScopeImageByteLimit = std::size_t{64} * 1024U * 1024U;
inline constexpr float kScopeBrightnessMinimum = 0.1F;
inline constexpr float kScopeBrightnessMaximum = 16.0F;
// The most frames one read covers.
inline constexpr std::uint64_t kScopeReadMaximumFrames = 1000;
// What a JSON rendering of one read may hold in table numbers (the MCP response limit counts the
// structured result twice).
inline constexpr std::size_t kScopeTablesJsonBudget = std::size_t{1700} * 1024U;

[[nodiscard]] std::string_view scopeImageKindName(ScopeReadImageKind kind) noexcept;
[[nodiscard]] std::optional<ScopeReadImageKind> scopeImageKindFromName(std::string_view name);
[[nodiscard]] std::string_view scopeResolutionName(ScopeReadResolution resolution) noexcept;
[[nodiscard]] std::optional<ScopeReadResolution> scopeResolutionFromName(std::string_view name);

// Scope names to the table bits the measure needs: `parade` uses the Waveform table. A name that is
// not one of kScopeNames sets `error` (naming the valid ones) and gives nothing.
[[nodiscard]] std::optional<std::uint32_t>
scopeTablesForNames(const std::vector<std::string>& names, std::string& error);
// The names of the scopes the table bits cover, in kScopeNames order (Waveform bits give both
// `waveform` and `parade`).
[[nodiscard]] std::vector<std::string> scopeNamesForTables(std::uint32_t scopes);

// ---- The request

struct ScopeReadDrawing final {
    runtime::scopes::WaveformChannels waveformChannels = runtime::scopes::WaveformChannels::Rgb;
    runtime::scopes::HistogramChannels histogramChannels = runtime::scopes::HistogramChannels::Rgb;
    runtime::scopes::HistogramScale histogramScale = runtime::scopes::HistogramScale::Linear;
    float brightness = 1.0F;
};

struct ScopeReadRequest final {
    document::CompositionId composition;
    // The first frame and the last (inclusive); equal for one frame.
    std::uint64_t firstFrame = 0;
    std::uint64_t lastFrame = 0;
    // render::kScopeTable* bits measured for the Tables and Summary outputs. An Image output
    // measures what its picture shows.
    std::uint32_t scopes = render::kScopeTableAll;
    runtime::ScopeSignal signal = runtime::ScopeSignal::Display;
    std::uint32_t axisMaximum = 1;
    std::optional<runtime::ScopeRegion> region;
    bool ignoreTransparent = false;
    ScopeReadResolution resolution = ScopeReadResolution::Full;
    // The Display signal's display and view; empty selects the color configuration's own.
    std::string display;
    std::string view;
    ScopeReadOutput output = ScopeReadOutput::Tables;
    ScopeReadEncoding encoding = ScopeReadEncoding::Dense;
    // Image output.
    ScopeReadImageKind image = ScopeReadImageKind::All;
    int width = 0;
    int height = 0;
    ScopeReadDrawing drawing;
};

// ---- Ports

// What a read asks the application's preview cache for: the frame's sidecar, under exactly what
// this read would measure. The application answers only when the sidecar of that frame was measured
// under the same frame identity and the same measure settings.
struct ScopeWarmQuery final {
    document::CompositionId composition;
    core::RationalTime time;
    runtime::EvaluationResolution resolution;
    runtime::EvaluationQuality quality = runtime::EvaluationQuality::Reference;
    runtime::EvaluationColorIntent colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
    // The display and view that will be measured (the resolved pair).
    std::string display;
    std::string view;
    // True when the caller named neither: the Viewer's default pair of the configuration.
    bool defaultDisplayView = true;
    runtime::ScopeMeasureSettings settings;
};

struct ScopeWarmHit final {
    runtime::ScopeTables tables;
    runtime::ScopeMeasuredArea area;
    // Why the CPU reference counted a frame of the device route; empty otherwise.
    std::string note;
};

struct ScopeReadPorts final {
    // The scheduler the read's one task runs on. It must outlive the call. Required.
    runtime::TaskScheduler* scheduler = nullptr;
    // Polled by the calling thread while the task runs; true cancels it, inside a frame too.
    std::function<bool()> cancelled;
    // Called on the calling thread after each finished frame with (frames done, frames total).
    std::function<void(std::uint64_t done, std::uint64_t total)> progress;
    // Where a frame is measured. `evaluateCpu` may stay empty (the read then evaluates with its own
    // CPU reference evaluator). Without a device (empty `device`) every frame is counted by the CPU
    // reference.
    runtime::ScopeFrameRoutes routes;
    // The warm sidecars of a desktop application's RAM preview; empty in a headless session.
    std::function<std::optional<ScopeWarmHit>(const ScopeWarmQuery&)> warm;
};

// The routes of a headless session: the provider's final-render evaluator and OCIO context when it
// has them, nothing otherwise. Blocks (polling `cancelled`) until the provider's bootstrap ends;
// never call it on an interface thread.
[[nodiscard]] runtime::ScopeFrameRoutes
headlessScopeRoutes(const std::shared_ptr<host::GpuExportProvider>& provider,
                    runtime::TaskScheduler& scheduler, const std::function<bool()>& cancelled);

// ---- The result

enum class ScopeReadStatus : std::uint8_t {
    Ok,
    // An argument the read cannot honor; `message` says which and what to do instead.
    Invalid,
    // The project cannot be evaluated here (no color settings, a composition that does not compile,
    // a frame that fails to evaluate).
    Failed,
    Cancelled,
};

struct ScopeReadError final {
    ScopeReadStatus status = ScopeReadStatus::Ok;
    // The argument at fault (Invalid), else empty.
    std::string argument;
    std::string message;
};

struct ScopeReadColor final {
    runtime::ScopeLuma luma;
    // The CIE primaries and white of the signal's color space, and its name in the configuration.
    std::array<double, 2> red{};
    std::array<double, 2> green{};
    std::array<double, 2> blue{};
    std::array<double, 2> white{};
    std::string colorSpace;
    // The display and view that were measured (Display signal), else empty.
    std::string display;
    std::string view;
};

struct ScopeReadFrame final {
    std::uint64_t frame = 0;
    core::RationalTime time;
    // The measured area is empty (a region outside the frame): nothing was counted.
    bool empty = false;
    bool fromCache = false;
    runtime::ScopeRoute route = runtime::ScopeRoute::Cpu;
    // Why the CPU reference counted a frame of the device route, or why no device was used; empty
    // when the device counted it.
    std::string routeNote;
    runtime::ScopeMeasuredArea area;
    // The pixels counted (0 for an empty area).
    std::uint32_t counted = 0;
    // Tables output: the unpacked tables (the layout says which are present).
    runtime::ScopeCounts counts;
    std::optional<runtime::ScopeSummary> summary;
    // Image output: the PNG and its size.
    std::vector<std::uint8_t> png;
    int imageWidth = 0;
    int imageHeight = 0;
};

struct ScopeReadResult final {
    ScopeReadError error;
    // What was measured, after defaults: the request with the axis maximum, display and view
    // resolved, and the composition's size.
    ScopeReadRequest settings;
    std::uint32_t compositionWidth = 0;
    std::uint32_t compositionHeight = 0;
    std::uint64_t revision = 0;
    ScopeReadColor color;
    // One per frame finished; on Cancelled the frames before the cancel.
    std::vector<ScopeReadFrame> frames;

    [[nodiscard]] bool ok() const noexcept { return error.status == ScopeReadStatus::Ok; }
};

// Reads the scopes of `request.firstFrame` .. `request.lastFrame` of the session's composition.
// Blocks on the calling thread (never an interface thread) while the task runs on the scheduler.
[[nodiscard]] ScopeReadResult readScopes(const Session& session, ScopeReadRequest request,
                                         const ScopeReadPorts& ports);

// The frame nearest to `time` of the composition; nothing for an unknown composition.
[[nodiscard]] std::optional<std::uint64_t> scopeFrameForTime(const document::Snapshot& snapshot,
                                                             document::CompositionId composition,
                                                             core::RationalTime time);

// ---- The tables as flat arrays

// One table as it is returned: `values` is every cell in the order `order` says (Dense) or
// [flat index, count] pairs of the non-zero cells in that order (Sparse). `under` and `over` are
// the clip counters of the Histogram (per channel) and the Waveform (per channel and column).
struct ScopeTableData final {
    std::vector<std::uint32_t> shape;
    std::string order;
    std::vector<std::uint32_t> values;
    std::vector<std::uint32_t> underOverShape;
    std::vector<std::uint32_t> under;
    std::vector<std::uint32_t> over;
};

struct ScopeTablesData final {
    std::optional<ScopeTableData> histogram;
    std::optional<ScopeTableData> waveform;
    std::optional<ScopeTableData> vectorscope;
    std::optional<ScopeTableData> chromaticity;
    // The bytes the numbers take as JSON text (digits and separators), for a response limit.
    std::size_t jsonBytes = 0;
};

[[nodiscard]] ScopeTablesData describeScopeTables(const runtime::ScopeCounts& counts,
                                                  ScopeReadEncoding encoding);

} // namespace bloom::scripting
