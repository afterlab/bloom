#pragma once

// #509 Stroke a layer's outline in place. A Text Outline or Mask Path answers its path in the space
// the text or the masks are drawn in, the source layer's own space, so a Shape drawing it lands on
// the letters or masks only when the Shape's layer maps that space the way the source layer does.
// This operation makes that setup in one undo step, with nothing left for the artist to line up:
//
//   Text Outline (or Mask Path) -> Path Stroke -> Shape Source -> a new Merge entry directly above
//   the source layer, whose own layer is the identity (pivot Frame at its defaults) and is
//   parented to the source layer.
//
// A child layer at the identity draws its content in its parent's own space through the parent's
// whole transform (docs/architecture/layer-graph-model.md, "Transform Parenting"), so the stroke
// stays on the letters or masks whatever the source layer's position, anchor, scale, rotation or
// parents are, keyed or not. The new layer keeps the source layer's in and out points.

#include <bloom/commands/operation.hpp>
#include <bloom/document/ids.hpp>

#include <cstdint>
#include <string_view>

namespace bloom::document {
class Composition;
}

namespace bloom::commands {

// What is stroked: the letters of the Text Source the layer shows, or the layer's path masks.
enum class OutlineStrokeSource : std::uint8_t { Text, Masks };

inline constexpr std::string_view kOutlineStrokeReaderOutput = "reader";  // Text Outline / Mask Path
inline constexpr std::string_view kOutlineStrokeStrokeOutput = "stroke";  // Path Stroke
inline constexpr std::string_view kOutlineStrokeShapeOutput = "shape";    // Shape Source
inline constexpr std::string_view kOutlineStrokeLayerOutput = "layer";    // the new layer (LayerId)

class StrokeLayerOutline final : public Operation {
  public:
    StrokeLayerOutline(document::CompositionId composition, document::LayerId layer,
                       OutlineStrokeSource source)
        : composition_(composition), layer_(layer), source_(source) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    OutlineStrokeSource source_;
};

// Whether `layer` has what StrokeLayerOutline needs for `source` (the menu's enabled state): a Text
// Source on its image chain, or a path mask.
[[nodiscard]] bool canStrokeLayerOutline(const document::Composition& composition,
                                         document::LayerId layer, OutlineStrokeSource source);

} // namespace bloom::commands
