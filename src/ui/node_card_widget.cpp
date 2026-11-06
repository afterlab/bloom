#include <bloom/ui/node_card_widget.hpp>

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/vignette_nodes.hpp>
#include <bloom/ui/colorize_widget.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/depth_effect_widget.hpp>
#include <bloom/ui/lens_flare_widget.hpp>
#include <bloom/ui/film_grain_widget.hpp>
#include <bloom/ui/grade_wheels.hpp>
#include <bloom/ui/hue_saturation_widget.hpp>
#include <bloom/ui/levels_graph.hpp>
#include <bloom/ui/motion_blur_widget.hpp>
#include <bloom/ui/node_controls_widget.hpp>
#include <bloom/ui/scalar_slider_widget.hpp>
#include <bloom/ui/vignette_widget.hpp>

namespace bloom::ui {

bool hasNodeCardWidget(const std::string_view typeId) noexcept {
    return typeId == document::kGradeNodeType || typeId == document::kLevelsNodeType ||
           typeId == document::kCurvesNodeType || typeId == document::kHueSaturationNodeType ||
           typeId == document::kColorizeNodeType || typeId == document::kVignetteNodeType ||
           document::isDepthEffectNodeType(typeId) || typeId == document::kLensFlareNodeType ||
           typeId == document::kFilmGrainNodeType ||
           document::isMotionBlurNodeType(typeId) ||
           isScalarSliderNodeType(typeId) ||
           hasNodeControlsWidget(typeId);
}

NodeCardWidget* makeNodeCardWidget(CompositionSession& session, const document::NodeId node,
                                   const std::string_view typeId, const bool card,
                                   QWidget* parent) {
    if (typeId == document::kGradeNodeType)
        return new GradeCardWidget(session, node, card, parent);
    if (typeId == document::kLevelsNodeType)
        return new LevelsCardWidget(session, node, card, parent);
    if (typeId == document::kCurvesNodeType)
        return new CurvesCardWidget(session, node, card, parent);
    if (typeId == document::kHueSaturationNodeType)
        return new HueSaturationCardWidget(session, node, card, parent);
    if (typeId == document::kColorizeNodeType)
        return new ColorizeCardWidget(session, node, card, parent);
    if (typeId == document::kVignetteNodeType)
        return new VignetteCardWidget(session, node, card, parent);
    if (typeId == document::kLensFlareNodeType)
        return new LensFlareCardWidget(session, node, card, parent);
    if (typeId == document::kFilmGrainNodeType)
        return new FilmGrainCardWidget(session, node, card, parent);
    if (document::isDepthEffectNodeType(typeId))
        return new DepthEffectCardWidget(session, node, card, parent);
    if (document::isMotionBlurNodeType(typeId))
        return new MotionBlurCardWidget(session, node, typeId, card, parent);
    if (isScalarSliderNodeType(typeId))
        return new ScalarSliderCardWidget(session, node, typeId, card, parent);
    // #541: Properties draws a layer's rotation in its own Transform section (with the same dial),
    // so only the node card takes the Layer node's widget.
    if (typeId == "bloom.layer-output" && !card)
        return nullptr;
    if (hasNodeControlsWidget(typeId))
        return new NodeControlsCardWidget(session, node, typeId, card, parent);
    return nullptr;
}

} // namespace bloom::ui
