#include <bloom/runtime/value_utility_kernels.hpp>

#include <bloom/core/coordinate_space.hpp>

#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using runtime::CompiledValue;
using runtime::ValueUtilityOutcome;

using Kernel = document::ValueUtilityKernel;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// One kernel call from plain values, which is all a library node's evaluation is: the plan's
// bookkeeping is the Evaluator's, and every operation here is a pure function of its operands.
[[nodiscard]] ValueUtilityOutcome runAt(const document::FrameRate rate, const Kernel kernel,
                                        const std::vector<CompiledValue>& values,
                                        const std::vector<std::int64_t>& selectors = {},
                                        const runtime::ValueUtilityLimits limits = {}) {
    std::vector<const CompiledValue*> operands;
    operands.reserve(values.size());
    for (const auto& value : values) {
        operands.push_back(&value);
    }
    return runtime::evaluateValueUtility(
        {kernel, operands, selectors, core::RationalTime::fromInteger(0), rate, nullptr, limits});
}

[[nodiscard]] ValueUtilityOutcome run(const Kernel kernel, const std::vector<CompiledValue>& values,
                                      const std::vector<std::int64_t>& selectors = {},
                                      const runtime::ValueUtilityLimits limits = {}) {
    return runAt(document::FrameRate::framesPerSecond24(), kernel, values, selectors, limits);
}

// An unconnected Space input, as the compiler lowers one.
[[nodiscard]] CompiledValue noSpace() { return runtime::CompiledSpaceValue{}; }

template <typename Value>
[[nodiscard]] bool holds(const ValueUtilityOutcome& outcome, const std::size_t slot,
                         const Value& expected) {
    if (slot >= outcome.outputCount) {
        return false;
    }
    const auto* value = std::get_if<Value>(&outcome.outputs[slot]);
    return value != nullptr && *value == expected;
}

// Every conversion's declared shape, checked against the descriptor table that produced both the
// registered definition and this kernel's operand order. A kernel that read its operands in a
// different order than the node declares them would compute the right answer from the wrong values.
void testDescriptorShapes(Expectations& expectations) {
    for (const auto& descriptor : document::valueUtilityDescriptors()) {
        expectations.expect(!descriptor.typeId.empty() && !descriptor.displayName.empty(),
                            "every descriptor names a type and an artist-facing name");
        expectations.expect(!descriptor.outputs.empty() &&
                                descriptor.outputs.size() <= runtime::kMaximumValueUtilityOutputs,
                            "every descriptor declares between one and four outputs");
        for (const auto& operand : descriptor.operands) {
            expectations.expect(operand.kind != document::SocketValueKind::Image,
                                "a value node never carries pixels");
        }
        expectations.expect(document::findValueUtilityDescriptor(descriptor.kernel) == &descriptor,
                            "a kernel names exactly one descriptor");
        expectations.expect(document::findValueUtilityDescriptor(descriptor.typeId) == &descriptor,
                            "a type id names exactly one descriptor");
    }
    // The safe-parse contract's own shape: a parsing node has a fallback operand and a `valid`
    // output, and neither is optional.
    for (const auto kernel : {Kernel::StringToScalar, Kernel::StringToInteger,
                              Kernel::StringToBoolean, Kernel::StringToColor}) {
        const auto* descriptor = document::findValueUtilityDescriptor(kernel);
        const bool shaped = descriptor != nullptr && descriptor->operands.size() == 2 &&
                            descriptor->operands[0].role == document::kTextPortName &&
                            descriptor->operands[0].kind == document::SocketValueKind::String &&
                            descriptor->operands[1].role == document::kFallbackPortName &&
                            descriptor->outputs.size() == 2 &&
                            descriptor->outputs[0].name == document::kValuePortName &&
                            descriptor->outputs[1].name == document::kValidPortName &&
                            descriptor->outputs[1].kind == document::SocketValueKind::Boolean &&
                            descriptor->outputs[0].kind == descriptor->operands[1].kind;
        expectations.expect(shaped,
                            "every parsing node carries a fallback operand and a valid flag");
    }
}

void testScalarAndIntegerToString(Expectations& expectations) {
    struct ScalarCase final {
        double value;
        std::int64_t decimals;
        bool trim;
        std::int64_t padWidth;
        std::string_view prefix;
        std::string_view suffix;
        std::string_view text;
    };
    static const ScalarCase kCases[]{
        {1.5, 2, false, 0, "", "", "1.50"},
        {1.5, 2, true, 0, "", "", "1.5"},
        {12.0, 0, false, 5, "", "", "00012"},
        {-3.25, 2, false, 0, "x = ", " px", "x = -3.25 px"},
        {0.0, 3, true, 0, "", "", "0"},
        // Negative precision means zero; high precision is retained before trailing zeros trim.
        {2.5, -4, false, 0, "", "", "2"},
        {2.5, 1000, true, 0, "", "", "2.5"},
        // Non-finite operands have no decimal spelling, so the number part is empty and the
        // decoration the artist authored still appears.
        {std::numeric_limits<double>::infinity(), 2, false, 0, "[", "]", "[]"},
        {std::numeric_limits<double>::quiet_NaN(), 2, false, 0, "", "", ""},
    };
    for (const auto& item : kCases) {
        const auto outcome =
            run(Kernel::ScalarToString, {item.value, item.decimals, item.trim, item.padWidth,
                                         std::string(item.prefix), std::string(item.suffix)});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.text)),
                            "Scalar To String writes its documented spelling");
    }
    const auto small = run(Kernel::ScalarToString, {1e-18, std::int64_t{18}, false, std::int64_t{0},
                                                    std::string{}, std::string{}});
    expectations.expect(!small.failed && holds(small, 0, std::string("0.000000000000000001")),
                        "18 decimal places preserve a representable small scalar");
    const auto wide = run(Kernel::IntegerToString,
                          {std::int64_t{7}, std::int64_t{257}, std::string{}, std::string{}});
    const auto* wideText = std::get_if<std::string>(&wide.outputs[0]);
    expectations.expect(!wide.failed && wideText != nullptr && wideText->size() == 257 &&
                            wideText->back() == '7',
                        "integer formatting honors width beyond 256");
    const auto wideScalar =
        run(Kernel::ScalarToString,
            {7.0, std::int64_t{0}, false, std::int64_t{257}, std::string{}, std::string{}});
    const auto* scalarText = std::get_if<std::string>(&wideScalar.outputs[0]);
    expectations.expect(!wideScalar.failed && scalarText != nullptr && scalarText->size() == 257 &&
                            scalarText->back() == '7',
                        "scalar formatting honors width beyond 256");
    runtime::ValueUtilityLimits tiny;
    tiny.maxStringBytes = 32;
    tiny.availableStringBytes = 8;
    const auto exhausted = run(
        Kernel::ScalarToString,
        {1.0, std::int64_t{18}, false, std::int64_t{0}, std::string{}, std::string{}}, {}, tiny);
    expectations.expect(exhausted.failed && holds(exhausted, 0, std::string{}) &&
                            exhausted.summary == "Value utility string budget exceeded",
                        "precision beyond the output allowance reports a diagnostic");
    const auto trimmed = run(
        Kernel::ScalarToString,
        {2.5, std::int64_t{1000}, true, std::int64_t{0}, std::string{}, std::string{}}, {}, tiny);
    expectations.expect(!trimmed.failed && holds(trimmed, 0, std::string("2.5")),
                        "trimmed high precision uses scratch allowance and fits final output");

    struct IntegerCase final {
        std::int64_t value;
        std::int64_t padWidth;
        std::int64_t radix;
        std::string_view text;
    };
    static const IntegerCase kIntegerCases[]{
        {255, 0, 16, "ff"},
        {255, 4, 16, "00ff"},
        {10, 0, 2, "1010"},
        {511, 0, 8, "777"},
        {-42, 5, 10, "-0042"},
        {0, 0, 10, "0"},
        {std::numeric_limits<std::int64_t>::min(), 0, 10, "-9223372036854775808"},
        // A radix the document could not have stored: the kernel computes base ten rather than
        // trusting a number that names no base.
        {7, 0, 99, "7"},
    };
    for (const auto& item : kIntegerCases) {
        const auto outcome =
            run(Kernel::IntegerToString, {item.value, item.padWidth, std::string{}, std::string{}},
                {item.radix});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.text)),
                            "Integer To String writes its documented spelling at its radix");
    }
}

void testSafeParsing(Expectations& expectations) {
    struct ScalarCase final {
        std::string_view text;
        bool valid;
        double value;
    };
    static const ScalarCase kScalars[]{
        {"1.5", true, 1.5},        {"  -2  ", true, -2.0}, {"1e3", true, 1000.0},
        {".5", true, 0.5},         {"", false, 99.0},      {"   ", false, 99.0},
        {"abc", false, 99.0},      {"1abc", false, 99.0},  {"1,5", false, 99.0},
        {"inf", false, 99.0},      {"nan", false, 99.0},   {"0x10", false, 99.0},
        {"1e999999", false, 99.0}, {"١٢٣", false, 99.0},
    };
    for (const auto& item : kScalars) {
        const auto outcome = run(Kernel::StringToScalar, {std::string(item.text), 99.0});
        const bool answered =
            holds(outcome, 0, item.value) && holds(outcome, 1, item.valid) && !outcome.failed;
        expectations.expect(answered,
                            "String To Scalar answers the parse or the fallback, and says which");
    }

    struct IntegerCase final {
        std::string_view text;
        std::int64_t radix;
        bool valid;
        std::int64_t value;
    };
    static const IntegerCase kIntegers[]{
        {"42", 10, true, 42},    {"ff", 16, true, 255}, {"1010", 2, true, 10},
        {"  -7 ", 10, true, -7}, {"", 10, false, -1},   {"1.5", 10, false, -1},
        {"2", 2, false, -1},     {"g", 16, false, -1},  {"9223372036854775808", 10, false, -1},
    };
    for (const auto& item : kIntegers) {
        const auto outcome =
            run(Kernel::StringToInteger, {std::string(item.text), std::int64_t{-1}}, {item.radix});
        expectations.expect(holds(outcome, 0, item.value) && holds(outcome, 1, item.valid) &&
                                !outcome.failed,
                            "String To Integer answers the parse or the fallback at its radix");
    }

    struct BooleanCase final {
        std::string_view text;
        bool valid;
        bool value;
    };
    static const BooleanCase kBooleans[]{
        {"true", true, true}, {"TRUE", true, true},   {"yes", true, true}, {"on", true, true},
        {"1", true, true},    {"false", true, false}, {"no", true, false}, {"off", true, false},
        {"0", true, false},   {"", false, true},      {" ", false, true},  {"maybe", false, true},
        {"2", false, true},
    };
    for (const auto& item : kBooleans) {
        const auto outcome = run(Kernel::StringToBoolean, {std::string(item.text), true});
        expectations.expect(holds(outcome, 0, item.value) && holds(outcome, 1, item.valid) &&
                                !outcome.failed,
                            "String To Boolean reads its documented spellings and nothing else");
    }

    const core::Color4d fallback{0.1, 0.2, 0.3, 0.4};
    struct ColorCase final {
        std::string_view text;
        bool valid;
        core::Color4d value;
    };
    static const ColorCase kColors[]{
        {"#ffffff", true, {1.0, 1.0, 1.0, 1.0}},
        {"#000000", true, {0.0, 0.0, 0.0, 1.0}},
        {"  #00FF0080  ", true, {0.0, 1.0, 0.0, 128.0 / 255.0}},
        {"", false, {0.1, 0.2, 0.3, 0.4}},
        {"#fff", false, {0.1, 0.2, 0.3, 0.4}},
        {"ffffff", false, {0.1, 0.2, 0.3, 0.4}},
        {"#gggggg", false, {0.1, 0.2, 0.3, 0.4}},
    };
    for (const auto& item : kColors) {
        const auto outcome = run(Kernel::StringToColor, {std::string(item.text), fallback});
        expectations.expect(holds(outcome, 0, item.value) && holds(outcome, 1, item.valid) &&
                                !outcome.failed,
                            "String To Color reads #RRGGBB and #RRGGBBAA and nothing else");
    }
}

void testNumericConversions(Expectations& expectations) {
    struct RoundingCase final {
        double value;
        document::RoundingMode mode;
        std::int64_t expected;
    };
    static const RoundingCase kCases[]{
        // Ties go to EVEN, the frozen scalar primitive's rule, so this node and a Rounding node
        // never disagree about 2.5.
        {2.5, document::RoundingMode::Round, 2},
        {3.5, document::RoundingMode::Round, 4},
        {-2.5, document::RoundingMode::Round, -2},
        {2.9, document::RoundingMode::Floor, 2},
        {-2.1, document::RoundingMode::Floor, -3},
        {2.1, document::RoundingMode::Ceiling, 3},
        {-2.9, document::RoundingMode::Ceiling, -2},
        {2.9, document::RoundingMode::Truncate, 2},
        {-2.9, document::RoundingMode::Truncate, -2},
        // The edges: NaN names no integer at all, and a magnitude past the signed range saturates
        // rather than wrapping into an unrelated number.
        {std::numeric_limits<double>::quiet_NaN(), document::RoundingMode::Round, 0},
        {std::numeric_limits<double>::infinity(), document::RoundingMode::Round,
         std::numeric_limits<std::int64_t>::max()},
        {-std::numeric_limits<double>::infinity(), document::RoundingMode::Round,
         std::numeric_limits<std::int64_t>::min()},
        {1e30, document::RoundingMode::Floor, std::numeric_limits<std::int64_t>::max()},
    };
    for (const auto& item : kCases) {
        const auto outcome =
            run(Kernel::ScalarToInteger, {item.value}, {document::selectorStoredValue(item.mode)});
        expectations.expect(!outcome.failed && holds(outcome, 0, item.expected),
                            "Scalar To Integer rounds as its mode says and saturates at the edges");
    }

    expectations.expect(holds(run(Kernel::IntegerToScalar, {std::int64_t{-5}}), 0, -5.0),
                        "Integer To Scalar widens exactly");
    expectations.expect(holds(run(Kernel::BooleanToScalar, {true}), 0, 1.0) &&
                            holds(run(Kernel::BooleanToScalar, {false}), 0, 0.0),
                        "Boolean To Scalar is the one mapping every stored boolean has");
    expectations.expect(holds(run(Kernel::BooleanToInteger, {true}), 0, std::int64_t{1}) &&
                            holds(run(Kernel::BooleanToInteger, {false}), 0, std::int64_t{0}),
                        "Boolean To Integer is the same mapping");
    expectations.expect(holds(run(Kernel::IntegerToBoolean, {std::int64_t{0}}), 0, false) &&
                            holds(run(Kernel::IntegerToBoolean, {std::int64_t{-3}}), 0, true),
                        "Integer To Boolean is nonzero");
    // Nonzero is true, and NaN is FALSE: `value != 0` would call a number that is not a number set.
    expectations.expect(
        holds(run(Kernel::ScalarToBoolean, {0.0}), 0, false) &&
            holds(run(Kernel::ScalarToBoolean, {-0.0}), 0, false) &&
            holds(run(Kernel::ScalarToBoolean, {1e-300}), 0, true) &&
            holds(run(Kernel::ScalarToBoolean, {std::numeric_limits<double>::quiet_NaN()}), 0,
                  false) &&
            holds(run(Kernel::ScalarToBoolean, {std::numeric_limits<double>::infinity()}), 0, true),
        "Scalar To Boolean is nonzero, with NaN false and negative zero false");

    const auto labelled =
        run(Kernel::BooleanToString, {true, std::string("ON AIR"), std::string("off air")});
    expectations.expect(holds(labelled, 0, std::string("ON AIR")),
                        "Boolean To String writes the label the artist authored");
    expectations.expect(
        holds(run(Kernel::BooleanToString, {false, std::string("ON AIR"), std::string("off air")}),
              0, std::string("off air")),
        "and its counterpart when false");
}

void testStructuredConversions(Expectations& expectations) {
    const core::Color4d color{0.25, 0.5, 0.75, 0.5};
    expectations.expect(
        holds(run(Kernel::ColorToVector3, {color}), 0, document::Vec3d{0.25, 0.5, 0.75}),
        "Color To Vector 3 carries RGB and drops alpha");
    expectations.expect(
        holds(run(Kernel::Vector3ToColor, {document::Vec3d{0.25, 0.5, 0.75}, 0.5}), 0, color),
        "Vector 3 To Color supplies alpha from its own operand");
    expectations.expect(holds(run(Kernel::Vector2ToVector3, {document::Vec2d{1.0, 2.0}, 3.0}), 0,
                              document::Vec3d{1.0, 2.0, 3.0}),
                        "Vector 2 To Vector 3 supplies Z");
    expectations.expect(holds(run(Kernel::Vector3ToVector2, {document::Vec3d{1.0, 2.0, 3.0}}), 0,
                              document::Vec2d{1.0, 2.0}),
                        "Vector 3 To Vector 2 drops Z rather than projecting through it");

    expectations.expect(
        holds(run(Kernel::ColorToString, {color, false, false}), 0, std::string("#4080bf")),
        "Color To String writes #RRGGBB by default");
    expectations.expect(
        holds(run(Kernel::ColorToString, {color, true, true}), 0, std::string("#4080BF80")),
        "and #RRGGBBAA in upper case on request");

    expectations.expect(holds(run(Kernel::Vector2ToString,
                                  {document::Vec2d{1.0, -2.5}, std::string(", "), std::int64_t{2}}),
                              0, std::string("1.00, -2.50")),
                        "Vector 2 To String joins its components with the authored separator");
    expectations.expect(holds(run(Kernel::Vector3ToString, {document::Vec3d{1.0, 2.0, 3.0},
                                                            std::string(" "), std::int64_t{0}}),
                              0, std::string("1 2 3")),
                        "Vector 3 To String does the same across three");
}

// An operand that arrives as the wrong alternative is a MALFORMED PLAN -- the document's typing
// refuses such a link, and every accepted widening became its own promotion operation. The kernel
// reports it, names the operand, and still writes outputs of the right kinds.
void testMalformedOperands(Expectations& expectations) {
    const auto outcome =
        run(Kernel::ScalarToString, {std::string("not a number"), std::int64_t{2}, false,
                                     std::int64_t{0}, std::string{}, std::string{}});
    expectations.expect(outcome.failed && outcome.failedOperand == 0 && !outcome.summary.empty(),
                        "a mistyped operand is reported and names its own position");
    expectations.expect(outcome.outputCount == 1 &&
                            std::holds_alternative<std::string>(outcome.outputs[0]),
                        "and the outcome still carries a value of the declared kind");

    const auto missing = runtime::evaluateValueUtility({Kernel::StringToScalar,
                                                        {},
                                                        {},
                                                        core::RationalTime::fromInteger(0),
                                                        document::FrameRate::framesPerSecond24()});
    expectations.expect(missing.failed && missing.outputCount == 2 &&
                            std::holds_alternative<double>(missing.outputs[0]) &&
                            std::holds_alternative<bool>(missing.outputs[1]),
                        "an operand the plan never supplied is reported, not read");
}

// The composition frame rate is the ONE rate these read: a Seconds To Frames node and a Time node
// must never disagree about which frame an instant falls in.
void testTimeConversions(Expectations& expectations) {
    const auto rate = document::FrameRate::framesPerSecond24();
    struct FrameCase final {
        double seconds;
        std::int64_t frames;
    };
    static const FrameCase kFrames[]{
        {0.0, 0},
        {1.0, 24},
        {2.5, 60},
        {-1.0, -24},
        // Floored, not rounded: the frame an instant falls INSIDE is the frame being rendered.
        {0.999, 23},
        {-0.5, -12},
        // The edges: NaN names no frame, and an unreachable magnitude saturates.
        {std::numeric_limits<double>::quiet_NaN(), 0},
        {std::numeric_limits<double>::infinity(), std::numeric_limits<std::int64_t>::max()},
    };
    for (const auto& item : kFrames) {
        expectations.expect(
            holds(runAt(rate, Kernel::SecondsToFrames, {item.seconds}), 0, item.frames),
            "Seconds To Frames floors at the composition rate");
    }
    expectations.expect(
        holds(runAt(rate, Kernel::FramesToSeconds, {std::int64_t{24}}), 0, 1.0) &&
            holds(runAt(rate, Kernel::FramesToSeconds, {std::int64_t{12}}), 0, 0.5) &&
            holds(runAt(rate, Kernel::FramesToSeconds, {std::int64_t{-24}}), 0, -1.0),
        "Frames To Seconds is its exact inverse at a frame-aligned time");

    for (const auto frameRate :
         {*document::FrameRate::create(24, 1), *document::FrameRate::create(25, 1),
          *document::FrameRate::create(30000, 1001)}) {
        for (const auto frame : std::array<std::int64_t, 9>{-100, -29, -7, -1, 0, 1, 7, 29, 100}) {
            const auto boundary = runAt(frameRate, Kernel::FramesToSeconds, {frame});
            const auto* seconds = std::get_if<double>(&boundary.outputs[0]);
            if (boundary.failed || seconds == nullptr) {
                expectations.expect(false, "an ordinary frame has a Scalar boundary");
                continue;
            }
            const auto before = std::nextafter(*seconds, -std::numeric_limits<double>::infinity());
            const auto after = std::nextafter(*seconds, std::numeric_limits<double>::infinity());
            expectations.expect(
                holds(runAt(frameRate, Kernel::SecondsToFrames, {*seconds}), 0, frame) &&
                    holds(runAt(frameRate, Kernel::SecondsToFrames, {before}), 0, frame - 1) &&
                    holds(runAt(frameRate, Kernel::SecondsToFrames, {after}), 0, frame),
                "a frame boundary and its adjacent Scalars keep exact floor semantics");
        }
    }

    struct TimecodeCase final {
        double seconds;
        std::string_view label;
    };
    static const TimecodeCase kLabels[]{
        {0.0, "00:00:00:00"},
        {1.0, "00:00:01:00"},
        {2.5, "00:00:02:12"},
        {60.0, "00:01:00:00"},
        {3600.0, "01:00:00:00"},
        // Hours are not wrapped at 24: a composition may be longer than a day.
        {90000.0, "25:00:00:00"},
        // A negative time carries one sign on the whole label rather than on a field.
        {-1.0, "-00:00:01:00"},
        {-0.5, "-00:00:00:12"},
    };
    for (const auto& item : kLabels) {
        expectations.expect(holds(runAt(rate, Kernel::SecondsToTimecode, {item.seconds}), 0,
                                  std::string(item.label)),
                            "Seconds To Timecode writes non-drop HH:MM:SS:FF");
    }

    struct ParsedTimecode final {
        std::string_view label;
        bool valid;
        double seconds;
    };
    static const ParsedTimecode kParsed[]{
        {"00:00:02:12", true, 2.5},
        {"02:12", true, 2.5},
        {"01:30:00", true, 90.0},
        {"  00:00:01:00  ", true, 1.0},
        {"-00:00:01:00", true, -1.0},
        {"", false, -7.0},
        {"12", false, -7.0},
        {"00:00:00:24", false, -7.0},
        {"00:00:60:00", false, -7.0},
        {"00:70:00:00", false, -7.0},
        // `;` before the frame field MEANS drop-frame, which Bloom does not support, so reading it
        // as non-drop would name a different frame than the label does.
        {"00:00:02;12", false, -7.0},
        {"00:00:0a:12", false, -7.0},
        {"00:00:-2:12", false, -7.0},
        {"1:2:3:4:5", false, -7.0},
        {"00::02:12", false, -7.0},
    };
    for (const auto& item : kParsed) {
        const auto outcome =
            runAt(rate, Kernel::TimecodeToSeconds, {std::string(item.label), -7.0});
        expectations.expect(holds(outcome, 0, item.seconds) && holds(outcome, 1, item.valid) &&
                                !outcome.failed,
                            "Timecode To Seconds reads its three forms and falls back otherwise");
    }

    // The Frame Number readout is the same arithmetic a Time node's frame output uses, so the two
    // can never name different frames for one instant.
    for (const auto& [second, frame] : std::array<std::pair<std::int64_t, std::int64_t>, 3>{
             std::pair{std::int64_t{0}, std::int64_t{0}},
             std::pair{std::int64_t{1}, std::int64_t{24}},
             std::pair{std::int64_t{3}, std::int64_t{72}}}) {
        const auto outcome = runtime::evaluateValueUtility(
            {Kernel::FrameNumber, {}, {}, core::RationalTime::fromInteger(second), rate});
        expectations.expect(!outcome.failed && holds(outcome, 0, frame),
                            "Frame Number answers the frame the request time falls in");
    }
    // The three composition readouts are baked into constants by the compiler, so one reaching the
    // evaluator means the plan disagrees with the lowering that produced it.
    for (const auto kernel :
         {Kernel::FrameRate, Kernel::CompositionDuration, Kernel::CompositionSize}) {
        const auto outcome = runtime::evaluateValueUtility(
            {kernel, {}, {}, core::RationalTime::fromInteger(0), rate});
        expectations.expect(outcome.failed && !outcome.summary.empty(),
                            "a composition readout is lowered to a constant, never evaluated");
    }

    // A fractional rate counts the frame field to its NOMINAL whole number, which is what non-drop
    // timecode is: the label drifts from wall clock, and drop-frame -- the correction -- is not
    // supported.
    const auto broadcast = document::FrameRate::create(30000, 1001);
    expectations.expect(broadcast.has_value(), "29.97 is a rate a composition may hold");
    if (broadcast.has_value()) {
        expectations.expect(holds(runAt(*broadcast, Kernel::SecondsToTimecode, {1.0}), 0,
                                  std::string("00:00:00:29")),
                            "one wall-clock second at 29.97 is 29 non-drop frames, not a second");
        const auto refused =
            runAt(*broadcast, Kernel::TimecodeToSeconds, {std::string("00:00:00:30"), -7.0});
        expectations.expect(holds(refused, 1, false),
                            "a frame field at the nominal count names no frame");
    }
}

} // namespace


// #596 Axes and Axis Tick, against numbers worked out by hand.
void testAxes(Expectations& expectations) {
    using document::Vec2d;
    // x from -2 to 2 by 1 (5 ticks), y from -1 to 1 by 0.5 (5 ticks), ticks 10 long, origin (100, 200),
    // 50 by 40 pixels a unit.
    const auto drawn = run(Kernel::Axes, {-2.0, 2.0, -1.0, 1.0, 1.0, 0.5, 10.0, Vec2d{100.0, 200.0},
                                          Vec2d{50.0, 40.0}, noSpace()});
    expectations.expect(!drawn.failed && holds(drawn, 1, std::int64_t{5}) &&
                            holds(drawn, 2, std::int64_t{5}),
                        "Axes counts five ticks on each axis");
    const auto* path = std::get_if<runtime::CompiledPathValue>(&drawn.outputs[0]);
    const bool shaped = path != nullptr && path->value != nullptr &&
                        path->value->contours.size() == 12 && path->value->isValid();
    expectations.expect(shaped, "Axes draws the two axes and ten ticks as twelve open contours");
    if (!shaped)
        return;
    const auto& contours = path->value->contours;
    const auto is = [&](const std::size_t index, const Vec2d from, const Vec2d to) {
        const auto& anchors = contours[index].anchors;
        return anchors.size() == 2 && !contours[index].closed && anchors[0].point == from &&
               anchors[1].point == to;
    };
    expectations.expect(is(0, {0.0, 200.0}, {200.0, 200.0}),
                        "the X axis runs from x = -2 to x = 2 along y = 0");
    expectations.expect(is(1, {100.0, 240.0}, {100.0, 160.0}),
                        "the Y axis runs from y = -1 (down the page) to y = 1 along x = 0");
    expectations.expect(is(2, {0.0, 195.0}, {0.0, 205.0}), "the first X tick is a mark at x = -2");
    expectations.expect(is(6, {200.0, 195.0}, {200.0, 205.0}), "the last X tick is at x = 2");
    expectations.expect(is(9, {95.0, 200.0}, {105.0, 200.0}), "the Y tick for y = 0 crosses the origin");

    // The axes cross at the range's nearest edge when zero is out of range.
    const auto offset = run(Kernel::Axes, {1.0, 3.0, 2.0, 4.0, 1.0, 1.0, 4.0, Vec2d{0.0, 0.0},
                                           Vec2d{10.0, 10.0}, noSpace()});
    const auto* shifted = std::get_if<runtime::CompiledPathValue>(&offset.outputs[0]);
    expectations.expect(shifted != nullptr && shifted->value != nullptr &&
                            shifted->value->contours.size() == 2 + 3 + 3 &&
                            shifted->value->contours[0].anchors[0].point == Vec2d{10.0, -20.0} &&
                            shifted->value->contours[1].anchors[0].point == Vec2d{10.0, -20.0},
                        "with zero out of range the axes meet at the range's low corner");

    // The tick count is the shared rule: a whole number of steps keeps its last tick.
    expectations.expect(document::axisTickCount(0.0, 1.0, 0.1) == 11 &&
                            document::axisTickCount(0.0, 1.0, 0.0) == 0 &&
                            document::axisTickCount(1.0, 0.0, 0.1) == 0 &&
                            document::axisTickCount(0.0, 1.0e9, 1.0) == document::kMaximumAxisTicks + 1,
                        "the tick count handles whole steps, no step, a reversed range and too many");

    // Refusals say so and still answer an empty path and zero counts.
    const auto crowded = run(Kernel::Axes, {0.0, 1.0e9, -1.0, 1.0, 1.0, 1.0, 4.0, Vec2d{}, Vec2d{1.0, 1.0}, noSpace()});
    const auto* none = std::get_if<runtime::CompiledPathValue>(&crowded.outputs[0]);
    expectations.expect(crowded.failed && none != nullptr && none->value->contours.empty() &&
                            holds(crowded, 1, std::int64_t{0}),
                        "an axis with more than 1024 ticks is refused with an empty path");
    const auto nonFinite = run(Kernel::Axes, {std::numeric_limits<double>::quiet_NaN(), 1.0, 0.0, 1.0,
                                              1.0, 1.0, 4.0, Vec2d{}, Vec2d{1.0, 1.0}, noSpace()});
    expectations.expect(nonFinite.failed, "a range that is not finite is refused");
}

void testAxisTick(Expectations& expectations) {
    using document::Vec2d;
    const auto tick = [](const double min, const double max, const double step,
                         const std::int64_t index, const bool yAxis, const Vec2d offset,
                         const std::int64_t decimals) {
        return run(Kernel::AxisTick, {min, max, step, index, yAxis, Vec2d{100.0, 200.0},
                                      Vec2d{50.0, 40.0}, offset, decimals, noSpace()});
    };
    // Tick 3 of 0 to 10 by 2.5 is 7.5: 100 + 7.5 * 50 across, 24 below the axis.
    const auto x = tick(0.0, 10.0, 2.5, 3, false, {0.0, 24.0}, 1);
    expectations.expect(holds(x, 0, 7.5) && holds(x, 1, Vec2d{475.0, 224.0}) &&
                            holds(x, 2, std::string("7.5")) && holds(x, 3, true),
                        "an X tick has its value, its place below the axis, its label and is valid");
    // The next index is 12.5, past the range: not valid and no text, but the number is still its index's.
    const auto past = tick(0.0, 10.0, 2.5, 5, false, {0.0, 24.0}, 1);
    expectations.expect(holds(past, 0, 12.5) && holds(past, 2, std::string()) && holds(past, 3, false),
                        "a tick past the range is not valid and shows no label");
    expectations.expect(holds(tick(0.0, 10.0, 2.5, -1, false, {0.0, 24.0}, 1), 3, false),
                        "a negative index is not valid");
    // A Y tick: 100 - 24 across, 200 - 2 * 40 up.
    const auto y = tick(-5.0, 5.0, 1.0, 7, true, {-24.0, 0.0}, 0);
    expectations.expect(holds(y, 0, 2.0) && holds(y, 1, Vec2d{76.0, 120.0}) &&
                            holds(y, 2, std::string("2")) && holds(y, 3, true),
                        "a Y tick is placed up the page by its value, beside the axis");
    // Rounding and decimals.
    expectations.expect(holds(tick(-1.0, 1.0, 0.1, 11, false, {}, 1), 2, std::string("0.1")),
                        "decimals set the places shown");
    expectations.expect(holds(tick(-0.0004, 1.0, 1.0, 0, false, {}, 1), 2, std::string("0.0")),
                        "a value that rounds to zero is written without a sign");
    expectations.expect(holds(tick(0.0, 1.0, 0.0, 0, false, {}, 1), 3, false),
                        "a range with no step has no valid tick");
    // An axis tick and Axes agree: tick i is at min + i * step.
    expectations.expect(holds(tick(-2.0, 2.0, 1.0, 4, false, {}, 0), 0, 2.0),
                        "tick 4 of -2 to 2 by 1 is the range's end");
}

// #591 coordinate spaces, against maps worked out by hand and core::space as the reference.
namespace space_tests {
using document::Vec2d;
using Space = std::shared_ptr<const core::space::Space>;

struct Definition final {
    CompiledValue parent = runtime::CompiledSpaceValue{};
    std::string name = "graph";
    Vec2d origin{};
    Vec2d pixelsPerUnit{100.0, 100.0};
    double rotation = 0.0;
    bool yUp = true;
    Vec2d rangeMin{-5.0, -5.0};
    Vec2d rangeMax{5.0, 5.0};
    Vec2d center{};
    Vec2d size{1000.0, 1000.0};
    std::array<Vec2d, 4> corners{Vec2d{0.0, 0.0}, Vec2d{1000.0, 0.0}, Vec2d{1000.0, 1000.0},
                                 Vec2d{0.0, 1000.0}};
    document::SpaceMode mode = document::SpaceMode::Scale;
};

[[nodiscard]] ValueUtilityOutcome define(const Definition& d) {
    return run(Kernel::CoordinateSpace,
               {d.parent, d.name, d.origin, d.pixelsPerUnit, d.rotation, d.yUp, d.rangeMin, d.rangeMax,
                d.center, d.size, d.corners[0], d.corners[1], d.corners[2], d.corners[3]},
               {static_cast<std::int64_t>(d.mode)});
}

[[nodiscard]] Space spaceOf(const ValueUtilityOutcome& outcome) {
    const auto* held = outcome.outputCount > 0
                           ? std::get_if<runtime::CompiledSpaceValue>(&outcome.outputs[0])
                           : nullptr;
    return held == nullptr ? nullptr : held->value;
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}
[[nodiscard]] bool near(const CompiledValue& value, const Vec2d expected,
                        const double tolerance = 1e-9) {
    const auto* held = std::get_if<Vec2d>(&value);
    return held != nullptr && near(held->x, expected.x, tolerance) &&
           near(held->y, expected.y, tolerance);
}

[[nodiscard]] bool nearScalar(const CompiledValue& value, const double expected) {
    const auto* held = std::get_if<double>(&value);
    return held != nullptr && near(*held, expected, 1e-12);
}

void testDefinitions(Expectations& expectations) {
    // Origin and scale: the map Function Plot's own origin and scale draw, bit for bit.
    Definition graph;
    graph.origin = {256.0, 200.0};
    graph.pixelsPerUnit = {40.0, 30.0};
    const auto defined = define(graph);
    const auto held = spaceOf(defined);
    expectations.expect(!defined.failed && held != nullptr && held->name == "graph" &&
                            held->dimensions == 2 && held->parent == nullptr &&
                            held->range == core::space::Range2{{-5.0, -5.0}, {5.0, 5.0}},
                        "Coordinate Space defines a named 2D space with its range");
    if (held == nullptr)
        return;
    bool exact = true;
    for (const double x : {-3.0, -1.0 / 3.0, 0.1, 2.75})
        for (const double y : {-2.5, 0.2, 1.0}) {
            const auto pixel = core::space::mapPoint(held->toPixels, {x, y});
            exact = exact && pixel && pixel->x == 256.0 + x * 40.0 && pixel->y == 200.0 - y * 30.0;
        }
    expectations.expect(exact, "an unturned space is exactly the origin plus the scaled units");

    // Range to rectangle: the range's middle on the centre, the range filling the size.
    Definition rectangle;
    rectangle.mode = document::SpaceMode::Range;
    rectangle.rangeMin = {0.0, -1.0};
    rectangle.rangeMax = {4.0, 1.0};
    rectangle.center = {300.0, 200.0};
    rectangle.size = {400.0, 100.0};
    const auto boxed = spaceOf(define(rectangle));
    expectations.expect(boxed != nullptr &&
                            core::space::mapPoint(boxed->toPixels, {0.0, 1.0}) ==
                                core::space::Point2{100.0, 150.0} &&
                            core::space::mapPoint(boxed->toPixels, {4.0, -1.0}) ==
                                core::space::Point2{500.0, 250.0},
                        "a range to a rectangle puts the range's corners on the rectangle's");

    // Range to corners: projective, every corner on its point.
    Definition plane;
    plane.mode = document::SpaceMode::Corners;
    plane.corners = {Vec2d{180.0, 100.0}, Vec2d{330.0, 100.0}, Vec2d{420.0, 300.0},
                     Vec2d{90.0, 300.0}};
    const auto projected = spaceOf(define(plane));
    const auto corner = [&](const double x, const double y, const Vec2d expected) {
        const auto pixel = projected ? core::space::mapPoint(projected->toPixels, {x, y})
                                     : std::nullopt;
        return pixel && near(pixel->x, expected.x, 1e-9) && near(pixel->y, expected.y, 1e-9);
    };
    expectations.expect(projected != nullptr && !core::space::isAffine(projected->toPixels) &&
                            corner(-5.0, 5.0, plane.corners[0]) && corner(5.0, 5.0, plane.corners[1]) &&
                            corner(5.0, -5.0, plane.corners[2]) && corner(-5.0, -5.0, plane.corners[3]),
                        "a range to corners is projective with every corner on its point");

    // A space defined in another: the child's map is the parent's after its own.
    Definition child;
    child.parent = runtime::CompiledSpaceValue{held};
    child.name = "inset";
    child.origin = {1.0, 1.0};
    child.pixelsPerUnit = {0.5, 0.5};
    child.yUp = false;
    const auto nested = spaceOf(define(child));
    const auto through = [&](const double x, const double y) {
        return core::space::mapPoint(held->toPixels, {1.0 + 0.5 * x, 1.0 + 0.5 * y});
    };
    const auto direct = nested ? core::space::mapPoint(nested->toPixels, {2.0, -4.0}) : std::nullopt;
    expectations.expect(nested != nullptr && nested->parent == held && direct &&
                            near(direct->x, through(2.0, -4.0)->x) &&
                            near(direct->y, through(2.0, -4.0)->y),
                        "a parented space composes its map with the parent's");

    // Refusals: each says so and answers no space.
    Definition flat;
    flat.pixelsPerUnit = {0.0, 10.0};
    const auto refusedFlat = define(flat);
    expectations.expect(refusedFlat.failed && spaceOf(refusedFlat) == nullptr,
                        "a space with no size on one axis is flat and refused");
    Definition empty;
    empty.mode = document::SpaceMode::Range;
    empty.rangeMax = {-5.0, 5.0};
    expectations.expect(define(empty).failed, "a range that does not run forward is refused");
    Definition line = plane;
    line.corners = {Vec2d{0.0, 0.0}, Vec2d{1.0, 1.0}, Vec2d{2.0, 2.0}, Vec2d{0.0, 5.0}};
    expectations.expect(define(line).failed, "three corners on one line make no plane");
    Definition endless;
    endless.rotation = std::numeric_limits<double>::infinity();
    expectations.expect(define(endless).failed, "a rotation that is not finite is refused");
    Definition named;
    named.name = std::string(document::kMaximumSpaceNameBytes + 1, 'a');
    expectations.expect(define(named).failed, "a name past 256 bytes is refused");
    // Sixteen deep: a chain of fifteen parents is the most a space may have.
    CompiledValue chain = runtime::CompiledSpaceValue{};
    bool chained = true;
    for (std::size_t level = 0; level < document::kMaximumSpaceDepth; ++level) {
        Definition next;
        next.parent = chain;
        next.pixelsPerUnit = {1.0, 1.0};
        const auto outcome = define(next);
        chained = chained && !outcome.failed;
        chain = runtime::CompiledSpaceValue{spaceOf(outcome)};
    }
    Definition deepest;
    deepest.parent = chain;
    expectations.expect(chained && define(deepest).failed,
                        "a space at most fifteen spaces deep is defined, and one deeper refused");
}

void testReadoutAndConversions(Expectations& expectations) {
    Definition graph;
    graph.origin = {256.0, 200.0};
    graph.pixelsPerUnit = {40.0, 30.0};
    graph.rotation = 90.0;
    const auto held = spaceOf(define(graph));
    const CompiledValue wired = runtime::CompiledSpaceValue{held};
    // A quarter turn clockwise, Y up: one unit of x is 40 px down, one unit of y 30 px right.
    const auto read = run(Kernel::SpaceReadout, {wired});
    expectations.expect(!read.failed && near(read.outputs[0], {256.0, 200.0}) &&
                            near(read.outputs[1], {0.0, 40.0}) && near(read.outputs[2], {30.0, 0.0}) &&
                            near(read.outputs[3], {40.0, 30.0}) && nearScalar(read.outputs[4], 90.0) &&
                            near(read.outputs[5], {-5.0, -5.0}) && near(read.outputs[6], {5.0, 5.0}) &&
                            holds(read, 8, std::string("graph")) && holds(read, 9, std::string()) &&
                            holds(read, 10, false) && holds(read, 11, true),
                        "Space Readout answers the map's origin, axes, scale, turn, range and name");
    const auto* matrix = std::get_if<runtime::CompiledNumberList>(&read.outputs[7]);
    const auto planar = core::space::planar(held->toPixels);
    expectations.expect(matrix != nullptr &&
                            *matrix->values == std::vector<double>(planar.begin(), planar.end()),
                        "the matrix readout is the space's 3x3 map, row by row");
    const auto none = run(Kernel::SpaceReadout, {noSpace()});
    expectations.expect(!none.failed && holds(none, 11, false) && holds(none, 8, std::string()),
                        "with no space connected every readout is empty and not valid");

    // Point conversions are each other's inverse; a vector ignores the origin.
    const auto from = run(Kernel::FromSpace, {wired, Vec2d{2.0, 1.0}}, {0});
    expectations.expect(near(from.outputs[0], {256.0 + 30.0, 200.0 + 80.0}) && holds(from, 1, true),
                        "From Space maps a point of the space to its pixel");
    const auto to = run(Kernel::ToSpace, {wired, Vec2d{286.0, 280.0}}, {0});
    expectations.expect(near(to.outputs[0], {2.0, 1.0}) && holds(to, 1, true),
                        "To Space reads the pixel back as the point");
    const auto vector = run(Kernel::FromSpace, {wired, Vec2d{1.0, 0.0}}, {1});
    expectations.expect(near(vector.outputs[0], {0.0, 40.0}), "a vector is turned and scaled only");
    const auto back = run(Kernel::ToSpace, {wired, Vec2d{0.0, 40.0}}, {1});
    expectations.expect(near(back.outputs[0], {1.0, 0.0}), "and a pixel vector reads back");
    expectations.expect(holds(run(Kernel::ToSpace, {noSpace(), Vec2d{1.0, 1.0}}, {0}), 1, false),
                        "a conversion with no space is not valid");

    // Place in Space: the layer's position, its scale for a 100 px unit and the turn of the x axis.
    const auto placed = run(Kernel::PlaceInSpace, {wired, Vec2d{1.0, 0.5}, Vec2d{0.0, -3.0},
                                                   Vec2d{100.0, 100.0}});
    expectations.expect(near(placed.outputs[0], {256.0 + 15.0, 200.0 + 40.0 - 3.0}) &&
                            near(placed.outputs[1], {0.4, 0.3}) && nearScalar(placed.outputs[2], 90.0) &&
                            holds(placed, 3, true),
                        "Place in Space answers the layer's position, scale and rotation");
    const auto unplaced =
        run(Kernel::PlaceInSpace, {noSpace(), Vec2d{1.0, 0.5}, Vec2d{7.0, 8.0}, Vec2d{100.0, 100.0}});
    expectations.expect(near(unplaced.outputs[0], {7.0, 8.0}) && near(unplaced.outputs[1], {1.0, 1.0}) &&
                            holds(unplaced, 3, false),
                        "with no space a layer stays at its offset, unscaled");
}

void testPlotsInASpace(Expectations& expectations) {
    // Axes drawn in a space agree, point for point, with the same axes drawn by origin and scale.
    Definition graph;
    graph.origin = {100.0, 200.0};
    graph.pixelsPerUnit = {50.0, 40.0};
    const CompiledValue wired = runtime::CompiledSpaceValue{spaceOf(define(graph))};
    const auto own = run(Kernel::Axes, {-2.0, 2.0, -1.0, 1.0, 1.0, 0.5, 10.0, Vec2d{100.0, 200.0},
                                        Vec2d{50.0, 40.0}, noSpace()});
    const auto inSpace = run(Kernel::Axes, {-2.0, 2.0, -1.0, 1.0, 1.0, 0.5, 10.0, Vec2d{9.0, 9.0},
                                            Vec2d{3.0, 3.0}, wired});
    const auto* a = std::get_if<runtime::CompiledPathValue>(&own.outputs[0]);
    const auto* b = std::get_if<runtime::CompiledPathValue>(&inSpace.outputs[0]);
    expectations.expect(!inSpace.failed && a != nullptr && b != nullptr && a->value != nullptr &&
                            b->value != nullptr && *a->value == *b->value,
                        "Axes in a space draw exactly what the same origin and scale draw");
    const auto tickOwn = run(Kernel::AxisTick, {0.0, 10.0, 2.5, std::int64_t{3}, false,
                                                Vec2d{100.0, 200.0}, Vec2d{50.0, 40.0},
                                                Vec2d{0.0, 24.0}, std::int64_t{1}, noSpace()});
    const auto tickSpace = run(Kernel::AxisTick, {0.0, 10.0, 2.5, std::int64_t{3}, false, Vec2d{},
                                                  Vec2d{1.0, 1.0}, Vec2d{0.0, 24.0},
                                                  std::int64_t{1}, wired});
    expectations.expect(holds(tickSpace, 1, Vec2d{475.0, 224.0}) &&
                            tickOwn.outputs[1] == tickSpace.outputs[1],
                        "an axis tick in a space is placed where its own origin and scale put it");
    // A turned space turns the ticks with the axes: an X tick runs along the space's y axis.
    graph.rotation = 90.0;
    const CompiledValue turned = runtime::CompiledSpaceValue{spaceOf(define(graph))};
    const auto turnedAxes = run(Kernel::Axes, {-2.0, 2.0, -1.0, 1.0, 1.0, 0.5, 10.0, Vec2d{},
                                               Vec2d{1.0, 1.0}, turned});
    const auto* path = std::get_if<runtime::CompiledPathValue>(&turnedAxes.outputs[0]);
    const bool tickAlong = path != nullptr && path->value != nullptr &&
                           path->value->contours.size() == 12 &&
                           path->value->contours[2].anchors.size() == 2;
    expectations.expect(tickAlong &&
                            near(path->value->contours[2].anchors[0].point.y,
                                 path->value->contours[2].anchors[1].point.y) &&
                            near(std::abs(path->value->contours[2].anchors[0].point.x -
                                          path->value->contours[2].anchors[1].point.x),
                                 10.0),
                        "a turned space's X ticks run along its turned y axis, 10 px long");
}

} // namespace space_tests

int main() {
    Expectations expectations;
    testDescriptorShapes(expectations);
    testScalarAndIntegerToString(expectations);
    testSafeParsing(expectations);
    testNumericConversions(expectations);
    testStructuredConversions(expectations);
    testTimeConversions(expectations);
    testMalformedOperands(expectations);
    testAxes(expectations);
    testAxisTick(expectations);
    space_tests::testDefinitions(expectations);
    space_tests::testReadoutAndConversions(expectations);
    space_tests::testPlotsInASpace(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
