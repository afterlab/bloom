#pragma once

// #348 Corner Pin, ST Map and Displace: the durable vocabulary of the three nodes that move a picture
// by reading it somewhere else. Each output pixel is an inverse mapping: it picks the position of the
// input to read, and reads it. The three differ only in where that position comes from.
//
//   Corner Pin  four corners, each an offset in pixels from where the input's own corner is
//               (y down), pin the input's data window to a quad with perspective.
//   ST Map      the position is given as data: a map image holds, in two of its channels, the
//               position to read as a fraction of the composition frame (0, 0 the bottom left, 1, 1
//               the top right). Absolute coordinates.
//   Displace    the position is the pixel's own, moved by a map image: (value - midpoint) times an
//               amount, in pixels, in each direction. Relative displacement.
//
// A map is read as the data it is: the values the image holds, unclamped and without a colour
// conversion, and it never takes the transform of an input colour space. The filter and the edge
// rule are the field raster's (docs/architecture/fields.md): Nearest takes one texel exactly, which
// an ID or data image needs.
//
// docs/architecture/image-filters.md, "Corner Pin, ST Map and Displace", is the specification.
//
// Schema keys are "bloom.corner-pin.<role>", "bloom.st-map.<role>" and "bloom.displace.<role>". The
// corners, the amounts and the midpoint are animatable, drivable and have a socket of their own;
// every choice is a constant with no socket.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kCornerPinNodeType = "bloom.corner-pin";
inline constexpr std::string_view kStMapNodeType = "bloom.st-map";
inline constexpr std::string_view kDisplaceNodeType = "bloom.displace";
// #513 Stabilize and Match Move: the picture moved by what a track followed (docs/architecture/
// point-tracking.md). It shares the inverse mapping and the filter and edge choices of the three above.
inline constexpr std::string_view kStabilizeNodeType = "bloom.stabilize";
// #511 Tile & Mirror: the picture repeated, panned with wrap, reflected about a line, or folded into
// a kaleidoscope (docs/architecture/image-filters.md, "Tile & Mirror"). It shares the inverse mapping
// and the filter and edge choices of the nodes above.
inline constexpr std::string_view kTileMirrorNodeType = "bloom.tile-mirror";
// #510 Procedural Warp: one node, one mode at a time, each an inverse map from the output pixel to
// the position to read (docs/architecture/image-filters.md, "Procedural Warp"; the field
// operation's arithmetic is docs/architecture/fields.md, "Procedural warp"). It shares the
// inverse mapping and the filter and edge choices of the nodes above.
inline constexpr std::string_view kProceduralWarpNodeType = "bloom.procedural-warp";
inline constexpr std::uint32_t kWarpNodeSchemaVersion = 1;

inline constexpr std::string_view kWarpImagePort = "image";
inline constexpr std::string_view kWarpMapPort = "map";

// The filter, in the field raster's order, and the edge rule in the order the node offers (the
// lowering maps it to the raster's).
inline constexpr std::int64_t kWarpFilterBilinear = 0;
inline constexpr std::int64_t kWarpFilterNearest = 1;
inline constexpr std::int64_t kWarpFilterBicubic = 2;
inline constexpr std::int64_t kWarpEdgeTransparent = 0;
inline constexpr std::int64_t kWarpEdgeExtend = 1;
inline constexpr std::int64_t kWarpEdgeRepeat = 2;
inline constexpr std::int64_t kWarpEdgeMirror = 3;
// A channel of a map: red, green, blue, alpha (ST Map); Displace's choice has None in front.
inline constexpr std::int64_t kWarpChannelRed = 0;
inline constexpr std::int64_t kWarpChannelAlpha = 3;
inline constexpr std::int64_t kDisplaceChannelNone = 0;

// How far a corner can be moved, and how far an amount can reach, in pixels.
inline constexpr double kMaximumCornerPinOffset = 1.0e6;
inline constexpr double kMaximumDisplaceAmount = 1.0e5;
inline constexpr double kMaximumDisplaceMidpoint = 1.0e3;

// Stabilize's mode: hold the picture still against the tracked move, or carry it along with it.
inline constexpr std::int64_t kStabilizeModeStabilize = 0;
inline constexpr std::int64_t kStabilizeModeMatchMove = 1;

// Tile & Mirror's mode and Mirror's side.
inline constexpr std::int64_t kTileMirrorModeTile = 0;
inline constexpr std::int64_t kTileMirrorModeWrapOffset = 1;
inline constexpr std::int64_t kTileMirrorModeMirror = 2;
inline constexpr std::int64_t kTileMirrorModeKaleidoscope = 3;
inline constexpr std::int64_t kTileMirrorSideLeftBelow = 0;
inline constexpr std::int64_t kTileMirrorSideRightAbove = 1;
inline constexpr std::int64_t kMinimumKaleidoscopeSegments = 1;
inline constexpr std::int64_t kMaximumKaleidoscopeSegments = 64;
// Tile & Mirror's domains: a size in percent of the composition frame, a phase or an angle in
// degrees.
inline constexpr double kMinimumTilePercent = 0.01;
inline constexpr double kMaximumTilePercent = 1.0e5;
inline constexpr double kMaximumTileAngle = 1.0e6;

// Procedural Warp's modes, as stored (a new mode is added at the end), and the choices of its modes.
inline constexpr std::int64_t kProceduralWarpTwirl = 0;
inline constexpr std::int64_t kProceduralWarpBulgePinch = 1;
inline constexpr std::int64_t kProceduralWarpSpherize = 2;
inline constexpr std::int64_t kProceduralWarpRipple = 3;
inline constexpr std::int64_t kProceduralWarpWave = 4;
inline constexpr std::int64_t kProceduralWarpPolar = 5;
inline constexpr std::int64_t kProceduralWarpMagnify = 6;
inline constexpr std::int64_t kProceduralWarpWarp = 7;
inline constexpr std::int64_t kProceduralWarpBend = 8;
inline constexpr std::int64_t kProceduralWarpSplit = 9;
inline constexpr std::int64_t kProceduralWarpTurbulent = 10;
inline constexpr std::int64_t kProceduralWarpDrizzle = 11;
inline constexpr std::int64_t kProceduralWarpLastMode = kProceduralWarpDrizzle;
inline constexpr std::int64_t kWarpStyleLast = 12;
inline constexpr std::int64_t kMaximumDrizzleDrops = 64;
inline constexpr std::int64_t kMaximumProceduralWarpSeed = 16777215;
inline constexpr std::int64_t kWarpPinNone = 0;
inline constexpr std::int64_t kWarpPinAllEdges = 1;
inline constexpr std::int64_t kWarpPinLastChoice = 7;
inline constexpr std::int64_t kPolarRectToPolar = 0;
inline constexpr std::int64_t kPolarPolarToRect = 1;
inline constexpr std::int64_t kMagnifyLinkNone = 0;
inline constexpr std::int64_t kMagnifyLinkSize = 1;
inline constexpr std::int64_t kMagnifyLinkSizeAndFeather = 2;
// Procedural Warp's domains: a length in pixels, an angle or a phase in degrees, a speed in cycles a
// second, a magnification in percent.
inline constexpr double kMaximumWarpLength = 1.0e5;
inline constexpr double kMaximumWarpAngle = 1.0e6;
inline constexpr double kMaximumWarpSpeed = 1.0e3;
inline constexpr double kMaximumMagnification = 1.0e4;

// A Choice, an Integer (a whole number, a constant), a Scalar (animatable) or a Boolean (a switch,
// a constant).
enum class WarpParameterKind : std::uint8_t { Choice, Scalar, Integer, Boolean };

struct WarpParameterSpec final {
    std::string_view role;
    WarpParameterKind kind = WarpParameterKind::Scalar;
    double value = 0.0;
    // The document domain.
    double minimum = 0.0;
    double maximum = 0.0;
    std::span<const std::string_view> choices{};
    // The control's label and presentation: the span its slider covers, one displayed step, the
    // decimals shown and a unit.
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 0.01;
    int decimals = 2;
    std::string_view unit{};
};

namespace warp_parameters {

using Kind = WarpParameterKind;

inline constexpr std::array<std::string_view, 3> kFilters{"Bilinear", "Nearest", "Bicubic"};
inline constexpr std::array<std::string_view, 4> kEdges{"Transparent", "Extend", "Repeat", "Mirror"};
inline constexpr std::array<std::string_view, 4> kChannels{"Red", "Green", "Blue", "Alpha"};
inline constexpr std::array<std::string_view, 5> kOptionalChannels{"None", "Red", "Green", "Blue",
                                                                   "Alpha"};

inline constexpr std::array<std::string_view, 2> kStabilizeModes{"Stabilize", "Match Move"};
inline constexpr std::array<std::string_view, 3> kPatternSizes{"Small", "Medium", "Large"};
inline constexpr std::array<std::string_view, 3> kSearchSizes{"Narrow", "Normal", "Wide"};
inline constexpr std::array kStabilize{
    WarpParameterSpec{"mode", Kind::Choice, 0.0, 0.0, 1.0, kStabilizeModes, "Mode"},
    WarpParameterSpec{"pattern", Kind::Choice, 1.0, 0.0, 2.0, kPatternSizes, "Pattern"},
    WarpParameterSpec{"search", Kind::Choice, 1.0, 0.0, 2.0, kSearchSizes, "Search"},
    WarpParameterSpec{"filter", Kind::Choice, 0.0, 0.0, 2.0, kFilters, "Filter"},
    WarpParameterSpec{"edges", Kind::Choice, 0.0, 0.0, 3.0, kEdges, "Edges"},
};
inline constexpr std::array<std::string_view, 4> kTileMirrorModes{"Tile", "Wrap Offset",
                                                                  "Mirror", "Kaleidoscope"};
inline constexpr std::array<std::string_view, 2> kMirrorSides{"Left or Below", "Right or Above"};
// Every parameter of Tile & Mirror that is not a point, in the order its rows read. A size is a
// percentage of the composition frame; a phase and an angle are degrees.
inline constexpr std::array kTileMirror{
    WarpParameterSpec{"mode", Kind::Choice, 0.0, 0.0, 3.0, kTileMirrorModes, "Mode"},
    WarpParameterSpec{"filter", Kind::Choice, 0.0, 0.0, 2.0, kFilters, "Filter"},
    WarpParameterSpec{"edges", Kind::Choice, 0.0, 0.0, 3.0, kEdges, "Edges"},
    WarpParameterSpec{"tileWidth", Kind::Scalar, 100.0, kMinimumTilePercent, kMaximumTilePercent,
                      {}, "Tile Width", 1.0, 400.0, 1.0, 1, "%"},
    WarpParameterSpec{"tileHeight", Kind::Scalar, 100.0, kMinimumTilePercent, kMaximumTilePercent,
                      {}, "Tile Height", 1.0, 400.0, 1.0, 1, "%"},
    WarpParameterSpec{"linkTileSize", Kind::Boolean, 0.0, 0.0, 1.0, {}, "Link Tile Size"},
    WarpParameterSpec{"outputWidth", Kind::Scalar, 100.0, 0.0, kMaximumTilePercent, {},
                      "Output Width", 0.0, 400.0, 1.0, 1, "%"},
    WarpParameterSpec{"outputHeight", Kind::Scalar, 100.0, 0.0, kMaximumTilePercent, {},
                      "Output Height", 0.0, 400.0, 1.0, 1, "%"},
    WarpParameterSpec{"clampToOutput", Kind::Boolean, 1.0, 0.0, 1.0, {}, "Clamp to Output"},
    WarpParameterSpec{"mirrorEdges", Kind::Boolean, 0.0, 0.0, 1.0, {}, "Mirror Edges"},
    WarpParameterSpec{"horizontalPhase", Kind::Scalar, 0.0, -kMaximumTileAngle, kMaximumTileAngle,
                      {}, "Horizontal Phase", -360.0, 360.0, 1.0, 1, "\xc2\xb0"},
    WarpParameterSpec{"verticalPhase", Kind::Scalar, 0.0, -kMaximumTileAngle, kMaximumTileAngle,
                      {}, "Vertical Phase", -360.0, 360.0, 1.0, 1, "\xc2\xb0"},
    WarpParameterSpec{"mirrorAngle", Kind::Scalar, 90.0, -kMaximumTileAngle, kMaximumTileAngle,
                      {}, "Line Angle", -360.0, 360.0, 1.0, 1, "\xc2\xb0"},
    WarpParameterSpec{"mirrorSide", Kind::Choice, 0.0, 0.0, 1.0, kMirrorSides, "Reflect"},
    WarpParameterSpec{"kaleidoscopeSegments", Kind::Integer, 6.0,
                      static_cast<double>(kMinimumKaleidoscopeSegments),
                      static_cast<double>(kMaximumKaleidoscopeSegments), {}, "Segments", 1.0,
                      64.0, 1.0, 0},
    WarpParameterSpec{"kaleidoscopeAngle", Kind::Scalar, 0.0, -kMaximumTileAngle,
                      kMaximumTileAngle, {}, "Rotation", -360.0, 360.0, 1.0, 1, "\xc2\xb0"},
    WarpParameterSpec{"kaleidoscopeSize", Kind::Scalar, 100.0, 1.0, kMaximumTilePercent, {},
                      "Size", 10.0, 400.0, 1.0, 1, "%"},
};
// Its four points, in pixels from the frame's centre, y down: the tile's centre, the wrap offset's
// shift, a point on the mirror line, the kaleidoscope's centre.
inline constexpr std::array<std::string_view, 4> kTileMirrorPointRoles{
    "tileCenter", "offset", "mirrorPoint", "kaleidoscopeCenter"};
inline constexpr std::array<std::string_view, 4> kTileMirrorPointLabels{
    "Tile Center", "Offset", "Line Point", "Center"};
inline constexpr std::array<std::string_view, 12> kProceduralWarpModes{
    "Twirl", "Bulge / Pinch", "Spherize / Lens", "Ripple", "Wave", "Polar Coordinates", "Magnify",
    "Warp", "Bend", "Split", "Turbulent", "Drizzle"};
inline constexpr std::array<std::string_view, 13> kWarpStyles{
    "Arc", "Arc Lower", "Arc Upper", "Arch", "Bulge", "Flag", "Wave",
    "Fish", "Rise", "Fisheye", "Inflate", "Squeeze", "Twist"};
inline constexpr std::array<std::string_view, 4> kWaveShapes{"Sine", "Triangle", "Square", "Saw"};
// The frame edges a Wave holds still (the order of the choice; the lowering turns it into a mask).
inline constexpr std::array<std::string_view, 8> kWavePins{
    "None", "All Edges", "Left", "Right", "Top", "Bottom", "Left and Right", "Top and Bottom"};
inline constexpr std::array<std::string_view, 2> kPolarDirections{"Rectangular to Polar",
                                                                  "Polar to Rectangular"};
inline constexpr std::array<std::string_view, 2> kMagnifyShapes{"Circle", "Square"};
inline constexpr std::array<std::string_view, 3> kMagnifyLinks{"Nothing", "Size", "Size and Feather"};
// Every parameter of Procedural Warp that is not a point, in the order its rows read. Lengths are
// pixels of the full-resolution frame; Falloff, Amount, Interpolation and Magnification are
// percent; angles, Phase and Direction are degrees (Angle turns the picture clockwise, Direction is
// clockwise from up); Speed is cycles a second. Each mode reads only its own rows.
inline constexpr std::array kProceduralWarp{
    WarpParameterSpec{"mode", Kind::Choice, 0.0, 0.0, static_cast<double>(kProceduralWarpLastMode),
                      kProceduralWarpModes, "Mode"},
    WarpParameterSpec{"filter", Kind::Choice, 0.0, 0.0, 2.0, kFilters, "Filter"},
    WarpParameterSpec{"edges", Kind::Choice, 0.0, 0.0, 3.0, kEdges, "Edges"},
    WarpParameterSpec{"radius", Kind::Scalar, 300.0, 0.0, kMaximumWarpLength, {}, "Radius", 0.0,
                      1500.0, 1.0, 1, "px"},
    WarpParameterSpec{"falloff", Kind::Scalar, 100.0, 0.0, 100.0, {}, "Falloff", 0.0, 100.0, 1.0,
                      1, "%"},
    WarpParameterSpec{"angle", Kind::Scalar, 90.0, -kMaximumWarpAngle, kMaximumWarpAngle, {},
                      "Angle", -720.0, 720.0, 1.0, 1, "\xc2\xb0"},
    WarpParameterSpec{"amount", Kind::Scalar, 50.0, -100.0, 100.0, {}, "Amount", -100.0, 100.0,
                      1.0, 1, "%"},
    WarpParameterSpec{"refraction", Kind::Scalar, 1.5, 1.0, 4.0, {}, "Refraction Index", 1.0, 3.0,
                      0.01, 2},
    WarpParameterSpec{"wavelength", Kind::Scalar, 40.0, 1.0, kMaximumWarpLength, {}, "Wavelength",
                      2.0, 400.0, 1.0, 1, "px"},
    WarpParameterSpec{"amplitude", Kind::Scalar, 10.0, -kMaximumWarpLength, kMaximumWarpLength, {},
                      "Amplitude", -100.0, 100.0, 0.5, 1, "px"},
    WarpParameterSpec{"phase", Kind::Scalar, 0.0, -kMaximumWarpAngle, kMaximumWarpAngle, {},
                      "Phase", -360.0, 360.0, 1.0, 1, "\xc2\xb0"},
    WarpParameterSpec{"speed", Kind::Scalar, 0.0, -kMaximumWarpSpeed, kMaximumWarpSpeed, {},
                      "Speed", -5.0, 5.0, 0.05, 2, "cycles/s"},
    WarpParameterSpec{"direction", Kind::Scalar, 90.0, -kMaximumWarpAngle, kMaximumWarpAngle, {},
                      "Direction", 0.0, 360.0, 1.0, 1, "\xc2\xb0"},
    WarpParameterSpec{"waveShape", Kind::Choice, 0.0, 0.0, 3.0, kWaveShapes, "Wave"},
    WarpParameterSpec{"pinning", Kind::Choice, 0.0, 0.0, static_cast<double>(kWarpPinLastChoice),
                      kWavePins, "Pin Edges"},
    WarpParameterSpec{"polarDirection", Kind::Choice, 0.0, 0.0, 1.0, kPolarDirections,
                      "Conversion"},
    WarpParameterSpec{"interpolation", Kind::Scalar, 100.0, 0.0, 100.0, {}, "Interpolation", 0.0,
                      100.0, 1.0, 1, "%"},
    WarpParameterSpec{"magnifyShape", Kind::Choice, 0.0, 0.0, 1.0, kMagnifyShapes, "Shape"},
    WarpParameterSpec{"magnification", Kind::Scalar, 200.0, 1.0, kMaximumMagnification, {},
                      "Magnification", 100.0, 800.0, 1.0, 1, "%"},
    WarpParameterSpec{"magnifyLink", Kind::Choice, 0.0, 0.0, 2.0, kMagnifyLinks,
                      "Grow with Magnification"},
    WarpParameterSpec{"feather", Kind::Scalar, 0.0, 0.0, kMaximumWarpLength, {}, "Feather", 0.0,
                      200.0, 1.0, 1, "px"},
    // Magnify's two-point pull and Ripple's pulse.
    WarpParameterSpec{"pull", Kind::Scalar, 0.0, -100.0, 100.0, {}, "Pull", -100.0, 100.0, 1.0, 1,
                      "%"},
    WarpParameterSpec{"pulseRadius", Kind::Scalar, 0.0, 0.0, kMaximumWarpLength, {}, "Pulse Radius",
                      0.0, 1500.0, 1.0, 1, "px"},
    WarpParameterSpec{"pulseSpeed", Kind::Scalar, 0.0, -kMaximumWarpLength, kMaximumWarpLength, {},
                      "Pulse Speed", -600.0, 600.0, 1.0, 1, "px/s"},
    // Warp and Bend.
    WarpParameterSpec{"warpStyle", Kind::Choice, 0.0, 0.0, static_cast<double>(kWarpStyleLast),
                      kWarpStyles, "Style"},
    WarpParameterSpec{"bend", Kind::Scalar, 50.0, -100.0, 100.0, {}, "Bend", -100.0, 100.0, 1.0, 1,
                      "%"},
    WarpParameterSpec{"horizontalDistortion", Kind::Scalar, 0.0, -100.0, 100.0, {},
                      "Horizontal Distortion", -100.0, 100.0, 1.0, 1, "%"},
    WarpParameterSpec{"verticalDistortion", Kind::Scalar, 0.0, -100.0, 100.0, {},
                      "Vertical Distortion", -100.0, 100.0, 1.0, 1, "%"},
    WarpParameterSpec{"height", Kind::Scalar, 400.0, 0.0, kMaximumWarpLength, {}, "Height", 0.0,
                      1500.0, 1.0, 1, "px"},
    // Split.
    WarpParameterSpec{"splitDistance", Kind::Scalar, 40.0, 0.0, kMaximumWarpLength, {},
                      "Split Distance", 0.0, 400.0, 1.0, 1, "px"},
    WarpParameterSpec{"mirror", Kind::Boolean, 0.0, 0.0, 1.0, {}, "Mirror"},
    // Turbulent and Drizzle.
    WarpParameterSpec{"displacement", Kind::Scalar, 50.0, -kMaximumWarpLength, kMaximumWarpLength,
                      {}, "Displacement", -200.0, 200.0, 0.5, 1, "px"},
    WarpParameterSpec{"complexity", Kind::Scalar, 3.0, 1.0, 8.0, {}, "Complexity", 1.0, 8.0, 0.1, 1},
    WarpParameterSpec{"evolution", Kind::Scalar, 0.0, -1.0e4, 1.0e4, {}, "Evolution", 0.0, 10.0,
                      0.05, 2},
    WarpParameterSpec{"seed", Kind::Integer, 1.0, 0.0, static_cast<double>(kMaximumProceduralWarpSeed),
                      {}, "Seed", 0.0, 999999.0},
    WarpParameterSpec{"drops", Kind::Integer, 12.0, 1.0, static_cast<double>(kMaximumDrizzleDrops),
                      {}, "Drops", 1.0, 64.0},
};
// Its points, in pixels from the frame's centre, y down: the centre of a radial mode and of Bend,
// the point Magnify's pull draws the picture towards, Split's line from its start to its end, and
// Turbulent's noise size (a size, not a place) and offset.
inline constexpr std::array<std::string_view, 6> kProceduralWarpPointRoles{
    "center", "pullPoint", "lineStart", "lineEnd", "noiseSize", "noiseOffset"};
inline constexpr std::array<std::string_view, 6> kProceduralWarpPointLabels{
    "Center", "Pull Point", "Line Start", "Line End", "Noise Size", "Noise Offset"};
// Each point's value on a new node: a vertical line through the middle, the pull to the right, a
// noise of 100 px.
inline constexpr std::array<std::array<double, 2>, 6> kProceduralWarpPointDefaults{
    {{0.0, 0.0}, {200.0, 0.0}, {0.0, 200.0}, {0.0, -200.0}, {100.0, 100.0}, {0.0, 0.0}}};
inline constexpr std::array kCornerPin{
    WarpParameterSpec{"filter", Kind::Choice, 0.0, 0.0, 2.0, kFilters, "Filter"},
};
inline constexpr std::array kStMap{
    WarpParameterSpec{"uChannel", Kind::Choice, 0.0, 0.0, 3.0, kChannels, "U Channel"},
    WarpParameterSpec{"vChannel", Kind::Choice, 1.0, 0.0, 3.0, kChannels, "V Channel"},
    WarpParameterSpec{"filter", Kind::Choice, 0.0, 0.0, 2.0, kFilters, "Filter"},
    WarpParameterSpec{"edges", Kind::Choice, 0.0, 0.0, 3.0, kEdges, "Edges"},
};
inline constexpr std::array kDisplace{
    WarpParameterSpec{"xChannel", Kind::Choice, 1.0, 0.0, 4.0, kOptionalChannels, "X Channel"},
    WarpParameterSpec{"yChannel", Kind::Choice, 2.0, 0.0, 4.0, kOptionalChannels, "Y Channel"},
    WarpParameterSpec{"xAmount", Kind::Scalar, 20.0, -kMaximumDisplaceAmount,
                      kMaximumDisplaceAmount, {}, "X Amount", -200.0, 200.0, 1.0, 1, "px"},
    WarpParameterSpec{"yAmount", Kind::Scalar, 20.0, -kMaximumDisplaceAmount,
                      kMaximumDisplaceAmount, {}, "Y Amount", -200.0, 200.0, 1.0, 1, "px"},
    WarpParameterSpec{"midpoint", Kind::Scalar, 0.5, -kMaximumDisplaceMidpoint,
                      kMaximumDisplaceMidpoint, {}, "Midpoint", -1.0, 1.0, 0.01, 3},
    WarpParameterSpec{"filter", Kind::Choice, 0.0, 0.0, 2.0, kFilters, "Filter"},
    WarpParameterSpec{"edges", Kind::Choice, 0.0, 0.0, 3.0, kEdges, "Edges"},
};

// The four corners, top left first, as the roles of Corner Pin's offsets.
inline constexpr std::array<std::string_view, 4> kCornerRoles{"topLeft", "topRight", "bottomRight",
                                                              "bottomLeft"};
inline constexpr std::array<std::string_view, 4> kCornerLabels{"Top Left", "Top Right",
                                                               "Bottom Right", "Bottom Left"};

// Stabilize's two points, in pixels from the frame's centre, y down: the feature as chosen, and
// where the track found it in each frame.
inline constexpr std::array<std::string_view, 2> kStabilizePointRoles{"trackPoint", "tracked"};
inline constexpr std::array<std::string_view, 2> kStabilizePointLabels{"Track Point", "Tracked Position"};
} // namespace warp_parameters

[[nodiscard]] constexpr std::span<const WarpParameterSpec> stabilizeParameters() noexcept {
    return warp_parameters::kStabilize;
}
[[nodiscard]] constexpr std::span<const std::string_view> stabilizePointRoles() noexcept {
    return warp_parameters::kStabilizePointRoles;
}
[[nodiscard]] constexpr std::span<const std::string_view> stabilizePointLabels() noexcept {
    return warp_parameters::kStabilizePointLabels;
}
// The half side, in pixels, of the pattern and of the search a choice names.
[[nodiscard]] constexpr int stabilizePatternRadius(const std::int64_t choice) noexcept {
    return choice <= 0 ? 8 : (choice == 1 ? 16 : 32);
}
[[nodiscard]] constexpr int stabilizeSearchRadius(const std::int64_t choice) noexcept {
    return choice <= 0 ? 16 : (choice == 1 ? 32 : 64);
}
[[nodiscard]] constexpr std::span<const WarpParameterSpec> tileMirrorParameters() noexcept {
    return warp_parameters::kTileMirror;
}
[[nodiscard]] constexpr std::span<const std::string_view> tileMirrorPointRoles() noexcept {
    return warp_parameters::kTileMirrorPointRoles;
}
[[nodiscard]] constexpr std::span<const std::string_view> tileMirrorPointLabels() noexcept {
    return warp_parameters::kTileMirrorPointLabels;
}
[[nodiscard]] constexpr std::span<const WarpParameterSpec> proceduralWarpParameters() noexcept {
    return warp_parameters::kProceduralWarp;
}
[[nodiscard]] constexpr std::span<const std::string_view> proceduralWarpPointRoles() noexcept {
    return warp_parameters::kProceduralWarpPointRoles;
}
[[nodiscard]] constexpr std::span<const std::string_view> proceduralWarpPointLabels() noexcept {
    return warp_parameters::kProceduralWarpPointLabels;
}
[[nodiscard]] constexpr std::span<const WarpParameterSpec> cornerPinParameters() noexcept {
    return warp_parameters::kCornerPin;
}
[[nodiscard]] constexpr std::span<const WarpParameterSpec> stMapParameters() noexcept {
    return warp_parameters::kStMap;
}
[[nodiscard]] constexpr std::span<const WarpParameterSpec> displaceParameters() noexcept {
    return warp_parameters::kDisplace;
}
[[nodiscard]] constexpr std::span<const std::string_view> cornerPinCornerRoles() noexcept {
    return warp_parameters::kCornerRoles;
}
[[nodiscard]] constexpr std::span<const std::string_view> cornerPinCornerLabels() noexcept {
    return warp_parameters::kCornerLabels;
}

// The spec of a "bloom.corner-pin.<role>", "bloom.st-map.<role>" or "bloom.displace.<role>" schema
// key, or null (a corner has none: it is a position).
[[nodiscard]] constexpr const WarpParameterSpec* warpParameterSpec(const std::string_view key) noexcept {
    const auto find = [key](const std::string_view prefix, const auto& table)
        -> const WarpParameterSpec* {
        if (!key.starts_with(prefix))
            return nullptr;
        const auto role = key.substr(prefix.size());
        for (const auto& spec : table)
            if (spec.role == role)
                return &spec;
        return nullptr;
    };
    if (const auto* spec = find("bloom.corner-pin.", warp_parameters::kCornerPin))
        return spec;
    if (const auto* spec = find("bloom.st-map.", warp_parameters::kStMap))
        return spec;
    if (const auto* spec = find("bloom.stabilize.", warp_parameters::kStabilize))
        return spec;
    if (const auto* spec = find("bloom.tile-mirror.", warp_parameters::kTileMirror))
        return spec;
    if (const auto* spec = find("bloom.procedural-warp.", warp_parameters::kProceduralWarp))
        return spec;
    return find("bloom.displace.", warp_parameters::kDisplace);
}

// Whether `key` is a point in pixels: one of Corner Pin's four corner offsets, or one of Stabilize's
// two points.
[[nodiscard]] constexpr bool isWarpPointSchemaKey(const std::string_view key) noexcept {
    constexpr std::string_view corner = "bloom.corner-pin.";
    constexpr std::string_view stabilize = "bloom.stabilize.";
    constexpr std::string_view tileMirror = "bloom.tile-mirror.";
    constexpr std::string_view proceduralWarp = "bloom.procedural-warp.";
    if (key.starts_with(proceduralWarp)) {
        const auto role = key.substr(proceduralWarp.size());
        for (const auto name : warp_parameters::kProceduralWarpPointRoles)
            if (name == role)
                return true;
    } else if (key.starts_with(tileMirror)) {
        const auto role = key.substr(tileMirror.size());
        for (const auto name : warp_parameters::kTileMirrorPointRoles)
            if (name == role)
                return true;
    } else if (key.starts_with(corner)) {
        const auto role = key.substr(corner.size());
        for (const auto name : warp_parameters::kCornerRoles)
            if (name == role)
                return true;
    } else if (key.starts_with(stabilize)) {
        const auto role = key.substr(stabilize.size());
        for (const auto name : warp_parameters::kStabilizePointRoles)
            if (name == role)
                return true;
    }
    return false;
}

[[nodiscard]] constexpr bool isWarpScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = warpParameterSpec(key);
    return spec != nullptr && spec->kind == WarpParameterKind::Scalar;
}
// A constant (every choice, whole number and switch): no socket and no animation.
[[nodiscard]] constexpr bool isWarpConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = warpParameterSpec(key);
    return spec != nullptr && spec->kind != WarpParameterKind::Scalar;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isWarpScalarWithinDomain(const std::string_view key,
                                                      const double value) noexcept {
    const auto* spec = warpParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document
