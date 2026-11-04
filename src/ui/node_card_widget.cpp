#include <bloom/ui/node_card_widget.hpp>

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/grade_wheels.hpp>
#include <bloom/ui/levels_graph.hpp>

namespace bloom::ui {

bool hasNodeCardWidget(const std::string_view typeId) noexcept {
    return typeId == document::kGradeNodeType || typeId == document::kLevelsNodeType ||
           typeId == document::kCurvesNodeType;
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
    return nullptr;
}

} // namespace bloom::ui
