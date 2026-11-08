#include <bloom/document/node_groups.hpp>

#include <algorithm>

namespace bloom::document {
namespace {

// The owner's tree (#554), one list per group. To add a node, put its type id in the group the
// "Planned" line names for it. A node nobody planned goes where someone coming from another
// compositor would look for it; ask rather than invent a group.
//
// Planned: Gradient (one node replacing Linear Gradient and Radial Gradient).
constexpr std::array<std::string_view, 9> kSources{
    "bloom.image-source",       "bloom.video-source",   "bloom.audio-source",
    "bloom.composition-source", "bloom.solid-source",   "bloom.checker-source",
    "bloom.linear-gradient",    "bloom.radial-gradient", "bloom.test-card"};
// Planned: Image Switch, Edge Extend.
constexpr std::array<std::string_view, 7> kCompositing{
    "bloom.layer-stack", "bloom.adjustment",     "bloom.layer-output",     "bloom.image-mix",
    "bloom.set-alpha",   "bloom.multiply-alpha", "bloom.alpha-association"};
// Planned: Image Math, Image Map Range, Image Clamp.
constexpr std::array<std::string_view, 7> kChannels{
    "bloom.channel-copy",        "bloom.channel-pass",      "bloom.channel-remove",
    "bloom.channel-shuffle",     "bloom.separate-channels", "bloom.combine-channels",
    "bloom.image-to-channel-set"};
// Planned: Channel Key, Chroma Key, Despill, Matte Refine, HSL Qualifier, Transition/Wipe Matte.
constexpr std::array<std::string_view, 8> kKeyingMattes{
    "bloom.luma-key",      "bloom.cryptomatte", "bloom.mask",         "bloom.mask-combine",
    "bloom.image-to-mask", "bloom.matte-blur",  "bloom.matte-dilate", "bloom.matte-erode"};
// Planned: Tile & Mirror.
constexpr std::array<std::string_view, 8> kTransform{
    "bloom.transform-2d", "bloom.camera-shake", "bloom.crop", "bloom.pad",
    "bloom.fit",          "bloom.resize",       "bloom.flip", "bloom.rotate-90"};
// Planned: Corner Pin, ST Map, Displace, Procedural Warp.
constexpr std::array<std::string_view, 2> kDistort{"bloom.lens-distortion",
                                                   "bloom.chromatic-aberration"};
// Planned: Denoise.
constexpr std::array<std::string_view, 11> kBlurSharpen{"bloom.gaussian-blur",
                                                        "bloom.box-blur",
                                                        "bloom.z-defocus",
                                                        "bloom.motion-blur",
                                                        "bloom.vector-motion-blur",
                                                        "bloom.optical-flow",
                                                        "bloom.optical-flow-motion-blur",
                                                        "bloom.directional-blur",
                                                        "bloom.spin-blur",
                                                        "bloom.zoom-blur", "bloom.sharpen"};
// Planned: Inner Shadow, Inner Glow.
constexpr std::array<std::string_view, 6> kLightShadow{"bloom.glow",      "bloom.light-wrap",
                                                       "bloom.drop-shadow", "bloom.depth-fog",
                                                       "bloom.lens-flare", "bloom.light-rays"};
// Planned: Dither, Quantize.
constexpr std::array<std::string_view, 3> kStylize{"bloom.vignette", "bloom.film-grain",
                                                   "bloom.codec-degrade"};
// Planned: Color Matrix, Tone Map.
constexpr std::array<std::string_view, 9> kColorCorrection{
    "bloom.grade",  "bloom.levels",   "bloom.curves", "bloom.hue-saturation", "bloom.exposure",
    "bloom.offset", "bloom.multiply", "bloom.invert", "bloom.colorize"};
constexpr std::array<std::string_view, 2> kColorManagement{"bloom.ocio-colour-space-transform",
                                                           "bloom.ocio-file-transform"};
// Planned: Time Offset, Frame Hold, Time Remap, Flow Retime.
constexpr std::array<std::string_view, 2> kTime{"bloom.echo", "bloom.frame-rate-hold"};
// Planned: SVG Vector Source, Text Layout, Text Animator, Text On Path, Text To Shape, Transform
// Path, Trim Path, Offset Path, Boolean/Merge Paths, Round Corners, Path Stroke.
constexpr std::array<std::string_view, 2> kTextPaths{"bloom.text-source", "bloom.shape-source"};
// Planned: Point, Grid, Scatter, Points Along Path, Shape To Points, Text To Points, Clone To
// Points, Draw Instances, Particle System.
constexpr std::array<std::string_view, 0> kInstancesParticles{};
// Planned: Tracker, Stabilize, Histogram, Image Statistics, Compare, Audio Amplitude, Audio
// Waveform, Audio Spectrum.
constexpr std::array<std::string_view, 0> kTrackingAnalysis{};
constexpr std::array<std::string_view, 16> kFields{
    "bloom.field-combine",    "bloom.field-derivative",     "bloom.field-force",
    "bloom.field-from-image", "bloom.field-gradient",       "bloom.field-math",
    "bloom.field-noise",      "bloom.field-pattern",        "bloom.field-random",
    "bloom.field-rasterize",  "bloom.field-remap",          "bloom.field-sample",
    "bloom.field-separate",   "bloom.field-shape-distance", "bloom.field-transform",
    "bloom.field-voronoi"};
constexpr std::array<std::string_view, 14> kValues{
    "bloom.composition-duration", "bloom.composition-size",  "bloom.data-sample",
    "bloom.point-set-read",       "bloom.ramp-sample",       "bloom.table-lookup",
    "bloom.value-boolean",        "bloom.value-color",       "bloom.value-integer",
    "bloom.value-scalar",         "bloom.value-string",      "bloom.value-vector2",
    "bloom.value-vector3",        "bloom.value.layer-bounds"};
// Planned: Oscillator, Wiggle, Loop Value.
constexpr std::array<std::string_view, 14> kMath{"bloom.clamp",        "bloom.degrees-to-radians",
                                                 "bloom.integer-math", "bloom.map-range",
                                                 "bloom.math",         "bloom.mix",
                                                 "bloom.ping-pong",    "bloom.radians-to-degrees",
                                                 "bloom.random",       "bloom.rounding",
                                                 "bloom.sign",         "bloom.smoothstep",
                                                 "bloom.snap",         "bloom.wrap"};
constexpr std::array<std::string_view, 20> kConvert{
    "bloom.boolean-to-integer", "bloom.boolean-to-scalar",  "bloom.boolean-to-string",
    "bloom.color-to-string",    "bloom.color-to-vector3",   "bloom.integer-to-boolean",
    "bloom.integer-to-scalar",  "bloom.integer-to-string",  "bloom.scalar-to-boolean",
    "bloom.scalar-to-integer",  "bloom.scalar-to-string",   "bloom.string-to-boolean",
    "bloom.string-to-color",    "bloom.string-to-integer",  "bloom.string-to-scalar",
    "bloom.vector2-to-string",  "bloom.vector2-to-vector3", "bloom.vector3-to-color",
    "bloom.vector3-to-string",  "bloom.vector3-to-vector2"};
constexpr std::array<std::string_view, 15> kString{
    "bloom.string-case",        "bloom.string-character-at", "bloom.string-concatenate",
    "bloom.string-contains",    "bloom.string-ends-with",    "bloom.string-equals",
    "bloom.string-format",      "bloom.string-length",       "bloom.string-pad",
    "bloom.string-repeat",      "bloom.string-replace",      "bloom.string-split",
    "bloom.string-starts-with", "bloom.string-substring",    "bloom.string-trim"};
constexpr std::array<std::string_view, 11> kLogic{
    "bloom.boolean-logic",  "bloom.boolean-not",    "bloom.in-range",       "bloom.compare",
    "bloom.switch-boolean", "bloom.switch-color",   "bloom.switch-integer", "bloom.switch-scalar",
    "bloom.switch-string",  "bloom.switch-vector2", "bloom.switch-vector3"};
constexpr std::array<std::string_view, 7> kTimeValues{
    "bloom.value-time",         "bloom.frame-number",      "bloom.frame-rate",
    "bloom.seconds-to-frames",  "bloom.frames-to-seconds", "bloom.seconds-to-timecode",
    "bloom.timecode-to-seconds"};
constexpr std::array<std::string_view, 7> kColorValues{
    "bloom.separate-hsv", "bloom.combine-hsv",   "bloom.hue-shift",   "bloom.luminance",
    "bloom.mix-color",    "bloom.separate-rgba", "bloom.combine-rgba"};
constexpr std::array<std::string_view, 7> kVector{
    "bloom.vector2-math",      "bloom.vector3-math", "bloom.vector2-reduce",
    "bloom.vector3-reduce",    "bloom.rotate-2d",    "bloom.polar-to-cartesian",
    "bloom.cartesian-to-polar"};
// Planned: Group Input, Group Output, Node Group, File Cache.
constexpr std::array<std::string_view, 5> kUtilities{"bloom.reroute", "bloom.separate-xy",
                                                     "bloom.combine-xy", "bloom.separate-xyz",
                                                     "bloom.combine-xyz"};
constexpr std::array<std::string_view, 2> kOutput{"bloom.composition-output",
                                                  "bloom.image-preview"};

struct GroupRecord final {
    std::string_view name;
    std::string_view wireName;
    std::span<const std::string_view> members;
};

[[nodiscard]] GroupRecord recordOf(const NodeGroup group) noexcept {
    switch (group) {
    case NodeGroup::Sources:
        return {"Sources", "sources", kSources};
    case NodeGroup::Compositing:
        return {"Compositing", "compositing", kCompositing};
    case NodeGroup::Channels:
        return {"Channels", "channels", kChannels};
    case NodeGroup::KeyingMattes:
        return {"Keying & Mattes", "keying-mattes", kKeyingMattes};
    case NodeGroup::Transform:
        return {"Transform", "transform", kTransform};
    case NodeGroup::Distort:
        return {"Distort", "distort", kDistort};
    case NodeGroup::BlurSharpen:
        return {"Blur & Sharpen", "blur-sharpen", kBlurSharpen};
    case NodeGroup::LightShadow:
        return {"Light & Shadow", "light-shadow", kLightShadow};
    case NodeGroup::Stylize:
        return {"Stylize", "stylize", kStylize};
    case NodeGroup::ColorCorrection:
        return {"Color Correction", "color-correction", kColorCorrection};
    case NodeGroup::ColorManagement:
        return {"Color Management", "color-management", kColorManagement};
    case NodeGroup::Time:
        return {"Time", "time", kTime};
    case NodeGroup::TextPaths:
        return {"Text & Paths", "text-paths", kTextPaths};
    case NodeGroup::InstancesParticles:
        return {"Instances & Particles", "instances-particles", kInstancesParticles};
    case NodeGroup::TrackingAnalysis:
        return {"Tracking & Analysis", "tracking-analysis", kTrackingAnalysis};
    case NodeGroup::Fields:
        return {"Fields", "fields", kFields};
    case NodeGroup::Values:
        return {"Values", "values", kValues};
    case NodeGroup::Math:
        return {"Math", "math", kMath};
    case NodeGroup::Convert:
        return {"Convert", "convert", kConvert};
    case NodeGroup::String:
        return {"String", "string", kString};
    case NodeGroup::Logic:
        return {"Logic", "logic", kLogic};
    case NodeGroup::TimeValues:
        return {"Time Values", "time-values", kTimeValues};
    case NodeGroup::ColorValues:
        return {"Color Values", "color-values", kColorValues};
    case NodeGroup::Vector:
        return {"Vector", "vector", kVector};
    case NodeGroup::Utilities:
        return {"Utilities", "utilities", kUtilities};
    case NodeGroup::Output:
        return {"Output", "output", kOutput};
    }
    return {};
}

} // namespace

std::string_view nodeGroupName(const NodeGroup group) noexcept { return recordOf(group).name; }

std::string_view nodeGroupWireName(const NodeGroup group) noexcept {
    return recordOf(group).wireName;
}

std::span<const std::string_view> nodeGroupMembers(const NodeGroup group) noexcept {
    return recordOf(group).members;
}

std::optional<NodeGroup> nodeGroupOf(const std::string_view typeId) noexcept {
    for (const auto group : kNodeGroups)
        if (std::ranges::find(nodeGroupMembers(group), typeId) != nodeGroupMembers(group).end())
            return group;
    return std::nullopt;
}

} // namespace bloom::document
