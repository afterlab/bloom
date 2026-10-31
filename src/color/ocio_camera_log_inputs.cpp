#include "ocio_camera_log_inputs.hpp"

#include <array>

namespace bloom::color::detail {
namespace {
struct CameraLogSpace final {
    std::string_view name;
    std::string_view style;
};
// The camera log interpretations Interpret Footage offers in every pinned config.
constexpr std::array<CameraLogSpace, 11> kCameraLogSpaces{{
    {"S-Log3 SGamut3 (Bloom)", "SONY_SLOG3-SGAMUT3_to_ACES2065-1"},
    {"S-Log3 SG3.Cine (Bloom)", "SONY_SLOG3-SGAMUT3.CINE_to_ACES2065-1"},
    {"Venice SG3 (Bloom)", "SONY_SLOG3-SGAMUT3-VENICE_to_ACES2065-1"},
    {"Venice SG3.Cine (Bloom)", "SONY_SLOG3-SGAMUT3.CINE-VENICE_to_ACES2065-1"},
    {"LogC3 AWG3 (Bloom)", "ARRI_ALEXA-LOGC-EI800-AWG_to_ACES2065-1"},
    {"LogC4 AWG4 (Bloom)", "ARRI_LOGC4_to_ACES2065-1"},
    {"V-Log V-Gamut (Bloom)", "PANASONIC_VLOG-VGAMUT_to_ACES2065-1"},
    {"Log3G10 RWG (Bloom)", "RED_LOG3G10-RWG_to_ACES2065-1"},
    {"C-Log2 CGamut (Bloom)", "CANON_CLOG2-CGAMUT_to_ACES2065-1"},
    {"C-Log3 CGamut (Bloom)", "CANON_CLOG3-CGAMUT_to_ACES2065-1"},
    {"Apple Log (Bloom)", "APPLE_LOG_to_ACES2065-1"},
}};

[[nodiscard]] bool builtinAvailable(const std::string_view style) {
    const auto registry = OCIO::BuiltinTransformRegistry::Get();
    for (std::size_t index = 0; index < registry->getNumBuiltins(); ++index)
        if (style == registry->getBuiltinStyle(index))
            return true;
    return false;
}

// OCIO's implementation of a built-in, embedded as a group so a pinned config of any version can
// carry it: the exact ops the built-in runs, identical CPU output and GPU processor cache id.
[[nodiscard]] OCIO::GroupTransformRcPtr embeddedBuiltin(const std::string_view style,
                                                        const OCIO::TransformDirection direction) {
    auto raw = OCIO::Config::CreateRaw()->createEditableCopy();
    raw->setMajorVersion(2);
    raw->setMinorVersion(4);
    auto transform = OCIO::BuiltinTransform::Create();
    transform->setStyle(std::string(style).c_str());
    transform->setDirection(direction);
    return raw->getProcessor(transform)->createGroupTransform();
}

// Rec.709 (D65) RGB to CIE XYZ (SMPTE RP 177), the matrix the Rec.2100 spaces use.
[[nodiscard]] std::array<double, 16> rec709ToXyz() {
    constexpr std::array<double, 8> xy{0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290};
    const auto column = [](const double x, const double y) {
        return std::array<double, 3>{x / y, 1.0, (1.0 - x - y) / y};
    };
    const auto r = column(xy[0], xy[1]), g = column(xy[2], xy[3]), b = column(xy[4], xy[5]),
               w = column(xy[6], xy[7]);
    const std::array<double, 9> p{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const auto det = p[0] * (p[4] * p[8] - p[5] * p[7]) - p[1] * (p[3] * p[8] - p[5] * p[6]) +
                     p[2] * (p[3] * p[7] - p[4] * p[6]);
    const std::array<double, 9> inv{
        (p[4] * p[8] - p[5] * p[7]) / det, (p[2] * p[7] - p[1] * p[8]) / det,
        (p[1] * p[5] - p[2] * p[4]) / det, (p[5] * p[6] - p[3] * p[8]) / det,
        (p[0] * p[8] - p[2] * p[6]) / det, (p[2] * p[3] - p[0] * p[5]) / det,
        (p[3] * p[7] - p[4] * p[6]) / det, (p[1] * p[6] - p[0] * p[7]) / det,
        (p[0] * p[4] - p[1] * p[3]) / det};
    std::array<double, 3> scale{};
    for (std::size_t i = 0; i < 3; ++i)
        scale[i] = inv[i * 3] * w[0] + inv[i * 3 + 1] * w[1] + inv[i * 3 + 2] * w[2];
    std::array<double, 16> m{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t col = 0; col < 3; ++col)
            m[row * 4 + col] = p[row * 3 + col] * scale[col];
    m[15] = 1.0;
    return m;
}
} // namespace

OCIO::ConstConfigRcPtr withBloomCameraLogInputs(const OCIO::ConstConfigRcPtr& config,
                                                const bool isNeutral) {
    OCIO::ConfigRcPtr editable;
    for (const auto& camera : kCameraLogSpaces) {
        const std::string name(camera.name);
        if (config->getColorSpace(name.c_str()) != nullptr || !builtinAvailable(camera.style))
            continue;
        auto toReference = OCIO::GroupTransform::Create();
        toReference->appendTransform(embeddedBuiltin(camera.style, OCIO::TRANSFORM_DIR_FORWARD));
        if (isNeutral) {
            // Bloom Neutral's reference is linear Rec.709 (D65): ACES2065-1 -> XYZ D65 (Bradford)
            // -> Rec.709.
            toReference->appendTransform(embeddedBuiltin("UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD",
                                                         OCIO::TRANSFORM_DIR_FORWARD));
            auto toRec709 = OCIO::MatrixTransform::Create();
            const auto matrix = rec709ToXyz();
            toRec709->setMatrix(matrix.data());
            toRec709->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
            toReference->appendTransform(toRec709);
        }
        auto space = OCIO::ColorSpace::Create(OCIO::REFERENCE_SPACE_SCENE);
        space->setName(name.c_str());
        space->setFamily("Input");
        space->setDescription((std::string("Bloom-synthesized camera log input (OCIO ") +
                               std::string(camera.style) + ")")
                                  .c_str());
        space->setTransform(toReference, OCIO::COLORSPACE_DIR_TO_REFERENCE);
        if (!editable)
            editable = config->createEditableCopy();
        editable->addColorSpace(space);
    }
    if (!editable)
        return config;
    editable->validate();
    return editable;
}

std::string cameraLogColorSpaceIdentity(const std::string_view colorSpaceId) {
    for (const auto& camera : kCameraLogSpaces)
        if (camera.name == colorSpaceId)
            return std::string("bloom-camera-log-v1;") + std::string(camera.style) +
                   ";ocio=" + OCIO::GetVersion();
    return {};
}

} // namespace bloom::color::detail
