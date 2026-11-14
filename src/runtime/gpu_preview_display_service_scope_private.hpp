#pragma once

// The scope tables of a preview frame, counted on the service's device as part of the frame's own
// job (#730, docs/architecture/scopes.md).
//
// A frame whose request asks for scope tables carries a measure prepared on the worker that
// prepared its GPU scene (PreviewGpuSceneStage::scopeMeasure). When the frame's display step is
// Ready, its job counts the tables on the scene picture the job computed (the display step
// consumed it; the job keeps an alias): the display color step (Display signal), the XYZ color
// step (Chromaticity table) and the count kernel with its one table readback, one native
// submission at a time, polled by the owner and never waited on. Only then does the frame's final
// transfer begin, and the frame is published carrying its tables. No scene runs again, no picture
// is read back, and nothing of the measure outlives the frame's job.

#include "gpu_process_frame_scope_private.hpp"

#include <bloom/runtime/preview_frame_scopes.hpp>

#include <atomic>
#include <cstdint>
#include <memory>

namespace bloom::runtime::detail {

struct PreviewDisplayServiceCore;
struct PreviewDisplayStageRecord;

// The service's scope state: the owner-thread native stage (color step executor and count
// kernel), created on first use, and the counters.
struct ScopeJobState final {
    std::unique_ptr<ScopeMeasureStage> stage;
    std::atomic<std::uint64_t> framesMeasured{0};
    std::atomic<std::uint64_t> framesFailed{0};
    std::atomic<std::uint64_t> colorDispatches{0};
    std::atomic<std::uint64_t> tableReadbacks{0};
    std::atomic<std::uint64_t> tableReadbackBytes{0};
    std::atomic<std::uint64_t> tableReadbackBytesTotal{0};
};

// Whether the stage's frame counts scope tables on the device.
[[nodiscard]] bool scopeMeasureWanted(const PreviewDisplayStageRecord& stage) noexcept;

// Owner thread, after the frame's display step is Ready. Starts the frame's measure; false when
// it has nothing to run on the device (the frame's tables are then settled in `stage.scopes`).
[[nodiscard]] bool beginFrameScopeMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                          PreviewDisplayStageRecord& stage) noexcept;

// Owner thread. One non-blocking turn of the frame's measure. True once it settled with nothing
// of it in flight: `stage.scopes` then holds the frame's tables (or why there are none).
[[nodiscard]] bool stepFrameScopeMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                         PreviewDisplayStageRecord& stage, bool cancelled,
                                         bool deadlineExpired) noexcept;

// Owner thread, when the service is idle: the native stage is released when nothing of it is in
// flight; the next measure creates it again.
void releaseScopeStageIdle(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread: up to `bytes` of the stage's retained color programs; the bytes released.
[[nodiscard]] std::uint64_t
reclaimScopePrograms(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                     std::uint64_t bytes) noexcept;

// Owner thread, at teardown: the native stage goes before the device.
void retireScopeNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

} // namespace bloom::runtime::detail
