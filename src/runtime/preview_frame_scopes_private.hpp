#pragma once

// Private to the runtime (#730): the device route's prepared scope measure of one frame, which the
// preview display service copies into the frame's job and runs on its owner thread.

#include "gpu_process_frame_scope_private.hpp"

#include <bloom/runtime/preview_frame_scopes.hpp>

#include <memory>

namespace bloom::runtime {

class PreparedFrameScopeMeasure final {
  public:
    ScopeMeasureSettings settings;
    // The table size the request asked for (`work.job.reduction` is the size counted).
    render::ScopeTableReduction reduction{};
    std::shared_ptr<const ScopeSignalColor> color;
    // The job, the measured area and the color programs; the owner fills a copy's counts.
    detail::ScopeMeasureWork work;
};

} // namespace bloom::runtime
