#pragma once

// SVG scene reader (#343, docs/architecture/vector-artwork.md): turns the bytes of an SVG file into a
// tree of groups and shapes whose geometry, paint, stroke and opacity are plain values, ready to
// become native Bloom layers and paths. Qt-free. The reader never fetches a reference, loads
// another file or runs anything: bytes in, tree out. Everything it cannot represent is listed in
// `skipped` by element name, never dropped silently.

#include <bloom/core/color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bloom::media {

// The bounds of the profile. A file or result beyond one is refused with a diagnostic.
inline constexpr std::size_t kMaxSvgFileBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kMaxSvgNesting = 128;
inline constexpr std::size_t kMaxSvgXmlNodes = 600'000;
inline constexpr std::size_t kMaxSvgDrawnElements = 20'000;
inline constexpr std::size_t kMaxSvgAnchors = 3'000'000;
// The Path value's own per-contour bound (document::kMaximumPathAnchors).
inline constexpr std::size_t kMaxSvgContourAnchors = 4096;
inline constexpr std::size_t kMaxSvgUseDepth = 24;
inline constexpr std::size_t kMaxSvgUseExpansions = 100'000;
inline constexpr std::size_t kMaxSvgEntities = 64;
inline constexpr std::size_t kMaxSvgSkippedNames = 2000;
inline constexpr std::size_t kMaxSvgStyleBytes = 1U * 1024U * 1024U;
inline constexpr double kMaxSvgExtent = 1'000'000.0;

// x' = a*x + c*y + e, y' = b*x + d*y + f.
struct SvgAffine final {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    [[nodiscard]] std::array<double, 2> apply(double x, double y) const noexcept {
        return {a * x + c * y + e, b * x + d * y + f};
    }
    [[nodiscard]] double determinant() const noexcept { return a * d - b * c; }
    friend bool operator==(const SvgAffine&, const SvgAffine&) = default;
};
[[nodiscard]] SvgAffine operator*(const SvgAffine& outer, const SvgAffine& inner) noexcept;

struct SvgPoint final {
    double x = 0, y = 0;
};

// One anchor of a contour. Handles are absolute points; a segment from anchor i to the next is a
// cubic when either side has a handle, a straight line otherwise.
struct SvgAnchor final {
    SvgPoint point;
    SvgPoint in;
    SvgPoint out;
    bool hasIn = false;
    bool hasOut = false;
};
struct SvgContour final {
    std::vector<SvgAnchor> anchors;
    bool closed = false;
};

enum class SvgSpread : std::uint8_t { Pad, Reflect, Repeat };

struct SvgGradientStop final {
    double offset = 0;
    // Straight sRGB-encoded colour; alpha is the stop's colour alpha times stop-opacity.
    core::Color4d color{};
};

// A gradient resolved against the shape that uses it, in the shape's final (world) coordinates.
struct SvgGradient final {
    bool radial = false;
    // Linear: the line whose projection is the gradient's t. Exact for any affine transform.
    SvgPoint p0, p1;
    // Radial: the centre and the radius (a circle; see `approximate`).
    SvgPoint center;
    double radius = 0;
    // The map from the gradient's own circle (radius `userRadius`) to final coordinates: x' = a x +
    // c y, y' = b x + d y. It lets a radial gradient under a non-uniform or skewed transform be
    // placed exactly, as an ellipse.
    double userRadius = 0;
    double linearA = 1, linearB = 0, linearC = 0, linearD = 1;
    SvgSpread spread = SvgSpread::Pad;
    std::vector<SvgGradientStop> stops;
    // A radial gradient with a focal point away from its centre is drawn centred (the focal point is ignored).
    bool approximate = false;
    std::string approximation;
};

struct SvgPaint final {
    enum class Kind : std::uint8_t { None, Color, Gradient };
    Kind kind = Kind::None;
    // Straight sRGB-encoded colour with the paint's opacity (fill-opacity, stroke-opacity) in alpha.
    core::Color4d color{};
    // Index into SvgScene::gradients for Kind::Gradient.
    std::size_t gradient = 0;
};

enum class SvgCap : std::uint8_t { Butt, Round, Square };
enum class SvgJoin : std::uint8_t { Miter, Round, Bevel };

struct SvgElement;

struct SvgElement final {
    enum class Kind : std::uint8_t { Group, Shape, Text };
    Kind kind = Kind::Group;
    // The file's own name for it: id, else an editor label, else the element's tag.
    std::string name;
    std::string tag;
    // Element opacity (the `opacity` property), applied after its fill and stroke are composited.
    double opacity = 1.0;
    // A group's children in painting order (bottom first).
    std::vector<SvgElement> children;
    // A clip or mask applies to this element as a whole. A clip is the union of its shapes; a mask
    // is read by luminance. Both are in the same final coordinates as the element.
    std::shared_ptr<const SvgElement> clip;
    std::shared_ptr<const SvgElement> mask;

    // Shape only. Geometry is in final coordinates: every transform and the view box are applied.
    std::vector<SvgContour> contours;
    bool evenOdd = false;
    SvgPaint fill;
    SvgPaint stroke;
    // In final coordinates: the file's stroke width times the transform's mean scale.
    double strokeWidth = 1.0;
    SvgCap cap = SvgCap::Butt;
    SvgJoin join = SvgJoin::Miter;
    double miterLimit = 4.0;
    std::vector<double> dashes;
    double dashOffset = 0.0;
    // Text only. The first character's baseline position, size, anchor (0 start, 1 middle, 2 end) and
    // turn in degrees, all in final coordinates. The fill is a solid colour.
    std::string text;
    double textX = 0, textY = 0, fontSize = 16.0, textRotation = 0.0;
    int textAnchor = 0;
    std::string fontFamily;
    bool fontBold = false;
    // Everything this element needed but could not take exactly, in plain sentences.
    std::vector<std::string> approximations;
};

struct SvgSkipped final {
    std::string name;
    std::string reason;
};

enum class SvgError : std::uint8_t {
    None,
    Cancelled,
    ReadFailed,
    TooLarge,
    NotSvg,
    Malformed,
    TooComplex,
    Empty
};

struct SvgScene final {
    // The viewport in pixels (96 per inch).
    double width = 0;
    double height = 0;
    SvgElement root;
    std::vector<SvgGradient> gradients;
    std::vector<SvgSkipped> skipped;
    // Elements of the file that are neither metadata nor definitions (drawn, skipped or hidden),
    // and the drawn shapes the scene holds.
    std::size_t elementCount = 0;
    std::size_t shapeCount = 0;
    std::size_t textCount = 0;
    std::size_t groupCount = 0;
    std::size_t anchorCount = 0;
};

struct SvgReadResult final {
    std::shared_ptr<const SvgScene> scene;
    SvgError error = SvgError::None;
    std::string diagnostic;
};

using SvgCancel = std::function<bool()>;

// Parses an in-memory document. `cancel` is polled during parsing.
[[nodiscard]] SvgReadResult parseSvgScene(std::span<const std::byte> bytes,
                                          const SvgCancel& cancel = {});

// Reads (at most kMaxSvgFileBytes) and parses one file. A missing or unreadable file is a
// diagnostic, not an exception.
[[nodiscard]] SvgReadResult readSvgSceneFile(const std::filesystem::path& path,
                                             const SvgCancel& cancel = {});

} // namespace bloom::media
