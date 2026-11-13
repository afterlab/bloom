// Parameter projections shared by both hosts (moved from apps/bloom-mcp).
#include "core_internal.hpp"

#include <bloom/document/animation.hpp>
#include <bloom/document/font_id.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/font_resolver.hpp>

#include <array>
#include <cstddef>
#include <string_view>

#include <type_traits>

namespace bloom::mcp {
namespace {

yyjson_mut_val* vector(Json& out, const std::initializer_list<double> values) {
    auto* result = out.array();
    for (const auto value : values)
        out.append(result, out.real(value));
    return result;
}

yyjson_mut_val* parameterValue(Json& out, const document::ParameterValue& value) {
    return std::visit(
        [&out](const auto& item) -> yyjson_mut_val* {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>) {
                return out.boolean(item);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return out.signedNumber(item);
            } else if constexpr (std::is_same_v<T, double>) {
                return out.real(item);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return out.text(item);
            } else if constexpr (std::is_same_v<T, document::Vec2d>) {
                return vector(out, {item.x, item.y});
            } else if constexpr (std::is_same_v<T, document::Vec3d>) {
                return vector(out, {item.x, item.y, item.z});
            } else if constexpr (std::is_same_v<T, core::Color4d>) {
                return vector(out, {item.red, item.green, item.blue, item.alpha});
            } else if constexpr (std::is_same_v<T, core::RationalTime>) {
                auto* result = out.array();
                out.append(result, out.signedNumber(item.numerator()));
                out.append(result, out.signedNumber(item.denominator()));
                return result;
            } else if constexpr (std::is_same_v<T, document::GradientValue>) {
                // #541: {"stops": [{"position": p, "color": [r, g, b, a]}]}, as a gradient
                // argument accepts.
                auto* gradient = out.object();
                auto* stops = out.array();
                for (const auto& stop : item.stops) {
                    auto* row = out.object();
                    out.set(row, "position", out.real(stop.position));
                    out.set(row, "color", vector(out, {stop.color.red, stop.color.green,
                                                       stop.color.blue, stop.color.alpha}));
                    out.append(stops, row);
                }
                out.set(gradient, "stops", stops);
                // #546: how it blends between them.
                out.set(gradient, "interpolation",
                        out.text(document::gradientInterpolationName(item.modes.interpolation)));
                out.set(gradient, "colorMode",
                        out.text(document::gradientColorModeName(item.modes.colorMode)));
                out.set(gradient, "hueDirection",
                        out.text(document::gradientHueDirectionName(item.modes.hueDirection)));
                return gradient;
            } else if constexpr (std::is_same_v<T, document::NumberListValue>) {
                // #627: {"values": [n...]}, the shape an argument sends a list in.
                auto* list = out.object();
                auto* values = out.array();
                for (const double value : item.values)
                    out.append(values, out.real(value));
                out.set(list, "values", values);
                return list;
            } else {
                const auto contourMembers = [&](yyjson_mut_val* path,
                                                const document::PathContour& contour) {
                    out.set(path, "closed", out.boolean(contour.closed));
                    auto* anchors = out.array();
                    for (const auto& anchor : contour.anchors) {
                        auto* row = out.object();
                        out.set(row, "point", vector(out, {anchor.point.x, anchor.point.y}));
                        out.set(row, "inHandle",
                                anchor.inHandle
                                    ? vector(out, {anchor.inHandle->x, anchor.inHandle->y})
                                    : out.null());
                        out.set(row, "outHandle",
                                anchor.outHandle
                                    ? vector(out, {anchor.outHandle->x, anchor.outHandle->y})
                                    : out.null());
                        out.set(row, "mode", out.text(document::pathAnchorModeName(anchor.mode)));
                        out.append(anchors, row);
                    }
                    out.set(path, "anchors", anchors);
                };
                auto* path = out.object();
                if (document::pathValueHasFlatForm(item)) {
                    contourMembers(path, item.firstContour());
                    return path;
                }
                // #589: {"contours": [{closed, anchors}...], "fillRule": ..., "groups": [...]}, as
                // a path argument accepts. Each of the last two appears only when it says
                // something.
                auto* contours = out.array();
                for (const auto& contour : item.contours) {
                    auto* row = out.object();
                    contourMembers(row, contour);
                    out.append(contours, row);
                }
                out.set(path, "contours", contours);
                if (item.fillRule != document::PathFillRule::Inherit)
                    out.set(path, "fillRule", out.text(document::pathFillRuleName(item.fillRule)));
                if (!item.groups.empty()) {
                    auto* groups = out.array();
                    for (const auto& group : item.groups) {
                        auto* row = out.object();
                        out.set(row, "key", out.text(group.key));
                        out.set(row, "firstContour", out.number(group.firstContour));
                        out.set(row, "contourCount", out.number(group.contourCount));
                        if (group.color)
                            out.set(row, "color",
                                    vector(out, {group.color->red, group.color->green,
                                                 group.color->blue, group.color->alpha}));
                        out.append(groups, row);
                    }
                    out.set(path, "groups", groups);
                }
                return path;
            }
        },
        value);
}
} // namespace

yyjson_mut_val* parameterRecord(Json& out, const document::ParameterRecord& value) {
    auto* record = out.object();
    out.set(record, "id", out.number(value.id.value()));
    out.set(record, "schemaKey", out.text(value.schemaKey));
    std::visit(
        [&](const auto& source) {
            using T = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<T, document::ConstantValueSource>) {
                out.set(record, "source", out.text("constant"));
                out.set(record, "value", parameterValue(out, source.value));
                // #489: a text font reads back as its font ID members and whether it can be drawn.
                if (value.schemaKey == document::kTextFontParameterSchemaKey) {
                    if (const auto* text = std::get_if<std::string>(&source.value))
                        if (const auto id = document::decodeFontId(*text)) {
                            auto* font = out.object();
                            out.set(font, "source", out.text(std::string(document::fontSourceName(id->source))));
                            out.set(font, "family", out.text(id->family));
                            out.set(font, "style", out.text(id->style));
                            out.set(font, "weight", out.number(static_cast<std::uint64_t>(id->weight)));
                            out.set(font, "italic", out.boolean(id->italic));
                            if (!id->postscriptName.empty())
                                out.set(font, "postscriptName", out.text(id->postscriptName));
                            if (!id->familyVersion.empty())
                                out.set(font, "familyVersion", out.text(id->familyVersion));
                            out.set(record, "font", font);
                            const auto status =
                                runtime::fonts::processFontResolver().status(*id, false);
                            using State = runtime::fonts::FontStatus::State;
                            out.set(record, "fontStatus",
                                    out.text(status.state == State::Available     ? "available"
                                             : status.state == State::Downloading ? "downloading"
                                             : status.state == State::Missing     ? "missing"
                                                                                  : "checking"));
                            if (status.state == State::Missing && !status.detail.empty())
                                out.set(record, "fontDetail", out.text(status.detail));
                        }
                }
                // An enumeration member reads back as both its stored value and its label.
                if (const auto* stored = std::get_if<std::int64_t>(&source.value)) {
                    for (const auto& option : document::parameterEnumOptions(value.schemaKey)) {
                        if (option.value == *stored)
                            out.set(record, "label", out.text(option.label));
                    }
                    if (value.schemaKey == document::kTextFontParameterSchemaKey) {
                        if (const auto label = document::legacyTextFontLabel(*stored))
                            out.set(record, "label", out.text(std::string(*label)));
                    }
                }
            } else if constexpr (std::is_same_v<T, document::AnimationCurveSource>) {
                out.set(record, "source", out.text("curve"));
                out.set(record, "curveId", out.number(source.curveId.value()));
            } else {
                out.set(record, "source", out.text("driver"));
                out.set(record, "nodeId", out.number(source.sourceNodeId.value()));
                out.set(record, "port", out.text(source.outputPort));
            }
        },
        value.source);
    return record;
}

yyjson_mut_val* noticeValue(Json& out, const McpNotice& notice) {
    auto* value = out.object();
    out.set(value, "kind", out.text(notice.kind));
    out.set(value, "revision", out.number(notice.revision));
    out.set(value, "message", out.text(notice.message));
    if (notice.composition)
        out.set(value, "composition", out.number(*notice.composition));
    if (notice.node || notice.parameter) {
        auto* subject = out.object();
        if (notice.composition)
            out.set(subject, "composition", out.number(*notice.composition));
        if (notice.node)
            out.set(subject, "node", out.number(*notice.node));
        if (notice.parameter)
            out.set(subject, "parameter", out.number(*notice.parameter));
        out.set(value, "subject", subject);
    }
    if (!notice.path.empty())
        out.set(value, "path", out.text(notice.path));
    if (!notice.projectName.empty())
        out.set(value, "projectName", out.text(notice.projectName));
    return value;
}

yyjson_mut_val* parameterSummary(Json& out, const document::ParameterRecord& value) {
    auto* record = out.object();
    out.set(record, "id", out.number(value.id.value()));
    out.set(record, "schemaKey", out.text(value.schemaKey));
    std::visit(
        [&](const auto& source) {
            using T = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<T, document::ConstantValueSource>) {
                out.set(record, "source", out.text("constant"));
                out.set(record, "valueOmitted", out.boolean(true));
                if (const auto* text = std::get_if<std::string>(&source.value)) {
                    out.set(record, "valueType", out.text("string"));
                    out.set(record, "valueBytes", out.number(text->size()));
                } else {
                    out.set(record, "valueType", out.text("other"));
                }
            } else if constexpr (std::is_same_v<T, document::AnimationCurveSource>) {
                out.set(record, "source", out.text("curve"));
                out.set(record, "curveId", out.number(source.curveId.value()));
            } else {
                out.set(record, "source", out.text("driver"));
                out.set(record, "nodeId", out.number(source.sourceNodeId.value()));
                out.set(record, "port", out.text(source.outputPort));
            }
        },
        value.source);
    return record;
}
std::string base64(const std::span<const std::byte> bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t index = 0;
    for (; index + 3 <= bytes.size(); index += 3) {
        const auto value = (std::to_integer<unsigned>(bytes[index]) << 16U) |
                           (std::to_integer<unsigned>(bytes[index + 1]) << 8U) |
                           std::to_integer<unsigned>(bytes[index + 2]);
        out.push_back(alphabet[(value >> 18U) & 63U]);
        out.push_back(alphabet[(value >> 12U) & 63U]);
        out.push_back(alphabet[(value >> 6U) & 63U]);
        out.push_back(alphabet[value & 63U]);
    }
    if (const auto left = bytes.size() - index; left > 0) {
        auto value = std::to_integer<unsigned>(bytes[index]) << 16U;
        if (left == 2)
            value |= std::to_integer<unsigned>(bytes[index + 1]) << 8U;
        out.push_back(alphabet[(value >> 18U) & 63U]);
        out.push_back(alphabet[(value >> 12U) & 63U]);
        out.push_back(left == 2 ? alphabet[(value >> 6U) & 63U] : '=');
        out.push_back('=');
    }
    return out;
}
} // namespace bloom::mcp

namespace bloom::mcp {
namespace {

constexpr std::array<std::string_view, 13> kInterpolationLabels{
    "Constant", "Linear",      "Bezier",   "Sine", "Quadratic", "Cubic",  "Quartic",
    "Quintic",  "Exponential", "Circular", "Back", "Bounce",    "Elastic"};
constexpr std::array<std::string_view, 3> kHandleModeLabels{"Free", "Aligned", "Auto"};
constexpr std::array<std::string_view, 4> kEasingLabels{"Automatic Easing", "Ease In", "Ease Out",
                                                        "Ease In and Out"};

} // namespace

void appendAnimationEnums(Json& out, yyjson_mut_val* records) {
    const auto table = [&](const std::string_view name, const auto& tokens, const auto& labels) {
        auto* record = out.object();
        out.set(record, "enum", out.text(name));
        auto* values = out.array();
        for (std::size_t index = 0; index < labels.size(); ++index) {
            auto* value = out.object();
            out.set(value, "id", out.number(index));
            out.set(value, "token", out.text(tokens(index)));
            out.set(value, "label", out.text(labels[index]));
            out.append(values, value);
        }
        out.set(record, "values", values);
        out.append(records, record);
    };
    table(
        "interpolation",
        [](const std::size_t index) {
            return document::keyframeInterpolationToken(
                static_cast<document::KeyframeInterpolation>(index));
        },
        kInterpolationLabels);
    table(
        "ease",
        [](const std::size_t index) {
            return document::keyframeEasingToken(static_cast<document::KeyframeEasing>(index));
        },
        kEasingLabels);
    table(
        "handleMode",
        [](const std::size_t index) {
            return document::keyframeHandleModeToken(
                static_cast<document::KeyframeHandleMode>(index));
        },
        kHandleModeLabels);
    constexpr std::array<std::string_view, 3> easyEaseTokens{"both", "in", "out"};
    constexpr std::array<std::string_view, 3> easyEaseLabels{"Easy Ease", "Easy Ease In",
                                                             "Easy Ease Out"};
    table(
        "easyEase", [&](const std::size_t index) { return easyEaseTokens[index]; }, easyEaseLabels);
}

} // namespace bloom::mcp
