#ifndef BLOOM_RUNTIME_SUBTITLES_FRAME_HPP
#define BLOOM_RUNTIME_SUBTITLES_FRAME_HPP

// Private to src/runtime. #752: the host preparation of a Subtitles node's frame
// (docs/architecture/subtitles.md, "Rendering"), shared by the CPU reference and the GPU scene
// builder so both draw exactly the same geometry. It selects the cues active at the operation's
// time, turns their markup into styled runs, lays them out (render/styled_text.hpp), places and
// stacks them in the safe area, and builds the box, outline and fill of each cue as path coverage
// rasters in drawing order. No pixel is produced here: the CPU fills each raster's coverage rows,
// the GPU rasterizes each raster's coverage geometry on the device, and both composite the layers
// source-over in this order.

#include "cpu_composition_evaluator_support.hpp"

#include <bloom/render/image.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/render/styled_text.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {

// The most cues one frame draws, and the most glyphs.
inline constexpr std::size_t kSubtitlesActiveCueLimit = 16;
inline constexpr std::size_t kSubtitlesGlyphLimit = 8192;

// One filled or stroked layer. `raster` is its author-space geometry rasterized at the frame's
// proxy scales: the node's own image (the leaf, read as an image or drawn on the integer grid).
// `paths` and `pathStroke` are that geometry itself, which a Layer Output redraws through its
// matrix so a scaled or rotated layer stays crisp (the vector leaf, as Text is).
struct SubtitlesLayer final {
    std::shared_ptr<const render::PathRaster> raster;
    std::shared_ptr<const std::vector<render::Path>> paths;
    render::PathStroke pathStroke;
    bool stroke = false;
    // The premultiplied process pixel, opacity and fade included.
    render::Rgba32f pixel = render::Rgba32f::transparent();
};

// Where one active cue was drawn.
struct SubtitlesPlacedCue final {
    std::size_t index = 0;
    std::uint64_t id = 0;
    // The cue's block (its box when the box is on), author pixels.
    render::PathBounds block;
    std::size_t lines = 0;
    // The size the cue was drawn at, after Shrink to fit.
    double size = 0.0;
    bool shrunk = false;
};

struct SubtitlesFrame final {
    std::vector<SubtitlesLayer> layers;
    // The proxy-pixel window the layers' ink covers, clipped to the frame; absent when nothing
    // draws (no active cue, Burn in off, or blank text).
    std::optional<render::ImageWindow> window;
    // The node's content bounds: the composition frame, author pixels.
    ContentBounds bounds;
    // The identity of everything the layers are made from: the active cues' content, the
    // resolved style, the fade phase, fonts, frame, safe area and proxy scales. Never the time or
    // the cue ids, so frames that show the same cues share it.
    std::string contentKey;
    std::vector<SubtitlesPlacedCue> cues;
    std::size_t glyphs = 0;
};

enum class SubtitlesFailureKind : std::uint8_t { Invalid, Budget, Cancelled };
struct SubtitlesFailure final {
    SubtitlesFailureKind kind = SubtitlesFailureKind::Invalid;
    std::string message;
};

// `frameWindow` is the composition's display window at the request's proxy scale.
[[nodiscard]] std::variant<SubtitlesFrame, SubtitlesFailure>
prepareSubtitlesFrame(const CompiledSubtitles& subtitles, const CompiledCompositionPlan& plan,
                      const ResolvedEvaluation& resolved, core::RationalTime time,
                      render::ImageWindow frameWindow, const CancellationToken& cancellation);

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_SUBTITLES_FRAME_HPP
