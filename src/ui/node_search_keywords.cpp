#include "node_editor_items.hpp"

#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/codec_degrade_nodes.hpp>
#include <bloom/document/lens_distortion_nodes.hpp>
#include <bloom/document/chromatic_aberration_nodes.hpp>
#include <bloom/document/rolling_shutter_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/document/test_card_nodes.hpp>
#include <bloom/document/light_rays_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/image_channel_nodes.hpp>
#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/sharpen_nodes.hpp>
#include <bloom/document/vignette_nodes.hpp>

#include <array>

namespace bloom::ui::node_editor {
namespace {

struct NodeAliases final {
    std::string_view typeId;
    // The names someone coming from another compositor reaches for, separated by '|'.
    const char* names;
};

// What other compositors call this node (#554). Add a new node's names here when it
// lands, and move a name that was parked on the closest node once its own node exists: Keylight
// and Primatte belong to Chroma Key, CornerPin2D to Corner Pin, Arithmetic to Image Math.
constexpr auto kNodeAliases = std::to_array<NodeAliases>({
    // Sources
    {"bloom.image-source", "Read|Footage|Still"},
    {"bloom.video-source", "Read|Footage|Movie"},
    {"bloom.audio-source", "AudioRead|Audio Footage"},
    {"bloom.composition-source", "Precomp|Pre-comp|Precompose|Pre-compose"},
    {"bloom.solid-source", "Constant|Solid Layer|Color Solid"},
    {"bloom.checker-source", "CheckerBoard|Checkerboard"},
    {"bloom.linear-gradient", "Ramp|Gradient Ramp"},
    {"bloom.radial-gradient", "Radial|Gradient Ramp"},
    // Compositing
    {"bloom.layer-stack", "Merge|Merge2|Over|Composite"},
    {"bloom.adjustment", "Adjustment Layer"},
    {"bloom.image-mix", "Dissolve|Keymix|Blend|Cross Dissolve"},
    {"bloom.set-alpha", "Set Matte|Copy Alpha"},
    {"bloom.multiply-alpha", "Track Matte|Stencil Alpha"},
    {"bloom.alpha-association", "Premult|Unpremult|Remove Color Matting"},
    // Channels
    {"bloom.channel-copy", "Copy|ShuffleCopy|Set Channels"},
    {"bloom.channel-pass", "EXtractoR|Extract Layer"},
    {"bloom.channel-remove", "Remove"},
    {"bloom.channel-shuffle", "Shuffle|Shuffle2|Shift Channels|Set Channels"},
    {"bloom.combine-channels", "ChannelMerge"},
    {"bloom.image-to-channel-set", "AddChannels"},
    // Keying & Mattes
    {"bloom.luma-key", "Keyer|Extract|Linear Color Key|Keylight|Primatte"},
    {"bloom.mask", "Roto|RotoPaint|Rotoscope|Bezier"},
    {"bloom.mask-combine", "MergeMat|Mask Mode"},
    {"bloom.image-to-mask", "Luma Matte|Alpha Matte"},
    {"bloom.matte-blur", "Feather|Matte Feather"},
    {"bloom.matte-dilate", "Dilate|Minimax|Simple Choker|Matte Choker"},
    {"bloom.matte-erode", "Erode|FilterErode|Minimax|Simple Choker|Matte Choker"},
    // Transform
    {"bloom.transform-2d", "Transform|Position|CornerPin2D|Corner Pin"},
    {"bloom.pad", "AdjBBox|Grow Bounds"},
    {"bloom.fit", "Reformat|Fit to Comp"},
    {"bloom.resize", "Reformat|Scale"},
    {"bloom.flip", "Mirror|Flop"},
    // Blur & Sharpen
    {"bloom.gaussian-blur", "Blur|Fast Blur"},
    {"bloom.box-blur", "Fast Box Blur"},
    {"bloom.z-defocus", "ZDefocus|Defocus|Lens Blur|Camera Lens Blur"},
    {"bloom.motion-blur", "MotionBlur|MotionBlur2D|Force Motion Blur"},
    {"bloom.vector-motion-blur", "VectorBlur"},
    {"bloom.optical-flow", "VectorGenerator|SmartVector"},
    {"bloom.optical-flow-motion-blur", "Pixel Motion Blur"},
    {"bloom.directional-blur", "DirBlur"},
    {"bloom.spin-blur", "RadialBlur|Radial Blur"},
    {"bloom.zoom-blur", "RadialBlur|Radial Blur"},
    {"bloom.sharpen", "Unsharp Mask|USM"},
    // Light & Shadow
    {"bloom.light-wrap", "LightWrap"},
    {"bloom.drop-shadow", "DropShadow|Shadow"},
    {"bloom.depth-fog", "Fog|Fog 3D"},
    {"bloom.lens-flare", "Flare|Optical Flares"},
    // Stylize
    {"bloom.film-grain", "Grain|Add Grain|Noise|ReGrain"},
    {"bloom.codec-degrade", "Compression|Macroblock|Blocky|JPEG|H.264"},
    // Color Correction
    {"bloom.grade", "ColorCorrect|Lumetri|Color Balance"},
    {"bloom.curves", "ColorLookup|HueCorrect"},
    {"bloom.hue-saturation", "HueShift|Saturation|HSVTool"},
    {"bloom.exposure", "EXPTool"},
    {"bloom.offset", "Add|Arithmetic"},
    {"bloom.multiply", "Arithmetic|Gain"},
    {"bloom.colorize", "Fill|Tint|Tritone|Colorama"},
    // Color Management
    {"bloom.ocio-colour-space-transform", "OCIOColorSpace|Colorspace|Color Profile Converter"},
    {"bloom.ocio-file-transform", "OCIOFileTransform|Apply Color LUT|Vectorfield"},
    // Time
    {"bloom.echo", "TimeEcho|FrameBlend"},
    // Fields
    {"bloom.field-noise", "Fractal Noise|Turbulent Noise"},
    // Values
    {"bloom.value-scalar", "Slider Control"},
    {"bloom.value-color", "Color Control"},
    {"bloom.value-vector2", "Point Control"},
    {"bloom.value-boolean", "Checkbox Control"},
    // Utilities
    {"bloom.reroute", "Dot|Elbow"},
    // Output
    {"bloom.composition-output", "Write|Render Queue|Output Module"},
    {"bloom.image-preview", "Viewer"},
});

} // namespace

QString nodeSearchKeywords(const std::string_view typeId) {
    if (typeId == document::kSeparateChannelsNodeType ||
        typeId == document::kCombineChannelsNodeType)
        return QStringLiteral("RGB RGBA channel split pass");
    // #503: the names compositors search for.
    if (typeId == document::kAlphaAssociationNodeType)
        return QStringLiteral("unmult premultiply unpremultiply premult unpremult straight "
                              "remove color matting matte black white");
    // #504.
    if (typeId == document::kColorizeNodeType)
        return QStringLiteral("fill tint tritone duotone gradient map ramp colorama black white "
                              "monochrome greyscale grayscale toner channel mixer");
    // #507.
    if (typeId == document::kLensFlareNodeType)
        return QStringLiteral("lens flare glare sun light streak star ghost halo hoop iris ring "
                              "anamorphic optical");
    // #486.
    if (typeId == document::kFilmGrainNodeType)
        return QStringLiteral("film grain noise add finishing stock analogue analog regrain");
    // #563.
    if (typeId == document::kCodecDegradeNodeType)
        return QStringLiteral("codec degrade compression artifacts blocky blocks macroblock jpeg "
                              "h264 mpeg low bitrate chroma subsampling keyframe gop smear");
    // #486.
    if (typeId == document::kLensDistortionNodeType)
        return QStringLiteral("lens distortion undistort redistort distort barrel pincushion radial "
                              "tangential brown conrady calibration st map stmap warp fisheye");
    if (typeId == document::kChromaticAberrationNodeType)
        return QStringLiteral("chromatic aberration fringing fringe colour split rgb split lateral "
                              "lens dispersion red blue shift prism");
    // #587.
    if (typeId == document::kTestCardNodeType)
        return QStringLiteral("test card bars color bars grid zone plate "
                              "smpte ebu hdr hlg pq patch chart gray ramp pluge circle safe areas "
                              "uv siemens star moving bar countdown alpha ramp chart");
    if (typeId == document::kLightRaysNodeType)
        return QStringLiteral("light rays god rays shafts volumetric beams radial streaks burst "
                              "sun glare crepuscular");
    if (typeId == document::kRollingShutterNodeType)
        return QStringLiteral("rolling shutter jello skew wobble cmos readout sensor scan phone "
                              "camera shake distort");
    if (typeId == document::kLightFlickerNodeType)
        return QStringLiteral("light flicker flickering dropout bulb headlight sputter fluorescent "
                              "unsteady seeded noise exposure");
    if (typeId == document::kSharpenNodeType)
        return QStringLiteral("sharpen unsharp mask usm crisp detail edges finishing");
    if (typeId == document::kFrameRateHoldNodeType)
        return QStringLiteral("frame rate hold fps stutter step stepped low frame rate phone "
                              "camera irregular cadence dropped doubled frames retime");
    // #512.
    if (typeId == document::kCameraShakeNodeType)
        return QStringLiteral("camera shake handheld jitter wobble earthquake impact explosion "
                              "gate weave film gate bump shaky");
    // #505.
    if (typeId == document::kVignetteNodeType)
        return QStringLiteral("vignette lens falloff darken edges corners finishing exposure "
                              "light falloff");
    return {};
}

QStringList nodeSearchAliases(const std::string_view typeId) {
    for (const auto& entry : kNodeAliases)
        if (entry.typeId == typeId)
            return QString::fromUtf8(entry.names).split(QLatin1Char('|'));
    return {};
}

} // namespace bloom::ui::node_editor
