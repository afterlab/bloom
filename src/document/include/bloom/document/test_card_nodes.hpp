#pragma once

// #587 Test Card: the durable vocabulary of the source node that draws a test pattern. It has no
// input and one Image output. A Card choice selects the pattern; every card is drawn procedurally at
// render resolution, so the size is never limited by a stored raster. The size is the composition's
// unless Size is Custom. docs/user-guide/test-card.md is the specification of every card and the
// source of every standard value it uses.
//
// Schema keys are "bloom.test-card.<role>". Every parameter is a constant with no socket. A card shows
// only the parameters that name it (testCardParameterRelevant()).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kTestCardNodeType = "bloom.test-card";
inline constexpr std::uint32_t kTestCardNodeSchemaVersion = 1;
inline constexpr std::string_view kTestCardImagePort = "image";

// The cards, in the order of the Card choice (the kernel's card numbers).
enum class TestCard : std::int64_t {
    SdBars = 0,
    HdBars,
    EbuBars,
    HdrBars,
    Patch,
    ColorChart,
    GrayRamp,
    Grid,
    Circle,
    SafeAreas,
    UvGrid,
    ZonePlate,
    SiemensStar,
    MovingBar,
    Countdown,
    AlphaRamp,
};
inline constexpr std::int64_t kTestCardCount = 16;

inline constexpr std::int64_t kTestCardSizeComposition = 0;
inline constexpr std::int64_t kTestCardSizeCustom = 1;

// The signal color spaces of a Patch.
inline constexpr std::int64_t kPatchSpaceRec709 = 0;
inline constexpr std::int64_t kPatchSpaceHlg = 1;
inline constexpr std::int64_t kPatchSpacePq = 2;
inline constexpr std::int64_t kPatchSpaceWorking = 3;

// The widest custom frame a document admits; the engine's own pixel budget refuses what it cannot
// hold, with its normal explicit diagnostic.
inline constexpr std::int64_t kMaximumTestCardPixels = 65536;

enum class TestCardParameterKind : std::uint8_t { Choice, Whole, Scalar, Boolean };

struct TestCardParameterSpec final {
    std::string_view role;
    TestCardParameterKind kind = TestCardParameterKind::Whole;
    double value = 0.0;
    // The document domain.
    double minimum = 0.0;
    double maximum = 0.0;
    std::span<const std::string_view> choices{};
    // The control's label and presentation.
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 1.0;
    int decimals = 0;
    std::string_view unit{};
};

namespace test_card_parameters {

using Kind = TestCardParameterKind;

inline constexpr std::array<std::string_view, 16> kCards{
    "SD Bars", "HD Bars",      "EBU Bars", "HDR Bars",     "Patch",   "Color Chart",
    "Gray Ramp", "Grid",       "Circle",   "Safe Areas",   "UV Grid", "Zone Plate",
    "Siemens Star", "Moving Bar", "Countdown", "Alpha Ramp"};
inline constexpr std::array<std::string_view, 2> kSizes{"Composition", "Custom"};
inline constexpr std::array<std::string_view, 3> kHdrVariants{
    "HLG, narrow range", "PQ, narrow range", "PQ, full range"};
inline constexpr std::array<std::string_view, 27> kPatchValues{
    "Black 0%",       "Near black 2%",  "Near black 4%",  "Gray 10%",     "Gray 20%",
    "Gray 30%",       "Gray 40%",       "Gray 50%",       "Gray 60%",     "Gray 70%",
    "Gray 80%",       "Gray 90%",       "Near white 95%", "Near white 98%", "White 100%",
    "Red 75%",        "Green 75%",      "Blue 75%",       "Cyan 75%",     "Magenta 75%",
    "Yellow 75%",     "Red 100%",       "Green 100%",     "Blue 100%",    "Cyan 100%",
    "Magenta 100%",   "Yellow 100%"};
inline constexpr std::array<std::string_view, 4> kPatchWindows{
    "Full field", "Window, half the frame", "Window, quarter of the frame",
    "Window, a tenth of the frame"};
inline constexpr std::array<std::string_view, 4> kPatchSpaces{
    "Rec.709", "Rec.2100 HLG", "Rec.2100 PQ", "Working space (linear)"};
inline constexpr std::array<std::string_view, 2> kDirections{"Horizontal", "Vertical"};

inline constexpr std::array kParameters{
    TestCardParameterSpec{"card", Kind::Choice, 0.0, 0.0, 15.0, kCards, "Card"},
    TestCardParameterSpec{"sizeMode", Kind::Choice, 0.0, 0.0, 1.0, kSizes, "Size"},
    TestCardParameterSpec{"width", Kind::Whole, 1920.0, 1.0, 65536.0, {}, "Width", 1.0, 8192.0, 1.0,
                          0, "px"},
    TestCardParameterSpec{"height", Kind::Whole, 1080.0, 1.0, 65536.0, {}, "Height", 1.0, 8192.0,
                          1.0, 0, "px"},
    TestCardParameterSpec{"label", Kind::Boolean, 1.0, 0.0, 1.0, {}, "Label"},
    TestCardParameterSpec{"hdrVariant", Kind::Choice, 0.0, 0.0, 2.0, kHdrVariants, "HDR format"},
    TestCardParameterSpec{"patchValue", Kind::Choice, 14.0, 0.0, 26.0, kPatchValues, "Patch value"},
    TestCardParameterSpec{"patchWindow", Kind::Choice, 0.0, 0.0, 3.0, kPatchWindows, "Window"},
    TestCardParameterSpec{"patchStep", Kind::Boolean, 0.0, 0.0, 1.0, {}, "Step through"},
    TestCardParameterSpec{"patchSpace", Kind::Choice, 0.0, 0.0, 3.0, kPatchSpaces, "Color space"},
    TestCardParameterSpec{"gridSpacing", Kind::Scalar, 90.0, 2.0, 65536.0, {}, "Spacing", 8.0,
                          512.0, 1.0, 1, "px"},
    TestCardParameterSpec{"gridWidth", Kind::Scalar, 2.0, 0.25, 256.0, {}, "Line width", 0.5, 16.0,
                          0.25, 2, "px"},
    TestCardParameterSpec{"uvCells", Kind::Whole, 16.0, 2.0, 256.0, {}, "Cells across", 4.0, 64.0,
                          1.0, 0},
    TestCardParameterSpec{"starSpokes", Kind::Whole, 36.0, 4.0, 720.0, {}, "Spokes", 8.0, 180.0, 1.0,
                          0},
    TestCardParameterSpec{"zoneMoving", Kind::Boolean, 0.0, 0.0, 1.0, {}, "Moving"},
    TestCardParameterSpec{"barWidth", Kind::Scalar, 4.0, 0.1, 100.0, {}, "Bar width", 0.5, 25.0, 0.1,
                          1, "% of the frame"},
    TestCardParameterSpec{"barDuration", Kind::Scalar, 2.0, 0.1, 600.0, {}, "Crossing time", 0.25,
                          10.0, 0.05, 2, "s"},
    TestCardParameterSpec{"barDirection", Kind::Choice, 0.0, 0.0, 1.0, kDirections, "Direction"},
    TestCardParameterSpec{"countdownStart", Kind::Whole, 10.0, 1.0, 3600.0, {}, "Start", 1.0, 60.0,
                          1.0, 0, "s"},
    TestCardParameterSpec{"guide169", Kind::Boolean, 1.0, 0.0, 1.0, {}, "16:9 guide"},
    TestCardParameterSpec{"guide916", Kind::Boolean, 1.0, 0.0, 1.0, {}, "9:16 guide"},
    TestCardParameterSpec{"guide11", Kind::Boolean, 1.0, 0.0, 1.0, {}, "1:1 guide"},
    TestCardParameterSpec{"guide239", Kind::Boolean, 1.0, 0.0, 1.0, {}, "2.39:1 guide"},
};

} // namespace test_card_parameters

[[nodiscard]] constexpr std::span<const TestCardParameterSpec> testCardParameters() noexcept {
    return test_card_parameters::kParameters;
}

[[nodiscard]] constexpr const TestCardParameterSpec*
testCardParameterSpecForRole(const std::string_view role) noexcept {
    for (const auto& spec : test_card_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

// The spec of a "bloom.test-card.<role>" schema key, or null.
[[nodiscard]] constexpr const TestCardParameterSpec*
testCardParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.test-card.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    return testCardParameterSpecForRole(schemaKey.substr(prefix.size()));
}

// Every Test Card parameter is a constant with no socket and no animation.
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isTestCardScalarWithinDomain(const std::string_view key,
                                                          const double value) noexcept {
    const auto* spec = testCardParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

[[nodiscard]] constexpr bool isTestCardConstantSchemaKey(const std::string_view key) noexcept {
    return testCardParameterSpec(key) != nullptr;
}

// Whether the parameter `role` belongs to `card`: the controls a card shows. Card and Size are
// every card's; Width and Height are Size's (shown when it is Custom, testCardShowsRole()).
[[nodiscard]] constexpr bool testCardParameterRelevant(const std::int64_t card,
                                                       const std::string_view role) noexcept {
    if (role == "card" || role == "sizeMode" || role == "width" || role == "height")
        return true;
    switch (static_cast<TestCard>(card)) {
    case TestCard::HdrBars:
        return role == "hdrVariant";
    case TestCard::Patch:
        return role == "patchValue" || role == "patchWindow" || role == "patchStep" ||
               role == "patchSpace";
    case TestCard::Grid:
        return role == "label" || role == "gridSpacing" || role == "gridWidth";
    case TestCard::Circle:
        return role == "label";
    case TestCard::SafeAreas:
        return role == "label" || role == "guide169" || role == "guide916" || role == "guide11" ||
               role == "guide239";
    case TestCard::UvGrid:
        return role == "label" || role == "uvCells";
    case TestCard::ZonePlate:
        return role == "label" || role == "zoneMoving";
    case TestCard::SiemensStar:
        return role == "label" || role == "starSpokes";
    case TestCard::MovingBar:
        return role == "label" || role == "barWidth" || role == "barDuration" ||
               role == "barDirection";
    case TestCard::Countdown:
        return role == "countdownStart";
    case TestCard::SdBars:
    case TestCard::HdBars:
    case TestCard::EbuBars:
    case TestCard::ColorChart:
    case TestCard::GrayRamp:
    case TestCard::AlphaRamp:
        return false;
    }
    return false;
}

// Whether the row of `role` is shown, given the card and the Size and Patch choices: Width and Height
// only for a Custom size, the Patch value only while the patch is not stepping.
[[nodiscard]] constexpr bool testCardShowsRole(const std::int64_t card, const std::int64_t size,
                                               const bool patchStep,
                                               const std::string_view role) noexcept {
    if (!testCardParameterRelevant(card, role))
        return false;
    if (role == "width" || role == "height")
        return size == kTestCardSizeCustom;
    if (role == "patchValue")
        return !patchStep;
    return true;
}

// The signal of the Patch value `index` (0 to 26) as straight red, green and blue, 0 black and 1 white
// of the card's color space; the numbers the kernel draws (field_kernel.glsl, bfk_tc_patch_value).
[[nodiscard]] constexpr std::array<double, 3> testCardPatchSignal(const std::int64_t index) noexcept {
    if (index <= 0)
        return {0.0, 0.0, 0.0};
    if (index == 1)
        return {0.02, 0.02, 0.02};
    if (index == 2)
        return {0.04, 0.04, 0.04};
    if (index <= 11) {
        const auto gray = static_cast<double>(index - 2) * 0.1;
        return {gray, gray, gray};
    }
    if (index == 12)
        return {0.95, 0.95, 0.95};
    if (index == 13)
        return {0.98, 0.98, 0.98};
    if (index == 14)
        return {1.0, 1.0, 1.0};
    const auto level = index < 21 ? 0.75 : 1.0;
    switch ((index - 15) % 6) {
    case 0:
        return {level, 0.0, 0.0};
    case 1:
        return {0.0, level, 0.0};
    case 2:
        return {0.0, 0.0, level};
    case 3:
        return {0.0, level, level};
    case 4:
        return {level, 0.0, level};
    default:
        return {level, level, 0.0};
    }
}

// The color space the card's signal is written in, named as the color configuration names it: the
// BT.709 camera encoding for every card but a Patch (which names its own) and the HDR bars (whose
// signal is the BT.2100 HLG or PQ encoding). Empty: the working space itself, no transform. These are
// the ids Bloom synthesizes into every built-in configuration (color/ocio_builtin_registry.hpp).
inline constexpr std::string_view kTestCardRec709Space = "Rec.709 Camera (Bloom)";
inline constexpr std::string_view kTestCardHlgSpace = "Rec.2100-HLG (Bloom)";
inline constexpr std::string_view kTestCardPqSpace = "Rec.2100-PQ (Bloom)";

[[nodiscard]] constexpr std::string_view
testCardSignalSpaceId(const std::int64_t card, const std::int64_t patchSpace,
                      const std::int64_t hdrVariant) noexcept {
    if (card == static_cast<std::int64_t>(TestCard::HdrBars))
        return hdrVariant == 0 ? kTestCardHlgSpace : kTestCardPqSpace;
    if (card != static_cast<std::int64_t>(TestCard::Patch))
        return kTestCardRec709Space;
    switch (patchSpace) {
    case kPatchSpaceHlg:
        return kTestCardHlgSpace;
    case kPatchSpacePq:
        return kTestCardPqSpace;
    case kPatchSpaceWorking:
        return {};
    default:
        return kTestCardRec709Space;
    }
}

} // namespace bloom::document
