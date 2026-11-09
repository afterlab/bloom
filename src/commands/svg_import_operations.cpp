#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/media/svg_scene.hpp>

#include "node_operation_support.hpp"

#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/document/gradient_interpolation.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <array>
#include <numbers>
#include <unordered_map>

namespace bloom::commands {
namespace {

using document::ParameterValue;
using media::SvgAffine;
using media::SvgElement;
using media::SvgPaint;

struct Failure final {
    std::string message;
};

double decodeSrgb(const double value) noexcept {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}
core::Color4d linearColor(const core::Color4d& srgb) noexcept {
    return {decodeSrgb(srgb.red), decodeSrgb(srgb.green), decodeSrgb(srgb.blue),
            std::clamp(srgb.alpha, 0.0, 1.0)};
}

// A name the document accepts: nonempty, valid UTF-8, bounded.
std::string cleanName(std::string name, const std::string& fallback) {
    // Cut at a UTF-8 boundary and drop control characters.
    std::string out;
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t')
            continue;
        out.push_back(c);
    }
    constexpr std::size_t kLimit = 200;
    if (out.size() > kLimit) {
        out.resize(kLimit);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0U) == 0x80U)
            out.pop_back();
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80U) != 0U)
            out.pop_back();
    }
    if (!document::isValidHumanFacingName(out))
        return fallback;
    return out;
}

struct Slot final {
    document::LayerSlotId slot;
    document::LayerId layer;
};

class Importer final {
  public:
    Importer(document::Draft& draft, document::Composition& composition,
             const document::CompositionId compositionId, const media::SvgScene& scene, const bool fit)
        : draft_(draft), composition_(composition), compositionId_(compositionId), graph_(composition.graph()),
          parameters_(composition.parameters()), registry_(document::builtInNodeDefinitions()), scene_(scene) {
        const double compositionWidth = composition.format().width(), compositionHeight = composition.format().height();
        frameCenter_ = {compositionWidth / 2.0, compositionHeight / 2.0};
        compositionWidth_ = compositionWidth;
        compositionHeight_ = compositionHeight;
        if (fit) {
            const double scale = std::min(compositionWidth / scene.width, compositionHeight / scene.height);
            fit_ = {scale, 0, 0, scale, (compositionWidth - scene.width * scale) / 2.0,
                    (compositionHeight - scene.height * scale) / 2.0};
        } else {
            fit_ = {1, 0, 0, 1, (compositionWidth - scene.width) / 2.0, (compositionHeight - scene.height) / 2.0};
        }
        // New cards go to the right of whatever is already on the canvas.
        double right = 0;
        for (const auto& [id, record] : composition.nodeLayout())
            right = std::max(right, record.position.x + record.width);
        layoutOriginX_ = right > 0 ? right + 400.0 : 0.0;
    }

    OperationResult run(const std::string& name) {
        try {
            const auto top = graph_.layerStack().nodeId();
            const auto rootMerge = addNode(std::string(document::kLayerStackNodeType), {}, {layoutOriginX_, 0});
            const auto rootSlot = addSlot(top, rootMerge);
            Slot ensured = ensureEntry(top, rootSlot, cleanName(name, "SVG"));
            // What the file holds and this import does not take, each named once.
            for (const auto& skipped : scene_.skipped)
                note(skipped.name + ": not imported (" + skipped.reason + ")");
            if (scene_.shapeCount > 2000)
                note(std::to_string(scene_.shapeCount) +
                     " shapes became that many layers; every layer is a full-frame image when the "
                     "frame is drawn, so a file this dense can need more memory than the GPU path "
                     "admits at a large frame size (a smaller frame or render scale draws it)");
            emitGroupChildren(scene_.root, rootMerge, cleanName(name, "SVG"));
            finishFrames();
            if (graph_.findNode(rootMerge) == nullptr)
                throw Failure{"The SVG root could not be created"};
            std::vector<OperationOutput> outputs{{"layer", ensured.layer},
                                                  {"slot", rootSlot},
                                                  {"merge", rootMerge}};
            auto result = OperationResult::applied(std::move(outputs));
            for (auto& note : notes_) {
                if (result.issues.size() >= 400)
                    break;
                result.issues.push_back({OperationIssueCode::Unsupported, std::move(note)});
            }
            if (!omitted_.empty()) {
                std::string text;
                for (const auto& [reason, count] : omitted_)
                    text += (text.empty() ? "" : "; ") + std::to_string(count) + " more: " + reason;
                result.issues.push_back({OperationIssueCode::Unsupported, "Further items not listed by name: " + text});
            }
            return result;
        } catch (const Failure& failure) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue, failure.message);
        }
    }

  private:
    document::Draft& draft_;
    document::Composition& composition_;
    document::CompositionId compositionId_;
    document::CanonicalGraph& graph_;
    document::ParameterStore& parameters_;
    const document::NodeDefinitionRegistry& registry_;
    const media::SvgScene& scene_;
    SvgAffine fit_;
    document::Vec2d frameCenter_;
    double compositionWidth_ = 0, compositionHeight_ = 0;
    double layoutOriginX_ = 0;
    std::map<std::string, const document::NodeDefinition*> definitions_;
    std::vector<std::string> notes_;
    std::map<std::string, std::size_t> omitted_;
    // One frame per SVG group: the nodes its direct children created.

    void note(std::string text) {
        if (notes_.size() < 380)
            notes_.push_back(std::move(text));
        else
            ++omitted_[text.find(": ") == std::string::npos ? text : text.substr(text.find(": ") + 2)];
    }

    // ---- nodes ----

    const document::NodeDefinition& definitionOf(const std::string& type) {
        auto found = definitions_.find(type);
        if (found != definitions_.end())
            return *found->second;
        const document::NodeDefinition* best = nullptr;
        for (const auto& candidate : registry_.definitions())
            if (candidate.key.typeId == type &&
                (best == nullptr || candidate.key.schemaVersion > best->key.schemaVersion))
                best = &candidate;
        if (best == nullptr)
            throw Failure{"The node type " + type + " is not available"};
        definitions_[type] = best;
        return *best;
    }

    using Overrides = std::map<std::string, document::ParameterSource>;

    document::NodeId addNode(const std::string& type, const Overrides& overrides, const document::Vec2d position,
                             const bool collapsed = true) {
        const auto& definition = definitionOf(type);
        const auto nodeId = draft_.ids().allocateNode();
        if (!nodeId)
            throw Failure{"The node ID space is exhausted"};
        document::NodeRecord node{*nodeId, definition.key.typeId, {}, definition.key.schemaVersion};
        node.parameters.reserve(definition.parameters.size());
        for (const auto& parameter : definition.parameters) {
            if (parameter.schemaKey == document::kTimeRemapParameterSchemaKey)
                continue;
            const auto id = draft_.ids().allocateParameter();
            if (!id)
                throw Failure{"The parameter ID space is exhausted"};
            document::ParameterSource source = document::ConstantValueSource{parameter.defaultValue};
            if (const auto found = overrides.find(parameter.role); found != overrides.end())
                source = found->second;
            if (!parameters_.insert({*id, parameter.schemaKey, std::move(source)}))
                throw Failure{"A value for " + type + " (" + parameter.role + ") is outside what it accepts"};
            node.parameters.push_back({parameter.role, *id});
        }
        if (!graph_.addNode(std::move(node)))
            throw Failure{"A node could not be inserted"};
        composition_.nodeLayout()[*nodeId] = {position, collapsed ? 160.0 : 240.0, collapsed, false};
        return *nodeId;
    }
    static document::ParameterSource constant(ParameterValue value) {
        return document::ConstantValueSource{std::move(value)};
    }

    // A new slot on top of `merge`, fed by `source`'s image output.
    document::LayerSlotId addSlot(const document::NodeId merge, const document::NodeId source) {
        auto* stack = graph_.merge(merge);
        if (stack == nullptr)
            throw Failure{"A Merge could not be found"};
        const auto slotId = draft_.ids().allocateLayerSlot();
        const auto edgeId = draft_.ids().allocateEdge();
        if (!slotId || !edgeId)
            throw Failure{"The slot or edge ID space is exhausted"};
        const auto existingTop = stack->entries().empty() ? std::nullopt : std::optional(stack->entries().front().slotId);
        if (!stack->append({*slotId, {}}) || !stack->moveBefore(*slotId, existingTop))
            throw Failure{"A slot could not be added"};
        if (!graph_.addEdge({*edgeId, {source, std::string(document::kLayerStackOutputPort)},
                             document::LayerStackInputRef{merge, *slotId, std::string(document::kLayerStackContentInputRole)}},
                            registry_))
            throw Failure{"A connection could not be made"};
        return *slotId;
    }

    // Creates the entry layer of a slot and names it.
    Slot ensureEntry(const document::NodeId merge, const document::LayerSlotId slot, const std::string& name) {
        auto result = EnsureEntryLayer(compositionId_, merge, slot).apply(draft_);
        if (result.status == OperationStatus::Rejected)
            throw Failure{result.issues.empty() ? "An entry layer could not be created" : result.issues.front().message};
        document::LayerId layer;
        for (const auto& output : result.outputs)
            if (output.name == "layer")
                layer = std::get<document::LayerId>(output.id);
        auto* boundary = graph_.findLayer(layer);
        if (boundary == nullptr)
            throw Failure{"An entry layer could not be found"};
        boundary->name = cleanName(name, "Layer");
        boundary->nameFollowsSource = false;
        return {slot, layer};
    }

    document::LayerOutputBoundary& layerOf(const Slot slot) {
        auto* boundary = graph_.findLayer(slot.layer);
        if (boundary == nullptr)
            throw Failure{"An entry layer could not be found"};
        return *boundary;
    }
    void setLayerParameter(const Slot slot, const std::string_view role, ParameterValue value) {
        auto& boundary = layerOf(slot);
        for (const auto& binding : boundary.parameters)
            if (binding.role == role) {
                if (!parameters_.setSource(binding.parameterId, document::ConstantValueSource{std::move(value)}))
                    throw Failure{"A layer value is outside what it accepts"};
                return;
            }
    }

    // ---- values ----

    document::PathValue toPath(const std::vector<media::SvgContour>& contours, const bool evenOdd) const {
        document::PathValue value;
        value.fillRule = evenOdd ? document::PathFillRule::EvenOdd : document::PathFillRule::NonZero;
        const auto map = [&](const media::SvgPoint p) {
            const auto q = fit_.apply(p.x, p.y);
            return document::Vec2d{q[0], q[1]};
        };
        for (const auto& contour : contours) {
            document::PathContour out;
            out.closed = contour.closed;
            out.anchors.reserve(contour.anchors.size());
            for (const auto& anchor : contour.anchors) {
                document::PathAnchor a;
                a.point = map(anchor.point);
                if (anchor.hasIn)
                    a.inHandle = map(anchor.in);
                if (anchor.hasOut)
                    a.outHandle = map(anchor.out);
                a.mode = document::inferPathAnchorMode(a);
                out.anchors.push_back(std::move(a));
            }
            value.contours.push_back(std::move(out));
        }
        return value;
    }
    // The bounds of a shape's contours in the composition (control points).
    std::array<double, 4> worldBounds(const SvgElement& shape) const {
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        for (const auto& contour : shape.contours)
            for (const auto& anchor : contour.anchors) {
                const auto add = [&](const media::SvgPoint p) {
                    const auto q = fit_.apply(p.x, p.y);
                    minX = std::min(minX, q[0]);
                    minY = std::min(minY, q[1]);
                    maxX = std::max(maxX, q[0]);
                    maxY = std::max(maxY, q[1]);
                };
                add(anchor.point);
                if (anchor.hasIn)
                    add(anchor.in);
                if (anchor.hasOut)
                    add(anchor.out);
            }
        return {minX, minY, maxX, maxY};
    }
    double fitScale() const noexcept { return std::sqrt(std::fabs(fit_.determinant())); }

    // The gradient the way a Gradient node holds it: straight scene-linear stops. SVG blends in
    // encoded sRGB, so each span whose colours differ is sampled densely enough that Bloom's linear
    // blend between samples follows the encoded blend.
    document::GradientValue gradientValue(const media::SvgGradient& gradient) const {
        document::GradientValue value;
        const auto& stops = gradient.stops;
        const auto push = [&](const double position, const core::Color4d& srgb) {
            value.stops.push_back({std::clamp(position, 0.0, 1.0), linearColor(srgb)});
        };
        std::size_t budget = document::kMaximumGradientStops;
        const auto spans = stops.size() > 1 ? stops.size() - 1 : 0;
        std::size_t subdivisions = 8;
        while (subdivisions > 1 && stops.size() + spans * (subdivisions - 1) > budget)
            subdivisions /= 2;
        for (std::size_t i = 0; i < stops.size(); ++i) {
            push(stops[i].offset, stops[i].color);
            if (i + 1 >= stops.size())
                break;
            const auto& a = stops[i];
            const auto& b = stops[i + 1];
            const double width = b.offset - a.offset;
            const double difference =
                std::max({std::fabs(a.color.red - b.color.red), std::fabs(a.color.green - b.color.green),
                          std::fabs(a.color.blue - b.color.blue)});
            if (width <= 1e-9 || difference < 0.02)
                continue;
            for (std::size_t k = 1; k < subdivisions; ++k) {
                const double t = static_cast<double>(k) / static_cast<double>(subdivisions);
                core::Color4d mixed{a.color.red + (b.color.red - a.color.red) * t,
                                    a.color.green + (b.color.green - a.color.green) * t,
                                    a.color.blue + (b.color.blue - a.color.blue) * t,
                                    a.color.alpha + (b.color.alpha - a.color.alpha) * t};
                push(a.offset + width * t, mixed);
            }
        }
        if (value.stops.size() > document::kMaximumGradientStops)
            value.stops.resize(document::kMaximumGradientStops);
        return value;
    }

    // ---- layout ----

    // Cards of one group sit in a compact grid (six pairs a row) so a file with thousands of
    // elements opens as a few tidy blocks, collapsed, instead of one long column.
    static constexpr std::size_t kColumns = 6;
    static constexpr double kPitchX = 420.0, kPitchY = 84.0;
    double blockBaseY_ = 0;
    double maxY_ = 0;
    std::size_t cellIndex_ = 0;
    document::Vec2d cell() {
        const auto index = cellIndex_++;
        const document::Vec2d position{layoutOriginX_ + static_cast<double>(index % kColumns) * kPitchX,
                                       blockBaseY_ + static_cast<double>(index / kColumns) * kPitchY};
        maxY_ = std::max(maxY_, position.y + kPitchY);
        return position;
    }

    // ---- frames (one per group, over the nodes its direct children made) ----

    std::vector<std::set<document::NodeId>*> activeMembers_;
    std::vector<std::pair<std::string, std::set<document::NodeId>>> finishedFrames_;
    void claim(const document::NodeId node) {
        if (!activeMembers_.empty())
            activeMembers_.back()->insert(node);
    }
    void finishFrames() {
        for (auto& [name, members] : finishedFrames_) {
            if (members.empty())
                continue;
            const auto groupId = draft_.ids().allocateNodeGroup();
            if (!groupId)
                break;
            document::NodeGroupRecord record;
            record.id = *groupId;
            record.name = cleanName(name, "Group");
            record.members = std::move(members);
            composition_.nodeGroups()[*groupId] = std::move(record);
        }
    }

    // ---- emission ----

    static constexpr std::size_t kChunkSize = 256;

    // The children of a group, bottom first; every new slot goes on top of the stack, so the
    // last painted element ends topmost, as in the file.
    void emitGroupChildren(const SvgElement& group, const document::NodeId merge, const std::string& groupName) {
        std::set<document::NodeId> members;
        activeMembers_.push_back(&members);
        const auto savedBase = blockBaseY_;
        const auto savedIndex = cellIndex_;
        blockBaseY_ = maxY_ + (maxY_ > 0 ? 80.0 : 0.0);
        cellIndex_ = 0;
        if (group.children.size() <= kChunkSize) {
            for (const auto& child : group.children)
                emitElement(child, merge);
        } else {
            // Thousands of siblings are held as consecutive runs, each a nested Merge: a plain
            // composite of the runs is the same picture, and no Merge, node card or render pass
            // has to hold thousands of entries at once.
            for (std::size_t first = 0; first < group.children.size(); first += kChunkSize) {
                const auto last = std::min(first + kChunkSize, group.children.size());
                const auto runMerge = addNode(std::string(document::kLayerStackNodeType), {}, cell());
                claim(runMerge);
                const auto runSlot = addSlot(merge, runMerge);
                ensureEntry(merge, runSlot,
                            (groupName.empty() ? std::string("SVG") : groupName) + " " + std::to_string(first + 1) +
                                "-" + std::to_string(last));
                for (std::size_t index = first; index < last; ++index)
                    emitElement(group.children[index], runMerge);
            }
        }
        activeMembers_.pop_back();
        finishedFrames_.push_back({groupName.empty() ? std::string("SVG") : groupName, std::move(members)});
        // The parent resumes in a fresh block below everything laid out so far.
        (void)savedBase;
        (void)savedIndex;
        blockBaseY_ = maxY_ + 80.0;
        cellIndex_ = 0;
    }

    // A group that is only a wrapper: no name of its own, no opacity, clip or mask.
    static bool isAnonymous(const SvgElement& group) {
        return group.kind == SvgElement::Kind::Group && group.name == group.tag && group.opacity >= 1.0 &&
               !group.clip && !group.mask;
    }

    // One element: one slot of `merge` (topmost), and when it is clipped or masked its matte source
    // directly below it.
    void emitElement(const SvgElement& element, const document::NodeId merge) {
        for (const auto& approximation : element.approximations)
            note(element.name + ": " + approximation);
        if (isAnonymous(element)) {
            for (const auto& child : element.children)
                emitElement(child, merge);
            return;
        }
        std::optional<Slot> matte;
        if (element.clip || element.mask)
            matte = emitMatteSource(element, merge);
        const Slot target = element.kind == SvgElement::Kind::Group  ? emitGroup(element, merge)
                            : element.kind == SvgElement::Kind::Text ? emitText(element, merge)
                                                                      : emitShape(element, merge);
        if (matte)
            layerOf(target).trackMatte = document::TrackMatte{
                matte->layer, element.mask ? document::TrackMatteChannel::Luma : document::TrackMatteChannel::Alpha,
                false, std::nullopt};
    }

    Slot emitMatteSource(const SvgElement& element, const document::NodeId merge) {
        const bool isMask = static_cast<bool>(element.mask);
        const auto& source = isMask ? *element.mask : *element.clip;
        for (const auto& approximation : source.approximations)
            note(element.name + ": " + approximation);
        const auto sourceMerge = addNode(std::string(document::kLayerStackNodeType), {}, cell());
        claim(sourceMerge);
        const auto slot = addSlot(merge, sourceMerge);
        auto entry = ensureEntry(merge, slot, source.name);
        emitGroupChildren(source, sourceMerge, source.name);
        return entry;
    }

    Slot emitGroup(const SvgElement& element, const document::NodeId merge) {
        const auto groupMerge = addNode(std::string(document::kLayerStackNodeType), {}, cell());
        claim(groupMerge);
        const auto slot = addSlot(merge, groupMerge);
        auto entry = ensureEntry(merge, slot, element.name);
        if (element.opacity < 1.0)
            setLayerParameter(entry, document::kOpacityParameterRole, element.opacity);
        emitGroupChildren(element, groupMerge, element.name);
        return entry;
    }

    // Text stays text: a native Text layer the artist can retype, restyle and animate, set in
    // Bloom's font (the file's fonts are not read).
    Slot emitText(const SvgElement& element, const document::NodeId merge) {
        Overrides overrides;
        overrides[std::string(document::kTextParameterRole)] = constant(element.text);
        overrides[std::string(document::kTextSizeParameterRole)] = constant(std::max(1.0, element.fontSize * fitScale()));
        overrides[std::string(document::kTextColorParameterRole)] = constant(linearColor(element.fill.color));
        overrides[std::string(document::kTextAlignmentParameterRole)] = constant(static_cast<std::int64_t>(element.textAnchor));
        overrides[std::string(document::kTextAnchorModeParameterRole)] = constant(document::kTextAnchorModeFollowsAlignment);
        const auto position = cell();
        const auto node = addNode(std::string(document::kTextSourceNodeType), overrides, position);
        claim(node);
        const auto slot = addSlot(merge, node);
        auto entry = ensureEntry(merge, slot, element.name);
        const auto at = fit_.apply(element.textX, element.textY);
        setLayerParameter(entry, document::kPositionParameterRole, document::Vec2d{frameCenter_.x + at[0], frameCenter_.y + at[1] - kTextBaselineLift * element.fontSize * fitScale()});
        if (element.textRotation != 0.0)
            setLayerParameter(entry, document::kRotationParameterRole, element.textRotation);
        if (element.opacity < 1.0)
            setLayerParameter(entry, document::kOpacityParameterRole, element.opacity);
        return entry;
    }

    static constexpr double kTextBaselineLift = 0.928; // the ascent of the built-in face, in ems; an untranslated text layer has its line's top at the frame's top and its anchor at the frame's left edge

    Slot emitShape(const SvgElement& shape, const document::NodeId merge) {
        const auto position = cell();
        const auto pathNode = addNode(std::string(document::kPathValueNodeType),
                                      {{std::string(document::kValueParameterRole), constant(toPath(shape.contours, shape.evenOdd))}},
                                      position);
        claim(pathNode);
        const bool solidFill = shape.fill.kind == SvgPaint::Kind::Color;
        const bool gradientFill = shape.fill.kind == SvgPaint::Kind::Gradient;
        const bool hasStroke = shape.stroke.kind != SvgPaint::Kind::None;
        const bool nativeStroke = shape.stroke.kind == SvgPaint::Kind::Color && shape.dashes.empty();
        // One Shape holds the fill and the stroke when both are plain.
        if (!gradientFill && (!hasStroke || nativeStroke)) {
            const auto node = addShapeNode(pathNode, shape, solidFill, nativeStroke, {position.x + 180.0, position.y});
            const auto slot = addSlot(merge, node);
            auto entry = ensureEntry(merge, slot, shape.name);
            if (shape.opacity < 1.0)
                setLayerParameter(entry, document::kOpacityParameterRole, shape.opacity);
            return entry;
        }
        // Otherwise a small group, the fill and then the stroke over it.
        const auto groupMerge = addNode(std::string(document::kLayerStackNodeType), {}, cell());
        claim(groupMerge);
        const auto slot = addSlot(merge, groupMerge);
        auto entry = ensureEntry(merge, slot, shape.name);
        if (shape.opacity < 1.0)
            setLayerParameter(entry, document::kOpacityParameterRole, shape.opacity);
        if (gradientFill) {
            emitGradientPaint(shape.fill, pathNode, std::string(document::kValuePortName), groupMerge, "Fill", shape.evenOdd, shape);
        } else if (solidFill) {
            const auto node = addShapeNode(pathNode, shape, true, false, cell());
            (void)ensureEntry(groupMerge, addSlot(groupMerge, node), "Fill");
        }
        if (hasStroke) {
            if (nativeStroke) {
                const auto node = addShapeNode(pathNode, shape, false, true, cell());
                (void)ensureEntry(groupMerge, addSlot(groupMerge, node), "Stroke");
            } else {
                // The stroke as an outline (Path Stroke makes the dashes), filled with its paint.
                const auto outline = addPathStroke(pathNode, shape, cell());
                claim(outline);
                if (shape.stroke.kind == SvgPaint::Kind::Gradient)
                    emitGradientPaint(shape.stroke, outline, "path", groupMerge, "Stroke", false, shape);
                else {
                    const auto node = addOutlineShape(outline, shape.stroke.color, false, cell());
                    (void)ensureEntry(groupMerge, addSlot(groupMerge, node), "Stroke");
                }
            }
        }
        return entry;
    }

    // A Shape Source reading its outline from `pathNode`.
    document::NodeId addShapeNode(const document::NodeId pathNode, const SvgElement& shape, const bool fill,
                                  const bool stroke, const document::Vec2d position) {
        Overrides overrides;
        overrides["kind"] = constant(static_cast<std::int64_t>(document::ShapeKind::Path));
        overrides["path"] = document::DriverBindingSource{pathNode, std::string(document::kValuePortName)};
        overrides["fillEnabled"] = constant(fill);
        if (fill)
            overrides["fillColor"] = constant(linearColor(shape.fill.color));
        overrides["strokeEnabled"] = constant(stroke);
        if (stroke) {
            overrides["strokeColor"] = constant(linearColor(shape.stroke.color));
            overrides["strokeWidth"] = constant(std::max(0.0, shape.strokeWidth * fitScale()));
            overrides["strokeJoin"] = constant(static_cast<std::int64_t>(
                shape.join == media::SvgJoin::Round ? document::ShapeStrokeJoin::Round
                : shape.join == media::SvgJoin::Bevel ? document::ShapeStrokeJoin::Bevel
                                                      : document::ShapeStrokeJoin::Miter));
            overrides["strokeCap"] = constant(static_cast<std::int64_t>(
                shape.cap == media::SvgCap::Round ? document::ShapeStrokeCap::Round
                : shape.cap == media::SvgCap::Square ? document::ShapeStrokeCap::Square
                                                     : document::ShapeStrokeCap::Butt));
        }
        overrides["fillRule"] = constant(static_cast<std::int64_t>(shape.evenOdd ? 1 : 0));
        const auto node = addNode(std::string(document::kShapeSourceNodeType), overrides, position);
        claim(node);
        return node;
    }

    // A Shape Source that fills an outline given by another node's Path output.
    document::NodeId addOutlineShape(const document::NodeId outlineNode, const core::Color4d& srgb, const bool evenOdd,
                                     const document::Vec2d position) {
        Overrides overrides;
        overrides["kind"] = constant(static_cast<std::int64_t>(document::ShapeKind::Path));
        overrides["path"] = document::DriverBindingSource{outlineNode, "path"};
        overrides["fillEnabled"] = constant(true);
        overrides["fillColor"] = constant(linearColor(srgb));
        overrides["strokeEnabled"] = constant(false);
        overrides["fillRule"] = constant(static_cast<std::int64_t>(evenOdd ? 1 : 0));
        const auto node = addNode(std::string(document::kShapeSourceNodeType), overrides, position);
        claim(node);
        return node;
    }

    document::NodeId addPathStroke(const document::NodeId pathNode, const SvgElement& shape, const document::Vec2d position) {
        Overrides overrides;
        overrides["path"] = document::DriverBindingSource{pathNode, std::string(document::kValuePortName)};
        overrides["width"] = constant(std::max(0.001, shape.strokeWidth * fitScale()));
        if (!shape.dashes.empty()) {
            overrides["dashLength"] = constant(std::max(0.0, shape.dashes[0] * fitScale()));
            overrides["dashGap"] = constant(std::max(0.0, (shape.dashes.size() > 1 ? shape.dashes[1] : shape.dashes[0]) * fitScale()));
            overrides["dashOffset"] = constant(shape.dashOffset * fitScale());
            if (shape.dashes.size() > 2)
                note(shape.name + ": a dash pattern of " + std::to_string(shape.dashes.size()) +
                     " lengths keeps its first dash and gap");
        }
        overrides["cap"] = constant(static_cast<std::int64_t>(shape.cap == media::SvgCap::Round ? 1 : shape.cap == media::SvgCap::Square ? 2 : 0));
        overrides["join"] = constant(static_cast<std::int64_t>(shape.join == media::SvgJoin::Round ? 1 : shape.join == media::SvgJoin::Bevel ? 2 : 0));
        const auto node = addNode(std::string(document::kPathStrokeNodeType), overrides, position);
        return node;
    }

    // A gradient painted through an outline: a gradient layer matted by the outline filled opaque.
    void emitGradientPaint(const SvgPaint& paint, const document::NodeId outlineNode, const std::string& port,
                           const document::NodeId group, const std::string& name, const bool evenOdd,
                           const SvgElement& shape) {
        const auto& gradient = scene_.gradients[paint.gradient];
        Overrides matteOverrides;
        matteOverrides["kind"] = constant(static_cast<std::int64_t>(document::ShapeKind::Path));
        matteOverrides["path"] = document::DriverBindingSource{outlineNode, port};
        matteOverrides["fillEnabled"] = constant(true);
        matteOverrides["fillColor"] = constant(core::Color4d{1, 1, 1, 1});
        matteOverrides["strokeEnabled"] = constant(false);
        matteOverrides["fillRule"] = constant(static_cast<std::int64_t>(evenOdd ? 1 : 0));
        const auto matteNode = addNode(std::string(document::kShapeSourceNodeType), matteOverrides, cell());
        claim(matteNode);
        const auto world = [&](const media::SvgPoint p) {
            const auto q = fit_.apply(p.x, p.y);
            return document::Vec2d{q[0], q[1]};
        };
        Overrides gradientOverrides;
        gradientOverrides["gradient"] = constant(gradientValue(gradient));
        gradientOverrides["width"] = constant(compositionWidth_);
        gradientOverrides["height"] = constant(compositionHeight_);
        gradientOverrides["repeat"] = constant(static_cast<std::int64_t>(gradient.spread == media::SvgSpread::Repeat ? 1
                                                                          : gradient.spread == media::SvgSpread::Reflect ? 2 : 0));
        std::optional<std::array<double, 3>> ellipse; // scale x, scale y, rotation (degrees)
        document::Vec2d centreWorld{};
        if (!gradient.radial) {
            gradientOverrides["start"] = constant(world(gradient.p0));
            gradientOverrides["end"] = constant(world(gradient.p1));
        } else {
            centreWorld = world(gradient.center);
            const double scale = fitScale();
            gradientOverrides["start"] = constant(centreWorld);
            gradientOverrides["end"] = constant(document::Vec2d{centreWorld.x + gradient.radius * scale, centreWorld.y});
            const double sx = std::hypot(gradient.linearA, gradient.linearB), sy = std::hypot(gradient.linearC, gradient.linearD);
            const double dot = gradient.linearA * gradient.linearC + gradient.linearB * gradient.linearD;
            const bool anisotropic = std::fabs(sx - sy) > 0.02 * std::max(sx, sy) || std::fabs(dot) > 0.02 * sx * sy;
            if (anisotropic && gradient.userRadius > 0) {
                // L = Rot(u) diag(s1, s2) Rot(v): the gradient's circle through L is that circle scaled
                // s1 by s2 about its centre and then turned by u.
                const double a = gradient.linearA, b = gradient.linearB, c = gradient.linearC, d = gradient.linearD;
                const double e = (a + d) / 2, f = (a - d) / 2, g = (c + b) / 2, h = (c - b) / 2;
                const double q = std::hypot(e, h), r = std::hypot(f, g);
                const double s1 = q + r, s2 = std::fabs(q - r);
                const double u = (std::atan2(h, e) + std::atan2(g, f)) / 2.0;
                if (s1 > 1e-9 && s2 > 1e-9)
                    ellipse = std::array<double, 3>{s1, s2, u * 180.0 / std::numbers::pi};
            }
        }
        document::Vec2d localCentre = centreWorld;
        if (ellipse) {
            // The gradient's own circle, then the layer scales it s1 by s2 about the circle's centre
            // and turns it. The node's window starts at the origin, so the circle is centred in a
            // window just big enough to cover what the shape (and room around it to move and grow)
            // can show: the preimage of that box under the layer's scale and turn.
            const double scale = fitScale();
            const auto box = worldBounds(shape);
            const double margin = std::max({box[2] - box[0], box[3] - box[1], 64.0});
            const double angle = (*ellipse)[2] * std::numbers::pi / 180.0;
            const double cosine = std::cos(angle), sine = std::sin(angle);
            // The circle is drawn 256 pixels across its radius and the layer scales it up or down to
            // the real size, so a gradient in a unit bounding box keeps a sensible resolution.
            constexpr double kLocalRadius = 256.0;
            const double kx = std::max((*ellipse)[0] * scale * gradient.userRadius / kLocalRadius, 1e-9);
            const double ky = std::max((*ellipse)[1] * scale * gradient.userRadius / kLocalRadius, 1e-9);
            (*ellipse)[0] = kx;
            (*ellipse)[1] = ky;
            double reach = 1.0;
            for (const double x : {box[0] - margin, box[2] + margin})
                for (const double y : {box[1] - margin, box[3] + margin}) {
                    const double vx = x - centreWorld.x, vy = y - centreWorld.y;
                    // R(-u) v, then divide by the scales.
                    const double wx = cosine * vx + sine * vy, wy = -sine * vx + cosine * vy;
                    reach = std::max({reach, std::fabs(wx / std::max((*ellipse)[0], 1e-6)),
                                      std::fabs(wy / std::max((*ellipse)[1], 1e-6))});
                }
            reach += 2.0;
            if (reach * 2.0 > 8192.0) {
                note(shape.name + ": an extreme elliptical gradient is drawn within an 8192 pixel window");
                reach = 4096.0;
            }
            localCentre = {reach, reach};
            gradientOverrides["start"] = constant(localCentre);
            gradientOverrides["end"] = constant(document::Vec2d{localCentre.x + kLocalRadius, localCentre.y});
            gradientOverrides["width"] = constant(reach * 2.0);
            gradientOverrides["height"] = constant(reach * 2.0);
        }
        const auto gradientNode = addNode(std::string(gradient.radial ? document::kRadialGradientNodeType : document::kLinearGradientNodeType),
                                          gradientOverrides, cell());
        claim(gradientNode);
        // The matte source is added first, so the gradient lands above it.
        const auto matteSlot = addSlot(group, matteNode);
        const auto matteEntry = ensureEntry(group, matteSlot, name + " matte");
        const auto gradientSlot = addSlot(group, gradientNode);
        const auto gradientEntry = ensureEntry(group, gradientSlot, name);
        if (ellipse) {
            // The layer turns and scales about the frame centre plus its anchor; this anchor and
            // position keep the gradient's centre where it is.
            setLayerParameter(gradientEntry, document::kAnchorParameterRole,
                              document::Vec2d{localCentre.x - frameCenter_.x, localCentre.y - frameCenter_.y});
            setLayerParameter(gradientEntry, document::kPositionParameterRole, centreWorld);
            setLayerParameter(gradientEntry, document::kScaleParameterRole, document::Vec2d{(*ellipse)[0], (*ellipse)[1]});
            setLayerParameter(gradientEntry, document::kRotationParameterRole, (*ellipse)[2]);
        }
        layerOf(gradientEntry).trackMatte = document::TrackMatte{matteEntry.layer, document::TrackMatteChannel::Alpha, false, std::nullopt};
    }
};

} // namespace

OperationResult AddSvgArtwork::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return detail::missingComposition(composition_);
    std::shared_ptr<const media::SvgScene> scene = scene_;
    if (!scene) {
        auto read = media::readSvgSceneFile(path_);
        if (read.error != media::SvgError::None)
            return OperationResult::rejected(OperationIssueCode::InvalidValue, "SVG import failed: " + read.diagnostic);
        scene = std::move(read.scene);
    }
    Importer importer(draft, *composition, composition_, *scene, fit_);
    auto result = importer.run(name_.empty() ? path_.stem().string() : name_);
    if (result.status == OperationStatus::Applied) {
        // The placement is by construction; one validation of the whole composition at the end.
        if (const auto failure = detail::validateGraph(*composition, document::builtInNodeDefinitions()))
            return *failure;
    }
    return result;
}

} // namespace bloom::commands
