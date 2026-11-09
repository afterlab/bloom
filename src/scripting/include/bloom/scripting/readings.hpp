#pragma once

// Facts an agent reads before it edits, so it never has to render pixels to learn them (#390):
// the size of a piece of text, and the evaluated bounds of a layer. Qt-free; both are heavy
// enough to belong on a client thread, never the UI thread.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/text_metrics.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bloom::scripting {

// ---- Text metrics (implemented by the runtime module, which owns the render dependency)

using runtime::TextMetricsLine;
using runtime::TextMetricsRequest;
using runtime::TextMetricsResult;
using runtime::measureText;

// ---- Layer bounds

enum class LayerBoundsStatus : std::uint8_t {
    Measured,
    // The layer has no visible bounds at the time: outside its range, disabled, or empty.
    NotVisible,
    // The project cannot be evaluated here (no render source, no color settings, a composition
    // that does not compile); `diagnostic` says why.
    Unavailable,
    Cancelled,
};

struct LayerBoundsReading final {
    LayerBoundsStatus status = LayerBoundsStatus::Unavailable;
    std::string diagnostic;
    // The layer's content rectangle before any layer transform: pixel edges, origin at the top-left
    // of the layer's content (a solid's own size, a text block's laid-out box).
    double localLeft = 0.0;
    double localTop = 0.0;
    double localRight = 0.0;
    double localBottom = 0.0;
    // The content rectangle's four corners after the layer's transform (and its parents'), in
    // composition pixels: x to the right, y down, origin at the frame's top-left. Order: top-left,
    // top-right, bottom-right, bottom-left of the content.
    std::array<std::array<double, 2>, 4> corners{};
    // The axis-aligned box around those corners, in composition pixels. Not clipped to the frame.
    double boundsLeft = 0.0;
    double boundsTop = 0.0;
    double boundsRight = 0.0;
    double boundsBottom = 0.0;
    // The layer's anchor point, in composition pixels.
    double anchorX = 0.0;
    double anchorY = 0.0;
};

// Evaluates `composition` at `time` on the CPU reference path and reads one layer's bounds. Needs
// the context's render source (a live or headless session provides it).
[[nodiscard]] LayerBoundsReading measureLayerBounds(const OperationContext& context,
                                                    document::CompositionId composition,
                                                    core::RationalTime time,
                                                    document::LayerId layer);

} // namespace bloom::scripting
