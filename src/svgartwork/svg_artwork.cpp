#include <bloom/svgartwork/svg_artwork.hpp>

#include <bloom/document/font_id.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/media/svg_scene.hpp>
#include <bloom/platform/font_catalog.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <numbers>
#include <set>

namespace bloom::document {

std::string svgArtworkName(std::string name, const std::string& fallback) {
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
    if (!isValidHumanFacingName(out))
        return fallback;
    return out;
}

void SvgArtworkNotes::add(std::string text) {
    if (listed.size() < kMaximumListedSvgNotes)
        listed.push_back(std::move(text));
    else
        ++omitted[text.find(": ") == std::string::npos ? text : text.substr(text.find(": ") + 2)];
}

void SvgArtworkNotes::append(const SvgArtworkNotes& later) {
    for (const auto& text : later.listed)
        add(text);
    for (const auto& [reason, count] : later.omitted)
        omitted[reason] += count;
}

double svgArtworkLayoutOrigin(const Composition& composition) {
    // New cards go to the right of whatever is already on the canvas.
    double right = 0;
    for (const auto& [id, record] : composition.nodeLayout())
        right = std::max(right, record.position.x + record.width);
    return right > 0 ? right + 400.0 : 0.0;
}

namespace {

using media::SvgAffine;
using media::SvgElement;
using media::SvgPaint;

double decodeSrgb(const double value) noexcept {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}
core::Color4d linearColor(const core::Color4d& srgb) noexcept {
    return {decodeSrgb(srgb.red), decodeSrgb(srgb.green), decodeSrgb(srgb.blue),
            std::clamp(srgb.alpha, 0.0, 1.0)};
}

// A slot and the entry layer made for it, by index.
struct Slot final {
    std::size_t slot = 0;
    std::size_t layer = 0;
};

class Builder final {
  public:
    Builder(const media::SvgScene& scene, const SvgArtworkOptions& options)
        : scene_(scene), fonts_(options.fonts), layoutOriginX_(options.layoutOriginX) {
        const double compositionWidth = options.width, compositionHeight = options.height;
        frameCenter_ = {compositionWidth / 2.0, compositionHeight / 2.0};
        compositionWidth_ = compositionWidth;
        compositionHeight_ = compositionHeight;
        if (options.fit) {
            const double scale = std::min(compositionWidth / scene.width, compositionHeight / scene.height);
            fit_ = {scale, 0, 0, scale, (compositionWidth - scene.width * scale) / 2.0,
                    (compositionHeight - scene.height * scale) / 2.0};
        } else {
            fit_ = {1, 0, 0, 1, (compositionWidth - scene.width) / 2.0, (compositionHeight - scene.height) / 2.0};
        }
    }

    SvgArtworkDescription run(const std::string& name) {
        const auto rootName = svgArtworkName(name, "SVG");
        // nodes[0], the root Merge; the writer makes it (addRoot) and the caller places it.
        out_.nodes.push_back({std::string(kLayerStackNodeType), {}, {layoutOriginX_, 0}, true});
        // What the file holds and this import does not take, each named once.
        for (const auto& skipped : scene_.skipped)
            note(skipped.name + ": not imported (" + skipped.reason + ")");
        if (scene_.shapeCount > 2000)
            note(std::to_string(scene_.shapeCount) +
                 " shapes became that many layers; every layer is a full-frame image when the "
                 "frame is drawn, so a file this dense can need more memory than the GPU path "
                 "admits at a large frame size (a smaller frame or render scale draws it)");
        emitGroupChildren(scene_.root, 0, rootName);
        for (auto& [frameName, members] : finishedFrames_)
            out_.frames.push_back({std::move(frameName), {members.begin(), members.end()}});
        return std::move(out_);
    }

  private:
    const media::SvgScene& scene_;
    std::shared_ptr<const platform::FontCatalogue> fonts_;
    SvgAffine fit_;
    Vec2d frameCenter_;
    double compositionWidth_ = 0, compositionHeight_ = 0;
    double layoutOriginX_ = 0;
    SvgArtworkDescription out_;

    void note(std::string text) { out_.notes.add(std::move(text)); }

    static const std::string& describe(const SvgElement& element) {
        return element.where.empty() ? element.name : element.where;
    }

    // ---- what is made ----

    using Overrides = std::map<std::string, SvgArtworkValue>;

    std::size_t addNode(const std::string& type, Overrides overrides, const Vec2d position,
                        const bool collapsed = true) {
        const auto index = out_.nodes.size();
        out_.nodes.push_back({type, std::move(overrides), position, collapsed});
        out_.steps.emplace_back(SvgArtworkAddNode{index});
        return index;
    }
    static SvgArtworkValue constant(ParameterValue value) { return value; }
    static SvgArtworkValue driver(const std::size_t node, std::string port) {
        return SvgArtworkDriver{node, std::move(port)};
    }

    // A new slot on top of `merge`, fed by `source`'s image output.
    std::size_t addSlot(const std::size_t merge, const std::size_t source) {
        out_.steps.emplace_back(SvgArtworkAddSlot{merge, source});
        return out_.slotCount++;
    }

    // Creates the entry layer of a slot and names it.
    Slot ensureEntry(const std::size_t merge, const std::size_t slot, const std::string& name) {
        out_.steps.emplace_back(SvgArtworkAddEntry{merge, slot, name});
        return {slot, out_.layerCount++};
    }

    void setLayerParameter(const Slot slot, const std::string_view role, ParameterValue value) {
        out_.steps.emplace_back(SvgArtworkSetLayerValue{slot.layer, std::string(role), std::move(value)});
    }
    void setTrackMatte(const Slot target, const Slot matte, const TrackMatteChannel channel) {
        out_.steps.emplace_back(SvgArtworkSetTrackMatte{target.layer, matte.layer, channel});
    }

    // ---- values ----

    PathValue toPath(const std::vector<media::SvgContour>& contours, const bool evenOdd) const {
        PathValue value;
        value.fillRule = evenOdd ? PathFillRule::EvenOdd : PathFillRule::NonZero;
        const auto map = [&](const media::SvgPoint p) {
            const auto q = fit_.apply(p.x, p.y);
            return Vec2d{q[0], q[1]};
        };
        for (const auto& contour : contours) {
            PathContour out;
            out.closed = contour.closed;
            out.anchors.reserve(contour.anchors.size());
            for (const auto& anchor : contour.anchors) {
                PathAnchor a;
                a.point = map(anchor.point);
                if (anchor.hasIn)
                    a.inHandle = map(anchor.in);
                if (anchor.hasOut)
                    a.outHandle = map(anchor.out);
                a.mode = inferPathAnchorMode(a);
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
    GradientValue gradientValue(const media::SvgGradient& gradient) const {
        GradientValue value;
        const auto& stops = gradient.stops;
        const auto push = [&](const double position, const core::Color4d& srgb) {
            value.stops.push_back({std::clamp(position, 0.0, 1.0), linearColor(srgb)});
        };
        std::size_t budget = kMaximumGradientStops;
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
        if (value.stops.size() > kMaximumGradientStops)
            value.stops.resize(kMaximumGradientStops);
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
    Vec2d cell() {
        const auto index = cellIndex_++;
        const Vec2d position{layoutOriginX_ + static_cast<double>(index % kColumns) * kPitchX,
                             blockBaseY_ + static_cast<double>(index / kColumns) * kPitchY};
        maxY_ = std::max(maxY_, position.y + kPitchY);
        return position;
    }

    // ---- frames (one per group, over the nodes its direct children made) ----

    std::vector<std::set<std::size_t>*> activeMembers_;
    std::vector<std::pair<std::string, std::set<std::size_t>>> finishedFrames_;
    void claim(const std::size_t node) {
        if (!activeMembers_.empty())
            activeMembers_.back()->insert(node);
    }

    // ---- pivot ----

    // The bounds of an element's geometry in the composition (control points; a group's children
    // together; text by its anchor point).
    void accumulateBounds(const SvgElement& element, std::array<double, 4>& box, const int depth = 0) const {
        if (depth > 200)
            return;
        const auto add = [&](const double x, const double y) {
            const auto q = fit_.apply(x, y);
            box[0] = std::min(box[0], q[0]);
            box[1] = std::min(box[1], q[1]);
            box[2] = std::max(box[2], q[0]);
            box[3] = std::max(box[3], q[1]);
        };
        if (element.kind == SvgElement::Kind::Text)
            add(element.textX, element.textY);
        for (const auto& contour : element.contours)
            for (const auto& anchor : contour.anchors) {
                add(anchor.point.x, anchor.point.y);
                if (anchor.hasIn)
                    add(anchor.in.x, anchor.in.y);
                if (anchor.hasOut)
                    add(anchor.out.x, anchor.out.y);
            }
        for (const auto& child : element.children)
            accumulateBounds(child, box, depth + 1);
    }

    // An imported element rotates and scales about the centre of its own bounds, not about the
    // frame: a Merge entry's layer pivots on the frame's centre plus its Anchor, and the entry's
    // Position is where that pivot lands, so Anchor = centre - frame centre and Position = centre
    // leaves the picture exactly where it was. The centre is a multiple of 1/16 of a pixel, so the
    // sums are exact and the layer's matrix stays the identity until the artist turns it. The pivot
    // is a fixed point that stays put when the element's points are edited later: moving them does
    // not move it, and the Viewer's point editing needs no compensation (a frame-pivot layer is never
    // compensated).
    void pivotAtBounds(const Slot entry, const SvgElement& element) {
        std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
        accumulateBounds(element, box);
        if (!(box[2] >= box[0]) || !(box[3] >= box[1]) || !std::isfinite(box[0]) || !std::isfinite(box[2]) ||
            !std::isfinite(box[1]) || !std::isfinite(box[3]))
            return;
        const auto snap = [](const double value) { return std::round(value * 16.0) / 16.0; };
        const Vec2d centre{snap((box[0] + box[2]) / 2.0), snap((box[1] + box[3]) / 2.0)};
        setLayerParameter(entry, kAnchorParameterRole, Vec2d{centre.x - frameCenter_.x, centre.y - frameCenter_.y});
        setLayerParameter(entry, kPositionParameterRole, centre);
    }

    // ---- blur ----

    // An element's filter that is one Gaussian blur becomes a Gaussian Blur node between its
    // content and its slot. Bloom's blur has one radius (sigma = radius / 3, transparent beyond the
    // edge), so an uneven blur is applied evenly and listed, and a radius past the node's 256 pixel
    // limit is held at the limit and listed.
    std::size_t withBlur(const SvgElement& element, const std::size_t source) {
        if (!(element.blurX > 0.0) && !(element.blurY > 0.0))
            return source;
        const double larger = std::max(element.blurX, element.blurY);
        const double smaller = std::min(element.blurX, element.blurY);
        // The reader's deviations are in the file's units; the artwork is fitted into the frame.
        const double fit = fitScale();
        double sigma = (element.blurX + element.blurY) / 2.0 * fit;
        if (smaller < larger * 0.9) {
            sigma = (larger > 0 && smaller > 0 ? std::sqrt(larger * smaller) : larger / 2.0) * fit;
            note(describe(element) + ": a blur of " + std::to_string(element.blurX * fit) + " by " +
                 std::to_string(element.blurY * fit) + " pixels is applied evenly (Bloom's blur has one radius)");
        }
        double radius = 3.0 * sigma;
        if (radius > kMaximumBlurRadiusPixels) {
            note(describe(element) + ": the blur is held at the node's largest radius of 256 pixels");
            radius = kMaximumBlurRadiusPixels;
        }
        Overrides overrides;
        overrides["radius"] = constant(radius);
        overrides["edge"] = constant(std::int64_t{0});
        const auto blur = addNode("bloom.gaussian-blur", overrides, cell());
        claim(blur);
        out_.steps.emplace_back(SvgArtworkConnect{source, blur, "input"});
        return blur;
    }

    // ---- emission ----

    static constexpr std::size_t kChunkSize = 256;

    // The children of a group, bottom first; every new slot goes on top of the stack, so the
    // last painted element ends topmost, as in the file.
    void emitGroupChildren(const SvgElement& group, const std::size_t merge, const std::string& groupName) {
        std::set<std::size_t> members;
        activeMembers_.push_back(&members);
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
                const auto runMerge = addNode(std::string(kLayerStackNodeType), {}, cell());
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
    void emitElement(const SvgElement& element, const std::size_t merge) {
        for (const auto& approximation : element.approximations)
            note(describe(element) + ": " + approximation);
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
        if (element.kind != SvgElement::Kind::Text)
            pivotAtBounds(target, element);
        if (matte)
            setTrackMatte(target, *matte, element.mask ? TrackMatteChannel::Luma : TrackMatteChannel::Alpha);
    }

    Slot emitMatteSource(const SvgElement& element, const std::size_t merge) {
        const bool isMask = static_cast<bool>(element.mask);
        const auto& source = isMask ? *element.mask : *element.clip;
        for (const auto& approximation : source.approximations)
            note(describe(element) + ": " + approximation);
        const auto sourceMerge = addNode(std::string(kLayerStackNodeType), {}, cell());
        claim(sourceMerge);
        const auto slot = addSlot(merge, sourceMerge);
        auto entry = ensureEntry(merge, slot, source.name);
        emitGroupChildren(source, sourceMerge, source.name);
        return entry;
    }

    Slot emitGroup(const SvgElement& element, const std::size_t merge) {
        const auto groupMerge = addNode(std::string(kLayerStackNodeType), {}, cell());
        claim(groupMerge);
        const auto slot = addSlot(merge, withBlur(element, groupMerge));
        auto entry = ensureEntry(merge, slot, element.name);
        if (element.opacity < 1.0)
            setLayerParameter(entry, kOpacityParameterRole, element.opacity);
        emitGroupChildren(element, groupMerge, element.name);
        return entry;
    }

    // The em ascent of a face (hhea ascender over units per em), read from the font file: where the
    // baseline sits below the top of a text line. The built-in face's 0.928 when it cannot be read.
    static double faceAscentEm(const platform::FontFace& face) {
        constexpr double kFallback = 0.928;
        std::ifstream file(face.path, std::ios::binary);
        if (!file)
            return kFallback;
        const auto be16 = [](const unsigned char* at) { return static_cast<unsigned>(at[0]) << 8 | at[1]; };
        const auto be32 = [](const unsigned char* at) {
            return static_cast<std::uint32_t>(at[0]) << 24 | static_cast<std::uint32_t>(at[1]) << 16 |
                   static_cast<std::uint32_t>(at[2]) << 8 | at[3];
        };
        const auto read = [&file](const std::uint64_t offset, unsigned char* out, const std::size_t size) {
            file.clear();
            file.seekg(static_cast<std::streamoff>(offset));
            file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
            return static_cast<std::size_t>(file.gcount()) == size;
        };
        unsigned char header[16]{};
        if (!read(0, header, 12))
            return kFallback;
        std::uint64_t base = 0;
        if (header[0] == 't' && header[1] == 't' && header[2] == 'c' && header[3] == 'f') {
            unsigned char entry[4]{};
            if (!read(12 + 4ULL * face.faceIndex, entry, 4))
                return kFallback;
            base = be32(entry);
            if (!read(base, header, 12))
                return kFallback;
        }
        const unsigned tables = be16(header + 4);
        std::uint64_t headOffset = 0, hheaOffset = 0;
        for (unsigned i = 0; i < tables && i < 128; ++i) {
            unsigned char record[16]{};
            if (!read(base + 12 + 16ULL * i, record, 16))
                return kFallback;
            const std::string tag(reinterpret_cast<const char*>(record), 4);
            if (tag == "head")
                headOffset = be32(record + 8);
            else if (tag == "hhea")
                hheaOffset = be32(record + 8);
        }
        unsigned char upm[2]{}, ascender[2]{};
        if (headOffset == 0 || hheaOffset == 0 || !read(headOffset + 18, upm, 2) || !read(hheaOffset + 4, ascender, 2))
            return kFallback;
        const double units = be16(upm);
        const double ascent = static_cast<std::int16_t>(be16(ascender));
        return units > 0 && ascent > 0 ? ascent / units : kFallback;
    }

    struct ResolvedFont final {
        std::optional<FontId> id;
        double ascentEm = 0.928;
    };

    // The file's font through Bloom's font selection: an installed face of the family at the
    // closest weight and slope. Only when the family is not installed (or is a generic name) does
    // the text keep Bloom's default font, and that is listed.
    ResolvedFont resolveFont(const SvgElement& element) {
        ResolvedFont resolved;
        if (element.fontFamily.empty())
            return resolved;
        const auto lowered = [](std::string text) {
            for (auto& c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        };
        const auto family = lowered(element.fontFamily);
        if (family == "serif" || family == "sans-serif" || family == "monospace" || family == "cursive" ||
            family == "fantasy" || family == "system-ui") {
            note(describe(element) + ": the generic font family '" + element.fontFamily +
                 "' names no particular font, so Bloom's default font is used");
            return resolved;
        }
        if (!fonts_)
            fonts_ = std::make_shared<const platform::FontCatalogue>(
                platform::FontCatalogueProvider{}.enumerate({}, false));
        const platform::FontFace* best = nullptr;
        int bestScore = 0;
        for (const auto& face : fonts_->faces) {
            if (lowered(face.family) != family)
                continue;
            const int score = std::abs(face.weight - element.fontWeight) + (face.italic == element.fontItalic ? 0 : 1000);
            if (best == nullptr || score < bestScore) {
                best = &face;
                bestScore = score;
            }
        }
        if (best == nullptr) {
            note(describe(element) + ": the font '" + element.fontFamily +
                 "' is not installed, so Bloom's default font is used");
            return resolved;
        }
        if (best->italic != element.fontItalic || std::abs(best->weight - element.fontWeight) >= 100)
            note(describe(element) + ": '" + best->family + "' has no " + (element.fontItalic ? "italic " : "") +
                 "weight " + std::to_string(element.fontWeight) + "; its " + best->style + " face is used");
        FontId id;
        id.source = FontSource::System;
        id.family = best->family;
        id.style = best->style;
        id.weight = best->weight;
        id.italic = best->italic;
        id.postscriptName = best->postscriptName;
        if (!isValidFontId(id))
            return resolved;
        resolved.id = std::move(id);
        resolved.ascentEm = faceAscentEm(*best);
        return resolved;
    }

    // Text stays text: a native Text layer the artist can retype, restyle and animate, set in the
    // file's font when it is installed.
    Slot emitText(const SvgElement& element, const std::size_t merge) {
        const auto font = resolveFont(element);
        Overrides overrides;
        if (font.id)
            overrides[std::string(kTextFontParameterRole)] = constant(encodeFontId(*font.id));
        overrides[std::string(kTextParameterRole)] = constant(element.text);
        overrides[std::string(kTextSizeParameterRole)] = constant(std::max(1.0, element.fontSize * fitScale()));
        overrides[std::string(kTextColorParameterRole)] = constant(linearColor(element.fill.color));
        overrides[std::string(kTextAlignmentParameterRole)] = constant(static_cast<std::int64_t>(element.textAnchor));
        overrides[std::string(kTextAnchorModeParameterRole)] = constant(kTextAnchorModeFollowsAlignment);
        const auto position = cell();
        const auto node = addNode(std::string(kTextSourceNodeType), overrides, position);
        claim(node);
        const auto slot = addSlot(merge, withBlur(element, node));
        auto entry = ensureEntry(merge, slot, element.name);
        // An untranslated text layer has its line's top at the frame's top and its anchor at the
        // frame's left edge, so the baseline is the face's ascent below it.
        const auto at = fit_.apply(element.textX, element.textY);
        setLayerParameter(entry, kPositionParameterRole,
                          Vec2d{frameCenter_.x + at[0],
                                frameCenter_.y + at[1] - font.ascentEm * element.fontSize * fitScale()});
        if (element.textRotation != 0.0)
            setLayerParameter(entry, kRotationParameterRole, element.textRotation);
        if (element.opacity < 1.0)
            setLayerParameter(entry, kOpacityParameterRole, element.opacity);
        return entry;
    }

    Slot emitShape(const SvgElement& shape, const std::size_t merge) {
        const auto position = cell();
        const auto pathNode = addNode(std::string(kPathValueNodeType),
                                      {{std::string(kValueParameterRole), constant(toPath(shape.contours, shape.evenOdd))}},
                                      position);
        claim(pathNode);
        const bool solidFill = shape.fill.kind == SvgPaint::Kind::Color;
        const bool gradientFill = shape.fill.kind == SvgPaint::Kind::Gradient;
        const bool hasStroke = shape.stroke.kind != SvgPaint::Kind::None;
        const bool nativeStroke = shape.stroke.kind == SvgPaint::Kind::Color && shape.dashes.empty();
        // One Shape holds the fill and the stroke when both are plain.
        if (!gradientFill && (!hasStroke || nativeStroke)) {
            const auto node = addShapeNode(pathNode, shape, solidFill, nativeStroke, {position.x + 180.0, position.y});
            const auto slot = addSlot(merge, withBlur(shape, node));
            auto entry = ensureEntry(merge, slot, shape.name);
            if (shape.opacity < 1.0)
                setLayerParameter(entry, kOpacityParameterRole, shape.opacity);
            return entry;
        }
        // Otherwise a small group, the fill and then the stroke over it.
        const auto groupMerge = addNode(std::string(kLayerStackNodeType), {}, cell());
        claim(groupMerge);
        const auto slot = addSlot(merge, withBlur(shape, groupMerge));
        auto entry = ensureEntry(merge, slot, shape.name);
        if (shape.opacity < 1.0)
            setLayerParameter(entry, kOpacityParameterRole, shape.opacity);
        if (gradientFill) {
            emitGradientPaint(shape.fill, pathNode, std::string(kValuePortName), groupMerge, "Fill", shape.evenOdd, shape);
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
    std::size_t addShapeNode(const std::size_t pathNode, const SvgElement& shape, const bool fill, const bool stroke,
                             const Vec2d position) {
        Overrides overrides;
        overrides["kind"] = constant(static_cast<std::int64_t>(ShapeKind::Path));
        overrides["path"] = driver(pathNode, std::string(kValuePortName));
        overrides["fillEnabled"] = constant(fill);
        if (fill)
            overrides["fillColor"] = constant(linearColor(shape.fill.color));
        overrides["strokeEnabled"] = constant(stroke);
        if (stroke) {
            overrides["strokeColor"] = constant(linearColor(shape.stroke.color));
            overrides["strokeWidth"] = constant(std::max(0.0, shape.strokeWidth * fitScale()));
            overrides["strokeJoin"] = constant(static_cast<std::int64_t>(
                shape.join == media::SvgJoin::Round   ? ShapeStrokeJoin::Round
                : shape.join == media::SvgJoin::Bevel ? ShapeStrokeJoin::Bevel
                                                      : ShapeStrokeJoin::Miter));
            overrides["strokeCap"] = constant(static_cast<std::int64_t>(
                shape.cap == media::SvgCap::Round    ? ShapeStrokeCap::Round
                : shape.cap == media::SvgCap::Square ? ShapeStrokeCap::Square
                                                     : ShapeStrokeCap::Butt));
        }
        overrides["fillRule"] = constant(static_cast<std::int64_t>(shape.evenOdd ? 1 : 0));
        const auto node = addNode(std::string(kShapeSourceNodeType), overrides, position);
        claim(node);
        return node;
    }

    // A Shape Source that fills an outline given by another node's Path output.
    std::size_t addOutlineShape(const std::size_t outlineNode, const core::Color4d& srgb, const bool evenOdd,
                                const Vec2d position) {
        Overrides overrides;
        overrides["kind"] = constant(static_cast<std::int64_t>(ShapeKind::Path));
        overrides["path"] = driver(outlineNode, "path");
        overrides["fillEnabled"] = constant(true);
        overrides["fillColor"] = constant(linearColor(srgb));
        overrides["strokeEnabled"] = constant(false);
        overrides["fillRule"] = constant(static_cast<std::int64_t>(evenOdd ? 1 : 0));
        const auto node = addNode(std::string(kShapeSourceNodeType), overrides, position);
        claim(node);
        return node;
    }

    std::size_t addPathStroke(const std::size_t pathNode, const SvgElement& shape, const Vec2d position) {
        Overrides overrides;
        overrides["path"] = driver(pathNode, std::string(kValuePortName));
        overrides["width"] = constant(std::max(0.001, shape.strokeWidth * fitScale()));
        if (!shape.dashes.empty()) {
            overrides["dashLength"] = constant(std::max(0.0, shape.dashes[0] * fitScale()));
            overrides["dashGap"] =
                constant(std::max(0.0, (shape.dashes.size() > 1 ? shape.dashes[1] : shape.dashes[0]) * fitScale()));
            overrides["dashOffset"] = constant(shape.dashOffset * fitScale());
            if (shape.dashes.size() > 2)
                note(describe(shape) + ": a dash pattern of " + std::to_string(shape.dashes.size()) +
                     " lengths keeps its first dash and gap");
        }
        overrides["cap"] = constant(static_cast<std::int64_t>(shape.cap == media::SvgCap::Round    ? 1
                                                              : shape.cap == media::SvgCap::Square ? 2
                                                                                                    : 0));
        overrides["join"] = constant(static_cast<std::int64_t>(shape.join == media::SvgJoin::Round   ? 1
                                                               : shape.join == media::SvgJoin::Bevel ? 2
                                                                                                     : 0));
        return addNode(std::string(kPathStrokeNodeType), overrides, position);
    }

    // A gradient painted through an outline: a gradient layer matted by the outline filled opaque.
    void emitGradientPaint(const SvgPaint& paint, const std::size_t outlineNode, const std::string& port,
                           const std::size_t group, const std::string& name, const bool evenOdd,
                           const SvgElement& shape) {
        const auto& gradient = scene_.gradients[paint.gradient];
        Overrides matteOverrides;
        matteOverrides["kind"] = constant(static_cast<std::int64_t>(ShapeKind::Path));
        matteOverrides["path"] = driver(outlineNode, port);
        matteOverrides["fillEnabled"] = constant(true);
        matteOverrides["fillColor"] = constant(core::Color4d{1, 1, 1, 1});
        matteOverrides["strokeEnabled"] = constant(false);
        matteOverrides["fillRule"] = constant(static_cast<std::int64_t>(evenOdd ? 1 : 0));
        const auto matteNode = addNode(std::string(kShapeSourceNodeType), matteOverrides, cell());
        claim(matteNode);
        const auto world = [&](const media::SvgPoint p) {
            const auto q = fit_.apply(p.x, p.y);
            return Vec2d{q[0], q[1]};
        };
        Overrides gradientOverrides;
        gradientOverrides["gradient"] = constant(gradientValue(gradient));
        gradientOverrides["width"] = constant(compositionWidth_);
        gradientOverrides["height"] = constant(compositionHeight_);
        gradientOverrides["repeat"] = constant(static_cast<std::int64_t>(gradient.spread == media::SvgSpread::Repeat    ? 1
                                                                         : gradient.spread == media::SvgSpread::Reflect ? 2
                                                                                                                        : 0));
        std::optional<std::array<double, 3>> ellipse; // scale x, scale y, rotation (degrees)
        Vec2d centreWorld{};
        if (!gradient.radial) {
            gradientOverrides["start"] = constant(world(gradient.p0));
            gradientOverrides["end"] = constant(world(gradient.p1));
        } else {
            centreWorld = world(gradient.center);
            const double scale = fitScale();
            gradientOverrides["start"] = constant(centreWorld);
            gradientOverrides["end"] = constant(Vec2d{centreWorld.x + gradient.radius * scale, centreWorld.y});
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
        Vec2d localCentre = centreWorld;
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
            // #343: each axis reaches only as far as that box needs, so a flat shape's gradient is
            // not drawn in a square window as tall as it is wide.
            double reachX = 1.0, reachY = 1.0;
            for (const double x : {box[0] - margin, box[2] + margin})
                for (const double y : {box[1] - margin, box[3] + margin}) {
                    const double vx = x - centreWorld.x, vy = y - centreWorld.y;
                    // R(-u) v, then divide by the scales.
                    const double wx = cosine * vx + sine * vy, wy = -sine * vx + cosine * vy;
                    reachX = std::max(reachX, std::fabs(wx / std::max((*ellipse)[0], 1e-6)));
                    reachY = std::max(reachY, std::fabs(wy / std::max((*ellipse)[1], 1e-6)));
                }
            reachX += 2.0;
            reachY += 2.0;
            if (reachX * 2.0 > 8192.0 || reachY * 2.0 > 8192.0) {
                note(describe(shape) + ": an extreme elliptical gradient is drawn within an 8192 pixel window");
                reachX = std::min(reachX, 4096.0);
                reachY = std::min(reachY, 4096.0);
            }
            localCentre = {reachX, reachY};
            gradientOverrides["start"] = constant(localCentre);
            gradientOverrides["end"] = constant(Vec2d{localCentre.x + kLocalRadius, localCentre.y});
            gradientOverrides["width"] = constant(reachX * 2.0);
            gradientOverrides["height"] = constant(reachY * 2.0);
        }
        const auto gradientNode =
            addNode(std::string(gradient.radial ? kRadialGradientNodeType : kLinearGradientNodeType), gradientOverrides,
                    cell());
        claim(gradientNode);
        // The matte source is added first, so the gradient lands above it.
        const auto matteSlot = addSlot(group, matteNode);
        const auto matteEntry = ensureEntry(group, matteSlot, name + " matte");
        const auto gradientSlot = addSlot(group, gradientNode);
        const auto gradientEntry = ensureEntry(group, gradientSlot, name);
        if (ellipse) {
            // The layer turns and scales about the frame centre plus its anchor; this anchor and
            // position keep the gradient's centre where it is.
            setLayerParameter(gradientEntry, kAnchorParameterRole,
                              Vec2d{localCentre.x - frameCenter_.x, localCentre.y - frameCenter_.y});
            setLayerParameter(gradientEntry, kPositionParameterRole, centreWorld);
            setLayerParameter(gradientEntry, kScaleParameterRole, Vec2d{(*ellipse)[0], (*ellipse)[1]});
            setLayerParameter(gradientEntry, kRotationParameterRole, (*ellipse)[2]);
        }
        setTrackMatte(gradientEntry, matteEntry, TrackMatteChannel::Alpha);
    }
};

} // namespace

SvgArtworkDescription buildSvgArtwork(const media::SvgScene& scene, const SvgArtworkOptions& options) {
    return Builder(scene, options).run(options.name);
}

// ---- the writer ----

namespace {

[[noreturn]] void fail(std::string message) { throw SvgArtworkWriteError(std::move(message)); }

const NodeDefinition& latestDefinition(const std::string& type,
                                       std::map<std::string, const NodeDefinition*>& cache) {
    if (const auto found = cache.find(type); found != cache.end())
        return *found->second;
    const NodeDefinition* best = nullptr;
    for (const auto& candidate : builtInNodeDefinitions().definitions())
        if (candidate.key.typeId == type && (best == nullptr || candidate.key.schemaVersion > best->key.schemaVersion))
            best = &candidate;
    if (best == nullptr)
        fail("The node type " + type + " is not available");
    cache[type] = best;
    return *best;
}

} // namespace

SvgArtworkWriter::SvgArtworkWriter(Composition& composition, SvgArtworkTarget& target,
                                   const SvgArtworkDescription& description, const bool writeLayout)
    : composition_(composition), target_(target), description_(description), writeLayout_(writeLayout),
      nodes_(description.nodes.size()), slots_(description.slotCount), layers_(description.layerCount) {}

NodeId SvgArtworkWriter::writeNode(const std::size_t index) {
    static thread_local std::map<std::string, const NodeDefinition*> definitions;
    if (index >= description_.nodes.size())
        fail("The SVG artwork names a node it does not hold");
    const auto& described = description_.nodes[index];
    const auto& definition = latestDefinition(described.type, definitions);
    const auto nodeId = target_.allocateNode();
    if (!nodeId)
        fail("The node ID space is exhausted");
    NodeRecord node{*nodeId, definition.key.typeId, {}, definition.key.schemaVersion};
    node.parameters.reserve(definition.parameters.size());
    auto& parameters = composition_.parameters();
    for (const auto& parameter : definition.parameters) {
        if (parameter.schemaKey == kTimeRemapParameterSchemaKey)
            continue;
        const auto id = target_.allocateParameter();
        if (!id)
            fail("The parameter ID space is exhausted");
        ParameterSource source = ConstantValueSource{parameter.defaultValue};
        if (const auto found = described.values.find(parameter.role); found != described.values.end()) {
            if (const auto* value = std::get_if<ParameterValue>(&found->second)) {
                source = ConstantValueSource{*value};
            } else {
                const auto& bound = std::get<SvgArtworkDriver>(found->second);
                if (bound.node >= nodes_.size() || !nodes_[bound.node].isValid())
                    fail("The SVG artwork drives a value from a node it has not made");
                source = DriverBindingSource{nodes_[bound.node], bound.port};
            }
        }
        if (!parameters.insert({*id, parameter.schemaKey, std::move(source)}))
            fail("A value for " + described.type + " (" + parameter.role + ") is outside what it accepts");
        node.parameters.push_back({parameter.role, *id});
    }
    if (!composition_.graph().addNode(std::move(node)))
        fail("A node could not be inserted");
    if (writeLayout_)
        composition_.nodeLayout()[*nodeId] = {described.position, described.collapsed ? 160.0 : 240.0,
                                              described.collapsed, false};
    nodes_[index] = *nodeId;
    return *nodeId;
}

NodeId SvgArtworkWriter::addRoot() { return writeNode(0); }

LayerSlotId SvgArtworkWriter::addSlot(const NodeId merge, const NodeId source) {
    auto& graph = composition_.graph();
    auto* stack = graph.merge(merge);
    if (stack == nullptr)
        fail("A Merge could not be found");
    const auto slotId = target_.allocateLayerSlot();
    const auto edgeId = target_.allocateEdge();
    if (!slotId || !edgeId)
        fail("The slot or edge ID space is exhausted");
    const auto existingTop = stack->entries().empty() ? std::nullopt : std::optional(stack->entries().front().slotId);
    if (!stack->append({*slotId, {}}) || !stack->moveBefore(*slotId, existingTop))
        fail("A slot could not be added");
    if (!graph.addEdge({*edgeId, {source, std::string(kLayerStackOutputPort)},
                        LayerStackInputRef{merge, *slotId, std::string(kLayerStackContentInputRole)}}))
        fail("A connection could not be made");
    return *slotId;
}

LayerId SvgArtworkWriter::addEntry(const NodeId merge, const LayerSlotId slot, const std::string& name) {
    auto entry = target_.createEntryLayer(merge, slot);
    if (!entry.layer)
        fail(entry.failure.empty() ? "An entry layer could not be created" : entry.failure);
    auto* boundary = composition_.graph().findLayer(*entry.layer);
    if (boundary == nullptr)
        fail("An entry layer could not be found");
    boundary->name = svgArtworkName(name, "Layer");
    boundary->nameFollowsSource = false;
    return *entry.layer;
}

void SvgArtworkWriter::writeSteps() {
    const auto node = [&](const std::size_t index) {
        if (index >= nodes_.size() || !nodes_[index].isValid())
            fail("The SVG artwork names a node it has not made");
        return nodes_[index];
    };
    const auto layer = [&](const std::size_t index) -> LayerOutputBoundary& {
        if (index >= layers_.size() || !layers_[index].isValid())
            fail("The SVG artwork names a layer it has not made");
        auto* boundary = composition_.graph().findLayer(layers_[index]);
        if (boundary == nullptr)
            fail("An entry layer could not be found");
        return *boundary;
    };
    std::size_t nextSlot = 0, nextLayer = 0;
    for (const auto& step : description_.steps) {
        if (const auto* add = std::get_if<SvgArtworkAddNode>(&step)) {
            (void)writeNode(add->node);
        } else if (const auto* slot = std::get_if<SvgArtworkAddSlot>(&step)) {
            if (nextSlot >= slots_.size())
                fail("The SVG artwork holds more slots than it declares");
            slots_[nextSlot++] = addSlot(node(slot->merge), node(slot->source));
        } else if (const auto* connect = std::get_if<SvgArtworkConnect>(&step)) {
            const auto edgeId = target_.allocateEdge();
            if (!edgeId)
                fail("The edge ID space is exhausted");
            if (!composition_.graph().addEdge({*edgeId, {node(connect->source), std::string(kLayerStackOutputPort)},
                                               NodeInputRef{node(connect->destination), connect->input}}))
                fail("The blur could not be connected");
        } else if (const auto* entry = std::get_if<SvgArtworkAddEntry>(&step)) {
            if (nextLayer >= layers_.size() || entry->slot >= slots_.size() || !slots_[entry->slot].isValid())
                fail("The SVG artwork names a slot it has not made");
            layers_[nextLayer++] = addEntry(node(entry->merge), slots_[entry->slot], entry->name);
        } else if (const auto* value = std::get_if<SvgArtworkSetLayerValue>(&step)) {
            auto& boundary = layer(value->layer);
            for (const auto& binding : boundary.parameters)
                if (binding.role == value->role) {
                    if (!composition_.parameters().setSource(binding.parameterId, ConstantValueSource{value->value}))
                        fail("A layer value is outside what it accepts");
                    break;
                }
        } else if (const auto* matte = std::get_if<SvgArtworkSetTrackMatte>(&step)) {
            const auto matteLayer = layer(matte->matte).layerId;
            layer(matte->layer).trackMatte = TrackMatte{matteLayer, matte->channel, false, std::nullopt};
        }
    }
}

void SvgArtworkWriter::writeFrames() {
    for (const auto& frame : description_.frames) {
        if (frame.members.empty())
            continue;
        const auto groupId = target_.allocateNodeGroup();
        if (!groupId)
            break;
        NodeGroupRecord record;
        record.id = *groupId;
        record.name = svgArtworkName(frame.name, "Group");
        for (const auto index : frame.members)
            if (index < nodes_.size() && nodes_[index].isValid())
                record.members.insert(nodes_[index]);
        composition_.nodeGroups()[*groupId] = std::move(record);
    }
}

} // namespace bloom::document
