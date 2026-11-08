#include <bloom/core/color.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/gpu_coverage_contract.hpp>
#include <bloom/runtime/node_definition_registry.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

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

[[nodiscard]] bloom::runtime::NodeDefinition customSolid(const std::uint32_t version = 1) {
    using namespace bloom;
    auto definition = *document::builtInNodeDefinitions().find(
        document::kSolidSourceNodeType, document::kSolidSourceNodeSchemaVersion);
    definition.key = {"example.solid", version};
    return definition;
}

[[nodiscard]] bloom::runtime::NodeDefinition unsupportedDefinition(std::string typeId) {
    using namespace bloom::runtime;
    return {{std::move(typeId), 1},
            NodeLoweringKind::Unsupported,
            {},
            {{"image", SocketValueKind::Image}},
            {},
            std::nullopt};
}

[[nodiscard]] bloom::runtime::NodeDefinition builtInDefinition(const std::string_view typeId,
                                                               const std::uint32_t version) {
    bloom::runtime::NodeDefinitionRegistry registry;
    if (!bloom::runtime::registerBuiltInNodeDefinitions(registry)) {
        throw std::logic_error("built-in definition fixture must register");
    }
    const auto* definition = registry.find(typeId, version);
    if (definition == nullptr) {
        throw std::logic_error("built-in definition fixture must be addressable");
    }
    return *definition;
}

void testValidationAndDuplicates(Expectations& expectations) {
    using namespace bloom::runtime;
    NodeDefinitionRegistry registry;

    NodeDefinition effect{{"example.effect", 1},
                          NodeLoweringKind::ImageEffect,
                          {{"input", SocketValueKind::Image}},
                          {{"image", SocketValueKind::Image}},
                          {},
                          std::nullopt};
    NodeDefinitionRegistry effects;
    expectations.expect(effects.registerDefinition(effect) == NodeRegistrationStatus::Registered,
                        "one image input and output form an effect");
    NodeDefinitionRegistry rejectedEffects;
    effect.inputs.push_back({"extra", SocketValueKind::Image});
    expectations.expect(rejectedEffects.registerDefinition(effect) ==
                            NodeRegistrationStatus::InvalidDefinition,
                        "effect shape rejects a second image input");
    effect.inputs.pop_back();
    effect.inputs.front().name = "wrong";
    expectations.expect(rejectedEffects.registerDefinition(effect) ==
                            NodeRegistrationStatus::InvalidDefinition,
                        "effect input name is canonical");

    auto invalid = customSolid();
    invalid.key.typeId.clear();
    expectations.expect(registry.registerDefinition(std::move(invalid)) ==
                            NodeRegistrationStatus::InvalidDefinition,
                        "invalid definitions are rejected");
    expectations.expect(registry.definitions().empty(),
                        "invalid registration leaves the registry unchanged");

    const auto original = customSolid();
    expectations.expect(registry.registerDefinition(original) == NodeRegistrationStatus::Registered,
                        "valid definition is registered");
    auto replacement = original;
    replacement.parameters.front().role = "replacement";
    expectations.expect(registry.registerDefinition(std::move(replacement)) ==
                            NodeRegistrationStatus::InvalidDefinition,
                        "malformed replacement is rejected before duplicate lookup");
    expectations.expect(registry.registerDefinition(original) ==
                            NodeRegistrationStatus::DuplicateDefinition,
                        "an exact type and version duplicate is rejected");
    expectations.expect(registry.definitions().size() == 1 &&
                            registry.definitions().front() == original,
                        "duplicate registration never replaces the original");

    expectations.expect(registry.registerDefinition(customSolid(2)) ==
                            NodeRegistrationStatus::Registered,
                        "different schema versions may coexist");
}

// #383: a grade, colour or filter effect as it was before its effect mask was appended (the mask
// input and channel are checked on their own).
std::optional<bloom::document::NodeDefinition>
withoutMask(const bloom::document::NodeDefinition* definition) {
    if (definition == nullptr || !bloom::document::hasEffectMask(*definition))
        return std::nullopt;
    return bloom::document::withoutEffectMask(*definition);
}

void testFreezeAndBuiltIns(Expectations& expectations) {
    using namespace bloom;
    runtime::NodeDefinitionRegistry registry;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(registry),
                        "built-in definitions register as one startup contribution");
    // ADAPTED (task S7, then FIX1 item I, then UTIL-1): the seven structural node types,
    // the value library's first slice, and UTIL-1's twenty conversions, four
    // time conversions, fifteen string utilities, nineteen numeric and logic nodes and four
    // readouts, and DATA-1's four data-block readers. The number is pinned rather than computed so
    // that adding a node type is a deliberate edit here. MEDIA-1 adds one Image source definition,
    // AUDIO-2 adds one Audio source definition, COMP-SRC adds Composition source, and LB-1 adds the
    // Layer Bounds value readout. MEDIA-3 adds Video; COLOR-3 adds CST and File Transform. The
    // image-node batch adds Exposure, Offset, Multiply, Invert, the two Combine nodes, Crop, Pad,
    // Multiply Alpha, Checker, Flip, Rotate 90, Resize, Fit, Gaussian and Box Blur, and Linear and
    // Radial Gradient: 30 structural/image definitions plus 100 value-library definitions. ADR 0028
    // adds the Mask node; ADR 0026 adds Shuffle, Channel Copy and Remove; #364 adds Cryptomatte.
    // #336 adds the standalone Transform 2D; #337 adds Matte Blur; #338 adds Matte Erode, Matte
    // Dilate and Luma Key; #490 adds the Image Preview sink; #340 adds Levels, Curves and
    // Hue/Saturation; #479 adds Separate Channels and Combine Channels; #480 adds Pass and Image to
    // Channel Set; #494 adds Grade; #339 adds Drop Shadow, Glow and Light Wrap; #503 adds Alpha
    // Association; #502 adds Adjustment; #504 adds Colorize; #505 adds Vignette; #525 adds the
    // sixteen Fields nodes; #383 adds Mask Combine and Image to Mask; #483 adds Z Defocus and Depth
    // Fog; #507 adds Lens Flare; #486 adds Film Grain and Sharpen; #484 adds Motion Blur, Vector
    // Motion Blur, Optical Flow, Optical Flow Motion Blur, Echo, Directional Blur, Spin Blur and
    // Zoom Blur; #564 adds Frame Rate Hold; #512 adds Camera Shake; #563 adds Codec Degrade; #486 adds Lens
    // Distortion and Chromatic Aberration; #587 adds Test Card; #508 adds Light Rays; #567 adds Light
    // Flicker; #568 adds Rolling Shutter; #589 adds Path.
    expectations.expect(registry.definitions().size() == 197,
                        "startup contribution includes every built-in definition");
    for (const auto* type :
         {"bloom.linear-gradient", "bloom.radial-gradient", "bloom.flip", "bloom.rotate-90",
          "bloom.resize", "bloom.fit", "bloom.transform-2d", "bloom.gaussian-blur",
          "bloom.box-blur", "bloom.matte-blur", "bloom.matte-erode", "bloom.matte-dilate",
          "bloom.luma-key", "bloom.drop-shadow", "bloom.glow", "bloom.light-wrap"})
        // #541: the gradient generators and Glow are at schema 2.
        expectations.expect(registry.find(type, std::string_view(type) == "bloom.glow" ||
                                                        std::string_view(type).ends_with("-gradient")
                                                    ? 2
                                                    : 1) != nullptr,
                            "each pinned image-node addition is registered");
    // #339: the recipes are Image effects whose first input is the required Image; Glow's key and
    // Light Wrap's background are optional Images, Glow's mask an optional Mask; every animatable
    // value has its socket and the switches and Glow's octaves are constants.
    for (const auto* type : {"bloom.drop-shadow", "bloom.glow", "bloom.light-wrap"}) {
        const auto* recipe = registry.find(type, std::string_view(type) == "bloom.glow" ? 2 : 1);
        bool sockets = recipe != nullptr;
        // #541: a gradient value animates as a whole and has no socket kind.
        if (recipe)
            for (const auto& parameter : recipe->parameters)
                sockets = sockets && (parameter.valueKind == runtime::ParameterValueKind::Gradient
                                          ? !std::ranges::any_of(recipe->inputs,
                                                                 [&](const auto& input) {
                                                                     return input.name ==
                                                                            parameter.role;
                                                                 })
                                          : parameter.supportsAnimation ==
                                      std::ranges::any_of(recipe->inputs, [&](const auto& input) {
                                          return input.name == parameter.role;
                                      }));
        expectations.expect(recipe && sockets &&
                                recipe->lowering == runtime::NodeLoweringKind::ImageEffect &&
                                recipe->inputs[0].name == "input" &&
                                recipe->inputs[0].valueKind == runtime::SocketValueKind::Image &&
                                recipe->inputs[0].required && recipe->outputs.size() == 1 &&
                                recipe->outputs[0].valueKind == runtime::SocketValueKind::Image,
                            "#339: a recipe is an Image effect with a socket per animatable value");
        if (recipe) {
            auto changed = *recipe;
            changed.outputs[0].valueKind = runtime::SocketValueKind::Mask;
            runtime::NodeDefinitionRegistry scratch;
            expectations.expect(scratch.registerDefinition(changed) ==
                                    runtime::NodeRegistrationStatus::InvalidDefinition,
                                "#339: a recipe of another shape is refused");
        }
    }
    const auto* glow = registry.find("bloom.glow", 2);
    expectations.expect(glow && glow->inputs[1].name == "key" &&
                            glow->inputs[1].valueKind == runtime::SocketValueKind::Image &&
                            !glow->inputs[1].required && glow->inputs[2].name == "mask" &&
                            glow->inputs[2].valueKind == runtime::SocketValueKind::Mask &&
                            !glow->inputs[2].required,
                        "#339: Glow takes an optional key pass and an optional Mask");
    // #506: an optional lens-dirt Image, and the mode and shared controls as optional parameters
    // (a Glow saved before them stays valid); the mode and ray count are constants.
    const auto glowParameter =
        [&](const std::string_view role) -> const runtime::ParameterDefinition* {
        if (!glow)
            return nullptr;
        for (const auto& parameter : glow->parameters)
            if (parameter.role == role)
                return &parameter;
        return nullptr;
    };
    bool optional = glow != nullptr;
    for (const auto* role : {"mode", "exponent", "radiusRed", "radiusGreen", "radiusBlue",
                             "gradient", "tintGradient", "points", "angle", "length", "chromatic",
                             "dirtAmount", "rollOff"}) {
        const auto* parameter = glowParameter(role);
        optional = optional && parameter != nullptr && !parameter->required;
    }
    for (const auto* role : {"threshold", "knee", "intensity", "size", "falloff", "tint",
                             "octaves", "glowOnly"})
        optional = optional && glowParameter(role) != nullptr && glowParameter(role)->required;
    const auto* mode = glowParameter("mode");
    const auto* points = glowParameter("points");
    expectations.expect(optional && glow->inputs[3].name == "dirt" &&
                            glow->inputs[3].valueKind == runtime::SocketValueKind::Image &&
                            !glow->inputs[3].required && mode && !mode->supportsAnimation &&
                            mode->defaultValue == document::ParameterValue{std::int64_t{0}} && points &&
                            !points->supportsAnimation &&
                            points->defaultValue == document::ParameterValue{std::int64_t{4}},
                        "#506: Glow's modes and shared controls are optional, Classic by default, "
                        "with an optional lens-dirt Image");
    const auto* wrap = registry.find("bloom.light-wrap", 1);
    expectations.expect(wrap && wrap->inputs[1].name == "background" &&
                            wrap->inputs[1].valueKind == runtime::SocketValueKind::Image &&
                            !wrap->inputs[1].required,
                        "#339: Light Wrap takes an optional background Image");
    // #500: grading is its own category apart from colour management (the OCIO transforms), the
    // blurs and matte filters are Filters, and the image channel nodes and #339 recipes are
    // Compositing.
    const auto categoryOf = [&](const std::string_view type) {
        std::optional<document::NodeCategory> category;
        for (const auto& definition : registry.definitions())
            if (definition.key.typeId == type)
                category = definition.category;
        return category;
    };
    const auto expectCategory = [&](const std::initializer_list<std::string_view> types,
                                    const document::NodeCategory category) {
        for (const auto type : types)
            expectations.expect(categoryOf(type) == category,
                                "#500: " + std::string(type) + " is listed in its category");
    };
    expectCategory({"bloom.grade", "bloom.levels", "bloom.curves", "bloom.hue-saturation",
                    "bloom.exposure", "bloom.offset", "bloom.multiply", "bloom.invert",
                    "bloom.colorize"},
                   document::NodeCategory::Grade);
    // #504 Colorize: an Image in and out; constant mode (Tint by default), source and cycle with no
    // socket; every colour and scalar animatable with its own socket in parameter order. #541: the
    // optional gradient last, animatable as a whole value with no socket.
    const auto colorizeUnmasked = withoutMask(registry.find("bloom.colorize", 1));
    const auto* colorize = colorizeUnmasked ? &*colorizeUnmasked : nullptr;
    const auto socketsMatch = [&] {
        std::size_t socket = 1;
        for (const auto& parameter : colorize->parameters) {
            if (!parameter.supportsAnimation ||
                parameter.valueKind == document::ParameterValueKind::Gradient)
                continue;
            if (socket >= colorize->inputs.size() || colorize->inputs[socket].name != parameter.role ||
                colorize->inputs[socket].required)
                return false;
            ++socket;
        }
        return socket == colorize->inputs.size();
    };
    expectations.expect(
        colorize && colorize->inputs.size() == 15 && colorize->parameters.size() == 18 &&
            colorize->parameters[17].role == "gradient" &&
            colorize->parameters[17].valueKind == document::ParameterValueKind::Gradient &&
            !colorize->parameters[17].required && colorize->parameters[17].supportsAnimation &&
            colorize->parameters[17].defaultValue ==
                document::ParameterValue{document::defaultColorizeGradient()} &&
            colorize->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            colorize->parameters[0].role == "mode" && !colorize->parameters[0].supportsAnimation &&
            colorize->parameters[0].defaultValue == document::ParameterValue{std::int64_t{1}} &&
            colorize->parameters[7].role == "source" &&
            !colorize->parameters[7].supportsAnimation &&
            colorize->parameters[8].role == "cycle" && !colorize->parameters[8].supportsAnimation &&
            socketsMatch() && colorize->outputs.size() == 1 &&
            colorize->outputs[0].valueKind == runtime::SocketValueKind::Image,
        "#504: Colorize has its constant choices and an animatable socket per value");
    expectCategory({"bloom.ocio-colour-space-transform", "bloom.ocio-file-transform"},
                   document::NodeCategory::Color);
    // Zoom Blur; #512 adds Camera Shake; #563 adds Codec Degrade; #486 adds Lens Distortion and Chromatic
    // Aberration; #587 adds Test Card.
    expectCategory({"bloom.gaussian-blur", "bloom.box-blur", "bloom.matte-blur",
                    "bloom.matte-erode", "bloom.matte-dilate", "bloom.vignette", "bloom.film-grain",
                    "bloom.sharpen", "bloom.codec-degrade"},
                   document::NodeCategory::Filters);
    // #505 Vignette: an Image in and out; the constant shape (Elliptical) and mode (Multiply) with
    // no socket; every value animatable with its own socket in parameter order, the centre a
    // Vector 2 offset from the frame's centre.
    const auto vignetteUnmasked = withoutMask(registry.find("bloom.vignette", 1));
    const auto* vignette = vignetteUnmasked ? &*vignetteUnmasked : nullptr;
    const auto vignetteSockets = [&] {
        std::size_t socket = 1;
        for (const auto& parameter : vignette->parameters) {
            if (!parameter.supportsAnimation)
                continue;
            if (socket >= vignette->inputs.size() ||
                vignette->inputs[socket].name != parameter.role || vignette->inputs[socket].required)
                return false;
            ++socket;
        }
        return socket == vignette->inputs.size();
    };
    expectations.expect(
        vignette && vignette->inputs.size() == 8 && vignette->parameters.size() == 9 &&
            vignette->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            vignette->parameters[0].role == "shape" && !vignette->parameters[0].supportsAnimation &&
            vignette->parameters[0].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            vignette->parameters[1].role == "mode" && !vignette->parameters[1].supportsAnimation &&
            vignette->parameters[2].defaultValue == document::ParameterValue{0.5} &&
            vignette->parameters[8].role == "center" &&
            vignette->inputs[7].valueKind == runtime::SocketValueKind::Vector2 &&
            vignetteSockets() && vignette->outputs.size() == 1 &&
            vignette->outputs[0].valueKind == runtime::SocketValueKind::Image,
        "#505: Vignette has its constant choices and an animatable socket per value");
    // #486 Film Grain: an Image in and out, its constant animation and seed with no socket, and an
    // animatable socket per value; the effect mask follows them.
    const auto grainUnmasked = withoutMask(registry.find("bloom.film-grain", 1));
    expectations.expect(
        grainUnmasked && grainUnmasked->inputs.size() == 13 &&
            grainUnmasked->parameters.size() == 16 &&
            grainUnmasked->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            grainUnmasked->inputs[1].name == "intensity" &&
            grainUnmasked->parameters[0].defaultValue == document::ParameterValue{0.02} &&
            grainUnmasked->parameters[8].role == "animation" &&
            !grainUnmasked->parameters[8].supportsAnimation &&
            grainUnmasked->parameters[9].role == "seed" &&
            grainUnmasked->parameters[9].defaultValue == document::ParameterValue{std::int64_t{1}} &&
            grainUnmasked->parameters[10].role == "monochrome" &&
            grainUnmasked->parameters[10].valueKind == document::ParameterValueKind::Boolean &&
            !grainUnmasked->parameters[10].required &&
            !grainUnmasked->parameters[10].supportsAnimation &&
            grainUnmasked->parameters[10].defaultValue == document::ParameterValue{false} &&
            // #565 Sensor Noise: optional, so a node saved before it opens in Film mode.
            grainUnmasked->parameters[11].role == "mode" &&
            grainUnmasked->parameters[11].valueKind == document::ParameterValueKind::Integer &&
            !grainUnmasked->parameters[11].required &&
            !grainUnmasked->parameters[11].supportsAnimation &&
            grainUnmasked->parameters[11].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            grainUnmasked->parameters[12].role == "shadowWeighting" &&
            !grainUnmasked->parameters[12].required && grainUnmasked->parameters[12].supportsAnimation &&
            grainUnmasked->parameters[13].role == "chromaBlotches" &&
            !grainUnmasked->parameters[13].required &&
            grainUnmasked->parameters[14].role == "blotchSize" && !grainUnmasked->parameters[14].required &&
            grainUnmasked->parameters[15].role == "fixedPattern" &&
            grainUnmasked->parameters[15].defaultValue == document::ParameterValue{0.0} &&
            !grainUnmasked->parameters[15].required &&
            grainUnmasked->outputs.size() == 1 &&
            grainUnmasked->outputs[0].valueKind == runtime::SocketValueKind::Image,
        "#486: Film Grain has its constants and an animatable socket per value");
    // #486 Sharpen: an Image in and out and an animatable socket per value; the effect mask follows.
    const auto sharpenUnmasked = withoutMask(registry.find("bloom.sharpen", 1));
    expectations.expect(
        sharpenUnmasked && sharpenUnmasked->inputs.size() == 4 &&
            sharpenUnmasked->parameters.size() == 3 &&
            sharpenUnmasked->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            sharpenUnmasked->inputs[1].name == "amount" &&
            sharpenUnmasked->inputs[2].name == "radius" &&
            sharpenUnmasked->inputs[3].name == "threshold" &&
            sharpenUnmasked->parameters[0].defaultValue == document::ParameterValue{1.0} &&
            sharpenUnmasked->parameters[1].defaultValue == document::ParameterValue{3.0} &&
            sharpenUnmasked->parameters[2].defaultValue == document::ParameterValue{0.0} &&
            sharpenUnmasked->outputs.size() == 1 &&
            sharpenUnmasked->outputs[0].valueKind == runtime::SocketValueKind::Image,
        "#486: Sharpen has an animatable socket per value");
    // #587 Test Card: no input and one Image output; every parameter a constant with no socket, in
    // the table's order.
    const auto* testCard = registry.find("bloom.test-card", 1);
    expectations.expect(
        testCard != nullptr && testCard->inputs.empty() && testCard->outputs.size() == 1 &&
            testCard->outputs[0].name == "image" &&
            testCard->outputs[0].valueKind == runtime::SocketValueKind::Image &&
            testCard->lowering == document::NodeLoweringKind::TestCard &&
            testCard->parameters.size() == document::testCardParameters().size() &&
            testCard->parameters[0].role == "card" && !testCard->parameters[0].supportsAnimation &&
            testCard->parameters[0].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            testCard->parameters[1].role == "sizeMode" &&
            testCard->parameters[1].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            std::ranges::none_of(testCard->parameters,
                                 [](const auto& p) { return p.supportsAnimation; }),
        "#587: Test Card is a source with an Image output and constant parameters only");
    // #486 Lens Distortion: an Image in; the warped Image and its ST map out; the mode with no
    // socket, then a socket for the four coefficients and the centre, in parameter order. It takes
    // no effect mask.
    const auto* lens = registry.find("bloom.lens-distortion", 1);
    expectations.expect(
        lens != nullptr && !document::hasEffectMask(*lens) && lens->inputs.size() == 6 &&
            lens->parameters.size() == 6 &&
            lens->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            lens->inputs[1].name == "k1" && lens->inputs[2].name == "k2" &&
            lens->inputs[3].name == "p1" && lens->inputs[4].name == "p2" &&
            lens->inputs[5].name == "center" &&
            lens->inputs[5].valueKind == runtime::SocketValueKind::Vector2 &&
            lens->parameters[0].role == "mode" &&
            lens->parameters[0].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            lens->parameters[1].defaultValue == document::ParameterValue{0.0} &&
            lens->parameters[2].defaultValue == document::ParameterValue{0.0} &&
            lens->parameters[3].defaultValue == document::ParameterValue{0.0} &&
            lens->parameters[4].defaultValue == document::ParameterValue{0.0} &&
            lens->parameters[5].defaultValue == document::ParameterValue{document::Vec2d{}} &&
            lens->outputs.size() == 2 && lens->outputs[0].name == "image" &&
            lens->outputs[0].valueKind == runtime::SocketValueKind::Image &&
            lens->outputs[1].name == "stMap" &&
            lens->outputs[1].valueKind == runtime::SocketValueKind::Image,
        "#486: Lens Distortion has an animatable socket per value and two Image outputs");
    // #486 Chromatic Aberration: an Image in and out, a socket for the amount and the centre, in
    // parameter order, both defaulting to no effect. It takes no effect mask.
    const auto* aberration = registry.find("bloom.chromatic-aberration", 1);
    expectations.expect(
        aberration != nullptr && !document::hasEffectMask(*aberration) &&
            aberration->inputs.size() == 3 && aberration->parameters.size() == 2 &&
            aberration->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            aberration->inputs[1].name == "amount" &&
            aberration->inputs[1].valueKind == runtime::SocketValueKind::Scalar &&
            aberration->inputs[2].name == "center" &&
            aberration->inputs[2].valueKind == runtime::SocketValueKind::Vector2 &&
            aberration->parameters[0].role == "amount" &&
            aberration->parameters[0].defaultValue == document::ParameterValue{0.0} &&
            aberration->parameters[1].role == "center" &&
            aberration->parameters[1].defaultValue == document::ParameterValue{document::Vec2d{}} &&
            aberration->outputs.size() == 1 && aberration->outputs[0].name == "image" &&
            aberration->outputs[0].valueKind == runtime::SocketValueKind::Image,
        "#486: Chromatic Aberration has an animatable socket per value and a default of no effect");
    // #568 Rolling Shutter: the picture, an optional motion image, the amount, the camera offset and
    // the camera roll as sockets; the readout, direction and edges are constants. No effect mask.
    const auto* shutter = registry.find("bloom.rolling-shutter", 1);
    expectations.expect(
        shutter != nullptr && !document::hasEffectMask(*shutter) && shutter->inputs.size() == 5 &&
            shutter->parameters.size() == 6 && shutter->inputs[0].name == "input" &&
            shutter->inputs[1].name == "motion" && !shutter->inputs[1].required &&
            shutter->inputs[1].valueKind == runtime::SocketValueKind::Image &&
            shutter->inputs[2].name == "amount" && shutter->inputs[3].name == "cameraOffset" &&
            shutter->inputs[3].valueKind == runtime::SocketValueKind::Vector2 &&
            shutter->inputs[4].name == "cameraRoll" && shutter->parameters[0].role == "readout" &&
            shutter->parameters[0].defaultValue == document::ParameterValue{0.5} &&
            shutter->parameters[3].role == "amount" &&
            shutter->parameters[3].defaultValue == document::ParameterValue{1.0} &&
            shutter->outputs.size() == 1 && shutter->outputs[0].name == "image",
        "#568: Rolling Shutter has a motion image, sockets for the amount and the camera, constants "
        "for the rest");
    expectCategory({"bloom.rolling-shutter"}, document::NodeCategory::Transform);
    expectCategory({"bloom.light-flicker"}, document::NodeCategory::Values);
    // #563 Codec Degrade: an Image in and out, its block size, GOP length and chroma bleed with no
    // socket, and a socket for Quality and P-frame persistence; the effect mask follows.
    const auto codecUnmasked = withoutMask(registry.find("bloom.codec-degrade", 1));
    expectations.expect(
        codecUnmasked && codecUnmasked->inputs.size() == 3 && codecUnmasked->parameters.size() == 5 &&
            codecUnmasked->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            codecUnmasked->inputs[1].name == "quality" &&
            codecUnmasked->inputs[2].name == "persistence" &&
            codecUnmasked->parameters[0].defaultValue == document::ParameterValue{40.0} &&
            codecUnmasked->parameters[1].defaultValue == document::ParameterValue{0.5} &&
            codecUnmasked->parameters[2].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            codecUnmasked->parameters[3].defaultValue == document::ParameterValue{std::int64_t{5}} &&
            codecUnmasked->parameters[4].defaultValue == document::ParameterValue{true} &&
            codecUnmasked->outputs.size() == 1 &&
            codecUnmasked->outputs[0].valueKind == runtime::SocketValueKind::Image,
        "#563: Codec Degrade has its constants and an animatable socket per value");
    expectCategory({"bloom.separate-channels", "bloom.combine-channels", "bloom.drop-shadow",
                    "bloom.lens-flare", "bloom.glow", "bloom.light-wrap", "bloom.alpha-association"},
                   document::NodeCategory::Compositing);
    // #503 Alpha Association: an Image in and out, a constant mode (Unmult by default) and white
    // switch with no socket, and an animatable background colour with a Color socket.
    const auto* association = registry.find("bloom.alpha-association", 1);
    expectations.expect(
        association && association->inputs.size() == 2 &&
            association->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            association->inputs[1].name == "background" &&
            association->inputs[1].valueKind == runtime::SocketValueKind::Color &&
            !association->inputs[1].required && association->parameters.size() == 3 &&
            association->parameters[0].role == "mode" &&
            !association->parameters[0].supportsAnimation &&
            association->parameters[0].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            association->parameters[1].role == "white" &&
            !association->parameters[1].supportsAnimation &&
            association->parameters[2].role == "background" &&
            association->parameters[2].supportsAnimation && association->outputs.size() == 1 &&
            association->outputs[0].valueKind == runtime::SocketValueKind::Image,
        "#503: Alpha Association has its mode, white switch and animatable background");
    expectations.expect(std::ranges::count_if(registry.definitions(),
                                              [](const auto& definition) {
                                                  return definition.category ==
                                                         document::NodeCategory::Color;
                                              }) == 2,
                        "#500: Color holds only the two OCIO colour-management transforms");
    // #340: Levels has five animatable Scalars per channel (RGB master, red, green, blue, alpha)
    // and Curves (#493 schema 2) nine animatable point-list curves (master, red, green, blue,
    // alpha, then the four hue curves) with no sockets, then a constant clamp; Hue/Saturation has
    // an animatable hue and saturation.
    const auto levelsUnmasked = withoutMask(registry.find("bloom.levels", 1));
    const auto* levels = levelsUnmasked ? &*levelsUnmasked : nullptr;
    const auto curvesUnmasked = withoutMask(registry.find("bloom.curves", 2));
    const auto* curves = curvesUnmasked ? &*curvesUnmasked : nullptr;
    const auto hueSaturationUnmasked = withoutMask(registry.find("bloom.hue-saturation", 1));
    const auto* hueSaturation = hueSaturationUnmasked ? &*hueSaturationUnmasked : nullptr;
    expectations.expect(
        levels && levels->lowering == runtime::NodeLoweringKind::ImageEffect &&
            levels->category == document::NodeCategory::Grade && levels->inputs.size() == 26 &&
            levels->parameters.size() == 26 &&
            levels->inputs[0] ==
                document::InputPortDefinition{"input", runtime::SocketValueKind::Image} &&
            levels->inputs[1] == document::InputPortDefinition{"masterInputBlack",
                                                               runtime::SocketValueKind::Scalar,
                                                               false} &&
            levels->parameters[2].schemaKey == "bloom.levels.master.gamma" &&
            levels->parameters[2].supportsAnimation &&
            levels->parameters[2].defaultValue == document::ParameterValue{1.0} &&
            levels->parameters[24].role == "alphaOutputWhite" &&
            levels->parameters[25].schemaKey == "bloom.levels.clamp" &&
            !levels->parameters[25].supportsAnimation &&
            levels->parameters[25].defaultValue == document::ParameterValue{false} &&
            levels->outputs ==
                std::vector<document::OutputPortDefinition>{
                    {"image", runtime::SocketValueKind::Image}},
        "#340: Levels exposes per-channel input/output black and white and gamma, clamp off");
    const auto pathPoints = [](const document::ParameterValue& value) {
        std::vector<document::Vec2d> points;
        if (const auto* path = std::get_if<document::PathValue>(&value))
            for (const auto& anchor : path->firstContour().anchors)
                points.push_back(anchor.point);
        return points;
    };
    expectations.expect(
        curves && registry.find("bloom.curves", 1) == nullptr && curves->inputs.size() == 1 &&
            curves->parameters.size() == 10 && curves->parameters[1].role == "red" &&
            curves->parameters[1].schemaKey == "bloom.curves.red" &&
            curves->parameters[1].valueKind == document::ParameterValueKind::Path &&
            curves->parameters[1].supportsAnimation &&
            pathPoints(curves->parameters[1].defaultValue) ==
                std::vector<document::Vec2d>{{0.0, 0.0}, {1.0, 1.0}} &&
            curves->parameters[5].role == "hueVsHue" &&
            curves->parameters[5].schemaKey == "bloom.curves.hue-vs-hue" &&
            pathPoints(curves->parameters[5].defaultValue).empty() &&
            curves->parameters[8].schemaKey == "bloom.curves.luma-vs-saturation" &&
            curves->parameters[9].schemaKey == "bloom.curves.clamp" &&
            !curves->parameters[9].supportsAnimation,
        "#493: Curves exposes identity channel curves, empty hue curves and an unset clamp");
    expectations.expect(hueSaturation && hueSaturation->inputs.size() == 3 &&
                            hueSaturation->parameters.size() == 2 &&
                            hueSaturation->parameters[0].schemaKey == "bloom.hue-saturation.hue" &&
                            hueSaturation->parameters[1].defaultValue ==
                                document::ParameterValue{1.0},
                        "#340: Hue/Saturation exposes an animatable hue and saturation");
    // #494: Grade has the four wheels (master, red, green, blue each) then the six primaries,
    // every one an animatable Scalar with its own socket.
    const auto gradeUnmasked = withoutMask(registry.find("bloom.grade", 1));
    const auto* grade = gradeUnmasked ? &*gradeUnmasked : nullptr;
    expectations.expect(
        grade && grade->lowering == runtime::NodeLoweringKind::ImageEffect &&
            grade->category == document::NodeCategory::Grade && grade->inputs.size() == 23 &&
            grade->parameters.size() == 22 &&
            grade->inputs[1] == document::InputPortDefinition{"liftMaster",
                                                              runtime::SocketValueKind::Scalar,
                                                              false} &&
            grade->parameters[0].schemaKey == "bloom.grade.lift.master" &&
            grade->parameters[9].role == "gainRed" &&
            grade->parameters[9].defaultValue == document::ParameterValue{1.0} &&
            grade->parameters[15].schemaKey == "bloom.grade.offset.blue" &&
            grade->parameters[19].role == "pivot" &&
            grade->parameters[19].defaultValue == document::ParameterValue{0.18} &&
            grade->parameters[21].schemaKey == "bloom.grade.exposure" &&
            std::ranges::all_of(grade->parameters,
                                [](const auto& parameter) { return parameter.supportsAnimation; }),
        "#494: Grade exposes the wheels and primaries, neutral by default");
    // #479: Separate Channels takes one Image and offers its four channels as grey Images;
    // Combine Channels takes four optional Images, a constant source channel dropdown per input
    // (red, green, blue and red by default) and a constant alpha mode (Premultiplied).
    const auto* separate = registry.find("bloom.separate-channels", 1);
    const auto* combine = registry.find("bloom.combine-channels", 1);
    expectations.expect(separate && separate->lowering == runtime::NodeLoweringKind::ImageEffect &&
                            separate->category == document::NodeCategory::Compositing &&
                            separate->inputs ==
                                std::vector<document::InputPortDefinition>{
                                    {"input", runtime::SocketValueKind::Image}} &&
                            separate->parameters.empty() &&
                            separate->outputs ==
                                std::vector<document::OutputPortDefinition>{
                                    {"red", runtime::SocketValueKind::Image},
                                    {"green", runtime::SocketValueKind::Image},
                                    {"blue", runtime::SocketValueKind::Image},
                                    {"alpha", runtime::SocketValueKind::Image}},
                        "#479: Separate Channels offers red, green, blue and alpha Image outputs");
    expectations.expect(
        combine && combine->lowering == runtime::NodeLoweringKind::ImageEffect &&
            combine->category == document::NodeCategory::Compositing &&
            combine->inputs ==
                std::vector<document::InputPortDefinition>{
                    {"red", runtime::SocketValueKind::Image, false},
                    {"green", runtime::SocketValueKind::Image, false},
                    {"blue", runtime::SocketValueKind::Image, false},
                    {"alpha", runtime::SocketValueKind::Image, false}} &&
            combine->parameters.size() == 5 &&
            combine->parameters[0].schemaKey == "bloom.combine-channels.source" &&
            combine->parameters[0].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            combine->parameters[2].defaultValue == document::ParameterValue{std::int64_t{2}} &&
            combine->parameters[3].role == "alphaSource" &&
            combine->parameters[3].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            !combine->parameters[3].supportsAnimation &&
            combine->parameters[4].schemaKey == "bloom.combine-channels.alpha-mode" &&
            combine->parameters[4].defaultValue == document::ParameterValue{std::int64_t{0}} &&
            combine->outputs ==
                std::vector<document::OutputPortDefinition>{
                    {"image", runtime::SocketValueKind::Image}},
        "#479: Combine Channels takes four optional Images, sources and a Premultiplied mode");
    if (separate) {
        auto threeOutputs = *separate;
        threeOutputs.outputs.pop_back();
        runtime::NodeDefinitionRegistry scratch;
        expectations.expect(scratch.registerDefinition(threeOutputs) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "#479: a Separate Channels missing an output is refused");
    }
    if (levels) {
        auto socketless = *levels;
        socketless.inputs.pop_back();
        runtime::NodeDefinitionRegistry scratch;
        expectations.expect(scratch.registerDefinition(socketless) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "#340: a Levels missing a parameter socket is refused");
    }

    for (const auto& [kind, version] : std::array<std::pair<std::string_view, std::uint32_t>, 4>{
             {{document::kSolidSourceNodeType, 1},
              {document::kTextSourceNodeType, 1},
              {document::kLayerOutputNodeType, 3},
              {document::kLayerStackNodeType, 1}}}) {
        expectations.expect(registry.find(kind, version) == nullptr,
                            "historical built-in definitions are not registered");
    }

    registry.freeze();
    const auto* solid =
        registry.find(document::kSolidSourceNodeType, document::kSolidSourceNodeSchemaVersion);
    expectations.expect(registry.isFrozen() && solid != nullptr,
                        "freeze publishes deterministic definitions");
    const auto* checker = registry.find(document::kCheckerSourceNodeType,
                                        document::kCheckerSourceNodeSchemaVersion);
    const auto* multiplyAlpha = registry.find("bloom.multiply-alpha", 1);
    expectations.expect(checker && checker->parameters.size() == 6 &&
                            checker->inputs.size() == 2 && multiplyAlpha &&
                            multiplyAlpha->inputs.size() == 4 &&
                            multiplyAlpha->parameters.size() == 2,
                        "Checker and Multiply Alpha publish linkable artist controls");
    const auto* setAlpha = registry.find("bloom.set-alpha", 1);
    const auto* imageMix = registry.find("bloom.image-mix", 1);
    expectations.expect(setAlpha && multiplyAlpha && imageMix &&
                            setAlpha->inputs[1].name == "matte" && !setAlpha->inputs[1].required &&
                            multiplyAlpha->inputs[1].name == "matte" &&
                            !multiplyAlpha->inputs[1].required && imageMix->inputs[1].required,
                        "#361: Set Alpha and Multiply Alpha mattes are optional, Mix's is not");
    // #490: the Image Preview is a sink with one optional input that also takes the pixel kinds,
    // and one inline display choice.
    const auto* imagePreview = registry.find("bloom.image-preview", 1);
    expectations.expect(
        imagePreview && imagePreview->lowering == runtime::NodeLoweringKind::ImagePreview &&
            imagePreview->outputs.empty() && imagePreview->inputs.size() == 1 &&
            imagePreview->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            !imagePreview->inputs[0].required && imagePreview->inputs[0].acceptsPixelKinds &&
            imagePreview->parameters.size() == 1 &&
            imagePreview->parameters[0].schemaKey == "bloom.image-preview.display" &&
            !imagePreview->parameters[0].supportsAnimation &&
            imagePreview->category == document::NodeCategory::Output,
        "#490: Image Preview is an inspection sink with a display choice");
    if (imagePreview) {
        auto withOutput = *imagePreview;
        withOutput.outputs.push_back({"image", runtime::SocketValueKind::Image});
        runtime::NodeDefinitionRegistry scratch;
        expectations.expect(scratch.registerDefinition(withOutput) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "#490: an Image Preview with an output is refused");
    }
    expectations.expect(std::ranges::none_of(
                            registry.definitions(),
                            [](const auto& definition) {
                                // #525: Image Field, Shape Distance and Rasterize Field's extent
                                // read an image or a mask, so they take every pixel kind too.
                                // #483: a depth effect's depth reads an Image, a Mask or a
                                // channel set. #484: Vector Motion Blur's vectors and depth too.
                                return definition.key.typeId != "bloom.image-preview" &&
                                       definition.key.typeId != "bloom.vector-motion-blur" &&
                                       definition.key.typeId != "bloom.z-defocus" &&
                                       definition.key.typeId != "bloom.depth-fog" &&
                                       definition.key.typeId != "bloom.field-from-image" &&
                                       definition.key.typeId != "bloom.field-shape-distance" &&
                                       definition.key.typeId != "bloom.field-rasterize" &&
                                       std::ranges::any_of(definition.inputs, [](const auto& port) {
                                           return port.acceptsPixelKinds;
                                       });
                            }),
                        "#490: only the inspection and field-reading inputs take every pixel kind");
    // #490 GPU coverage classification: the sink lowers to no operation (its preview is the
    // upstream operations through the Viewer's GPU route), so it is a scope, not a pixel
    // operation, and needs no node fixture of its own.
    const auto coverage = runtime::gpuNodeTypeCoverage(registry);
    expectations.expect(
        !runtime::gpuLoweringIsPixelOperation(runtime::NodeLoweringKind::ImagePreview) &&
            runtime::gpuLoweringOperationId(runtime::NodeLoweringKind::ImagePreview) ==
                "scope.inspection" &&
            std::ranges::none_of(coverage,
                                 [](const auto& entry) {
                                     return std::string_view{entry.id} ==
                                            "node.bloom.image-preview";
                                 }),
        "#490: the Image Preview is classified as an inspection scope, not a pixel operation");
    const auto* matteBlur = registry.find("bloom.matte-blur", 1);
    expectations.expect(
        matteBlur && matteBlur->lowering == runtime::NodeLoweringKind::ImageEffect &&
            matteBlur->inputs.size() == 2 &&
            matteBlur->inputs[0].valueKind == runtime::SocketValueKind::Mask &&
            matteBlur->inputs[0].required &&
            matteBlur->inputs[1].valueKind == runtime::SocketValueKind::Scalar &&
            matteBlur->outputs.size() == 1 &&
            matteBlur->outputs[0].valueKind == runtime::SocketValueKind::Mask &&
            matteBlur->parameters.size() == 2 &&
            matteBlur->parameters[0].schemaKey == "bloom.blur.radius" &&
            matteBlur->parameters[1].schemaKey == "bloom.blur.edge",
        "#337: Matte Blur filters a Mask into a Mask with the blur radius and edge controls");
    if (matteBlur) {
        auto imageInput = *matteBlur;
        imageInput.inputs[0].valueKind = runtime::SocketValueKind::Image;
        imageInput.key.typeId = "bloom.matte-blur";
        runtime::NodeDefinitionRegistry scratch;
        expectations.expect(scratch.registerDefinition(imageInput) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "#337: a Matte Blur that accepts an Image is refused");
    }
    for (const auto* type : {"bloom.matte-erode", "bloom.matte-dilate"}) {
        const auto* filter = registry.find(type, 1);
        expectations.expect(
            filter && matteBlur && filter->inputs == matteBlur->inputs &&
                filter->outputs == matteBlur->outputs &&
                filter->parameters == matteBlur->parameters &&
                filter->lowering == runtime::NodeLoweringKind::ImageEffect,
            "#338: Matte Erode/Dilate filter a Mask into a Mask with the Matte Blur controls");
        if (filter) {
            auto imageOutput = *filter;
            imageOutput.outputs[0].valueKind = runtime::SocketValueKind::Image;
            runtime::NodeDefinitionRegistry scratch;
            expectations.expect(scratch.registerDefinition(imageOutput) ==
                                    runtime::NodeRegistrationStatus::InvalidDefinition,
                                "#338: a Matte Erode/Dilate that publishes an Image is refused");
        }
    }
    const auto* lumaKey = registry.find("bloom.luma-key", 1);
    expectations.expect(
        lumaKey && lumaKey->lowering == runtime::NodeLoweringKind::ImageEffect &&
            lumaKey->inputs.size() == 3 &&
            lumaKey->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            lumaKey->inputs[0].required &&
            lumaKey->inputs[1].valueKind == runtime::SocketValueKind::Scalar &&
            lumaKey->inputs[2].valueKind == runtime::SocketValueKind::Scalar &&
            lumaKey->outputs.size() == 1 &&
            lumaKey->outputs[0].valueKind == runtime::SocketValueKind::Mask &&
            lumaKey->parameters.size() == 3 &&
            lumaKey->parameters[0].schemaKey == "bloom.luma-key.low" &&
            lumaKey->parameters[0].supportsAnimation &&
            lumaKey->parameters[1].schemaKey == "bloom.luma-key.high" &&
            lumaKey->parameters[1].supportsAnimation &&
            lumaKey->parameters[2].schemaKey == "bloom.luma-key.invert" &&
            !lumaKey->parameters[2].supportsAnimation &&
            lumaKey->parameters[0].defaultValue == document::ParameterValue{0.0} &&
            lumaKey->parameters[1].defaultValue == document::ParameterValue{1.0} &&
            lumaKey->parameters[2].defaultValue == document::ParameterValue{false},
        "#338: Luma Key keys an Image into a Mask with low/high thresholds and invert");
    if (lumaKey) {
        auto imageOutput = *lumaKey;
        imageOutput.outputs[0].valueKind = runtime::SocketValueKind::Image;
        runtime::NodeDefinitionRegistry scratch;
        expectations.expect(scratch.registerDefinition(imageOutput) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "#338: a Luma Key that publishes an Image is refused");
    }
    registry.freeze();
    expectations.expect(registry.find(document::kSolidSourceNodeType,
                                      document::kSolidSourceNodeSchemaVersion) == solid,
                        "idempotent freeze preserves lookup addresses");
    expectations.expect(registry.containsType(document::kTextSourceNodeType),
                        "every built-in type remains discoverable");
    const auto* text =
        registry.find(document::kTextSourceNodeType, document::kTextSourceNodeSchemaVersion);
    // ADAPTED (task S3): Text was NodeLoweringKind::Unsupported while no portable CPU glyph
    // rasterizer existed. It now has its own lowering, so the contract pinned here is its parameter
    // shape -- content, then size, then color, typography, font reference, and box layout.
    expectations.expect(text != nullptr && text->lowering == runtime::NodeLoweringKind::Text,
                        "Text is a lowered capability with its own compiled operation");
    expectations.expect(
        text != nullptr && text->parameters.size() == 12 &&
            text->parameters[0].role == document::kTextParameterRole &&
            text->parameters[0].schemaKey == document::kTextParameterSchemaKey &&
            text->parameters[0].valueKind == runtime::ParameterValueKind::String &&
            text->parameters[1].role == document::kTextSizeParameterRole &&
            text->parameters[1].schemaKey == document::kTextSizeParameterSchemaKey &&
            text->parameters[1].valueKind == runtime::ParameterValueKind::Float64 &&
            text->parameters[2].role == document::kTextColorParameterRole &&
            text->parameters[2].schemaKey == document::kTextColorParameterSchemaKey &&
            text->parameters[2].valueKind == runtime::ParameterValueKind::Color4d &&
            text->parameters[6].role == document::kTextFontParameterRole &&
            text->parameters[6].schemaKey == document::kTextFontParameterSchemaKey &&
            text->parameters[6].valueKind == runtime::ParameterValueKind::String &&
            !text->parameters[6].required && !text->parameters[6].supportsAnimation,
        "the text schema includes a defaulted, non-animatable font reference");
    expectations.expect(
        text != nullptr && text->parameters.size() == 12 &&
            text->parameters[1].defaultValue ==
                document::ParameterValue{document::kDefaultTextSizePixels} &&
            text->parameters[2].defaultValue ==
                document::ParameterValue{core::Color4d{1.0, 1.0, 1.0, 1.0}} &&
            text->parameters[6].defaultValue ==
                document::ParameterValue{std::string(document::kDefaultTextFontReference)},
        "a new text layer defaults to 72 px opaque white and a DejaVu Sans reference");
    const auto* layer =
        registry.find(document::kLayerOutputNodeType, document::kLayerOutputNodeSchemaVersion);
    // ADAPTED (task S4): the Layer Output schema grew from two parameters to five, so this
    // ADAPTED (task S5): a source parameter no longer declares "no animation" as a class. Animation
    // support is still an explicit per-parameter capability, but what it must EQUAL is the shared
    // schema predicate -- so a registered definition can never be a second opinion about what is
    // animatable. Solid colour and text size/colour now declare it; text content, a String, does
    // not.
    expectations.expect(
        // ADR 0024: schema 5 appends the optional animatable Time Remap.
        layer != nullptr && layer->parameters.size() == 7 &&
            std::ranges::all_of(
                std::span(layer->parameters).first(5),
                [](const auto& parameter) { return parameter.supportsAnimation; }) &&
            !layer->parameters[5].supportsAnimation && layer->parameters[6].supportsAnimation &&
            !layer->parameters[6].required && solid != nullptr &&
            solid->parameters.front().supportsAnimation && text != nullptr &&
            text->parameters.size() == 12 && !text->parameters[0].supportsAnimation &&
            text->parameters[1].supportsAnimation && text->parameters[2].supportsAnimation,
        "animation support is an explicit per-parameter evaluator capability");
    const auto declarationMatchesSchema = [](const auto& definition) {
        return std::ranges::all_of(definition->parameters, [](const auto& parameter) {
            return parameter.supportsAnimation ==
                   document::isAnimatableSchemaKey(parameter.schemaKey);
        });
    };
    expectations.expect(layer != nullptr && solid != nullptr && text != nullptr &&
                            declarationMatchesSchema(layer) && declarationMatchesSchema(solid) &&
                            declarationMatchesSchema(text),
                        "and it agrees with the shared schema predicates for every registered "
                        "parameter, so the two can never drift");
    expectations.expect(
        layer != nullptr && layer->parameters.size() == 7 &&
            layer->parameters[6].role == document::kTimeRemapParameterRole &&
            layer->parameters[6].schemaKey == document::kTimeRemapParameterSchemaKey &&
            layer->parameters[0].role == document::kPositionParameterRole &&
            layer->parameters[0].schemaKey == document::kPositionParameterSchemaKey &&
            layer->parameters[0].valueKind == runtime::ParameterValueKind::Vec2d &&
            layer->parameters[1].role == document::kAnchorParameterRole &&
            layer->parameters[1].schemaKey == document::kAnchorParameterSchemaKey &&
            layer->parameters[1].valueKind == runtime::ParameterValueKind::Vec2d &&
            layer->parameters[2].role == document::kScaleParameterRole &&
            layer->parameters[2].schemaKey == document::kScaleParameterSchemaKey &&
            layer->parameters[2].valueKind == runtime::ParameterValueKind::Vec2d &&
            layer->parameters[3].role == document::kRotationParameterRole &&
            layer->parameters[3].schemaKey == document::kRotationParameterSchemaKey &&
            layer->parameters[3].valueKind == runtime::ParameterValueKind::Float64 &&
            layer->parameters[4].role == document::kOpacityParameterRole &&
            layer->parameters[4].schemaKey == document::kOpacityParameterSchemaKey &&
            layer->parameters[4].valueKind == runtime::ParameterValueKind::Float64 &&
            layer->parameters[5].role == document::kBlendModeParameterRole &&
            layer->parameters[5].schemaKey == document::kBlendModeParameterSchemaKey &&
            layer->parameters[5].valueKind == runtime::ParameterValueKind::Integer,
        "the Layer Output schema is exactly position, anchor, scale, rotation, opacity, blend "
        "mode and the optional Time Remap, in the registered order");
    expectations.expect(layer != nullptr && layer->parameters.size() == 7 &&
                            layer->parameters[1].defaultValue ==
                                document::ParameterValue{document::kDefaultAnchor} &&
                            layer->parameters[2].defaultValue ==
                                document::ParameterValue{document::kDefaultScale} &&
                            layer->parameters[3].defaultValue ==
                                document::ParameterValue{document::kDefaultRotationDegrees} &&
                            layer->parameters[5].defaultValue ==
                                document::ParameterValue{document::kDefaultBlendModeValue},
                        "the transform defaults are the identity transform -- center anchor, unit "
                        "scale, no rotation -- and the blend mode defaults to Normal");
    expectations.expect(registry.registerDefinition(customSolid()) ==
                            runtime::NodeRegistrationStatus::Frozen,
                        "registration is closed after freeze");
}

void testStructuralLoweringsRequireCanonicalKeys(Expectations& expectations) {
    using namespace bloom;
    struct Case final {
        std::string_view typeId;
        std::uint32_t version;
    };
    constexpr std::array cases{
        Case{document::kLayerOutputNodeType, document::kLayerOutputNodeSchemaVersion},
        Case{document::kLayerStackNodeType, document::kLayerStackNodeSchemaVersion},
        Case{document::kCompositionOutputNodeType, document::kCompositionOutputNodeSchemaVersion},
    };

    // Task FIX1, item H: the composition Output is a SINK. It declares one input and no output at
    // all, and a definition claiming one is refused -- which is what keeps "nothing connects from
    // the end of the composition" a registry fact rather than an editor convention.
    {
        const auto output = builtInDefinition(document::kCompositionOutputNodeType,
                                              document::kCompositionOutputNodeSchemaVersion);
        expectations.expect(
            output.outputs.empty() && output.inputs.size() == 3 &&
                output.inputs[1].name == document::kCompositionOutputAudioInputPort &&
                output.inputs[1].valueKind == runtime::SocketValueKind::Audio &&
                output.inputs[2].name == document::kCompositionOutputChannelsInputPort &&
                output.inputs[2].valueKind == runtime::SocketValueKind::ChannelSet &&
                !output.inputs[2].required,
            "the composition Output declares image, optional audio and optional channel inputs "
            "and no output");
        runtime::NodeDefinitionRegistry sourcingOutput;
        auto spoof = output;
        spoof.outputs.push_back(
            {std::string(document::kCompositionOutputOutputPort), runtime::SocketValueKind::Image});
        expectations.expect(sourcingOutput.registerDefinition(std::move(spoof)) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "and a definition that gives it one is refused");

        // The image input is OPTIONAL, mirroring the Layer Output content port: a disconnected
        // Output is an empty composition that draws nothing, not a required-input failure. The
        // registry refuses a definition that claims it is required, so optionality is a registry
        // fact rather than an editor convention.
        expectations.expect(output.inputs.size() == 3 &&
                                output.inputs[0].name == document::kCompositionOutputInputPort &&
                                output.inputs[0].valueKind == runtime::SocketValueKind::Image &&
                                !output.inputs[0].required,
                            "the composition Output image input is optional");
        runtime::NodeDefinitionRegistry requiredImageOutput;
        auto requiredSpoof = output;
        requiredSpoof.inputs[0].required = true;
        expectations.expect(requiredImageOutput.registerDefinition(std::move(requiredSpoof)) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "a definition that requires the Output image input is refused");
    }

    for (const auto& testCase : cases) {
        runtime::NodeDefinitionRegistry customTypeRegistry;
        auto customType = builtInDefinition(testCase.typeId, testCase.version);
        customType.key.typeId = "example.structural-spoof";
        expectations.expect(customTypeRegistry.registerDefinition(std::move(customType)) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "structural lowering rejects a custom type ID");

        runtime::NodeDefinitionRegistry customVersionRegistry;
        auto customVersion = builtInDefinition(testCase.typeId, testCase.version);
        ++customVersion.key.schemaVersion;
        expectations.expect(customVersionRegistry.registerDefinition(std::move(customVersion)) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "structural lowering rejects a non-canonical schema version");
    }

    runtime::NodeDefinitionRegistry customSolidRegistry;
    expectations.expect(customSolidRegistry.registerDefinition(customSolid(17)) ==
                            runtime::NodeRegistrationStatus::Registered,
                        "Solid remains an explicitly extensible lowering contract");
}

// Task S7: the value library's ONE shape contract, exercised through the registry rather than by
// reaching into it. Every built-in registers (the count above proves that), so what is pinned here
// is the refusals -- each perturbation below breaks exactly one clause of the contract, and a
// definition that still registered would mean the clause is not being checked.
void testValueLoweringShapeContract(Expectations& expectations) {
    using namespace bloom;

    const auto refuses = [&expectations](runtime::NodeDefinition definition,
                                         const std::string_view message) {
        runtime::NodeDefinitionRegistry registry;
        expectations.expect(registry.registerDefinition(std::move(definition)) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            message);
    };

    // A literal: no inputs, one output, one parameter.
    const auto literal =
        builtInDefinition(document::kScalarValueNodeType, document::kValueNodeSchemaVersion);
    {
        auto extraSocket = literal;
        extraSocket.inputs.push_back(
            {std::string(document::kValueParameterRole), runtime::SocketValueKind::Scalar, false});
        refuses(std::move(extraSocket), "a literal Value node may not take an input at all");
    }
    {
        // ADAPTED (task FIX1, item G): the Scalar, Vector 2 and Colour LITERALS are animatable now,
        // so the clause is no longer "never" but "exactly what the schema predicates say". A
        // definition that disagrees with them in either direction is refused, which is what these
        // two pin.
        auto notAnimatable = literal;
        notAnimatable.parameters.front().supportsAnimation = false;
        refuses(std::move(notAnimatable),
                "a Scalar literal that denies its own animatable schema is refused");
        auto animatable =
            builtInDefinition(document::kStringValueNodeType, document::kValueNodeSchemaVersion);
        animatable.parameters.front().supportsAnimation = true;
        refuses(std::move(animatable),
                "and a String literal claiming a curve kind that does not exist is refused too");
    }
    {
        auto structural = literal;
        structural.cardinality = runtime::NodeCardinality::OnePerComposition;
        refuses(std::move(structural), "a value node is never a structural singleton");
    }
    {
        auto misfiled = literal;
        misfiled.category = runtime::NodeCategory::Compositing;
        refuses(std::move(misfiled),
                "a value node is listed under Values or Utilities and nowhere else");
    }

    // An operand node: every socket backed by a parameter of the matching kind, and never required.
    const auto clamp =
        builtInDefinition(document::kClampNodeType, document::kValueNodeSchemaVersion);
    {
        auto required = clamp;
        required.inputs.front().required = true;
        refuses(std::move(required),
                "an operand socket is never required: its parameter is the value when nothing is "
                "connected");
    }
    {
        auto mismatched = clamp;
        mismatched.inputs.front().valueKind = runtime::SocketValueKind::Color;
        refuses(std::move(mismatched),
                "an operand socket's kind must be the one its parameter's value kind carries");
    }
    {
        auto unbacked = clamp;
        unbacked.inputs.push_back({"stray", runtime::SocketValueKind::Scalar, false});
        refuses(std::move(unbacked),
                "a socket with no parameter behind it exists only on a Reroute");
    }
    {
        auto unsocketed = clamp;
        unsocketed.parameters.push_back(
            {"stray", "example.operand", runtime::ParameterValueKind::Float64, true, false, 0.0});
        refuses(
            std::move(unsocketed),
            "a parameter with no socket must be an inline selector, not an unreachable operand");
    }

    // The Reroute: ADAPTED (task FIX1, item I). There is ONE reroute type now, and the kind it
    // carries comes from the link it sits on rather than from its name, so the clause that used to
    // read "a kind-named type and its sockets cannot disagree" reads "its two sockets agree with
    // each other".
    const auto reroute =
        builtInDefinition(document::kRerouteNodeType, document::kValueNodeSchemaVersion);
    {
        auto optional = reroute;
        optional.inputs.front().required = false;
        refuses(std::move(optional),
                "a Reroute's pass-through is required: nothing else can supply it");
    }
    {
        auto retyped = reroute;
        retyped.outputs.front().valueKind = runtime::SocketValueKind::Color;
        refuses(std::move(retyped), "a Reroute's two sockets declare one kind, not two");
    }

    const auto bounds =
        builtInDefinition(document::kLayerBoundsNodeType, document::kValueNodeSchemaVersion);
    expectations.expect(bounds.lowering == runtime::NodeLoweringKind::ValueBoundsReadout &&
                            bounds.inputs.size() == 1 &&
                            bounds.inputs.front().name == document::kLayerBoundsImagePortName &&
                            bounds.inputs.front().valueKind == runtime::SocketValueKind::Image &&
                            bounds.inputs.front().required && bounds.outputs.size() == 4 &&
                            std::ranges::all_of(bounds.outputs,
                                                [](const auto& output) {
                                                    return output.valueKind ==
                                                           runtime::SocketValueKind::Vector2;
                                                }),
                        "Layer Bounds declares one required Image input and four Vector2 outputs");
    {
        auto extraSocket = bounds;
        extraSocket.inputs.push_back({std::string(document::kLayerBoundsImagePortName),
                                      runtime::SocketValueKind::Image, true});
        refuses(std::move(extraSocket), "Layer Bounds accepts exactly one Image input");
    }

    // Time: no inputs, no parameters, and its two outputs in their declared units.
    {
        auto time =
            builtInDefinition(document::kTimeValueNodeType, document::kValueNodeSchemaVersion);
        time.outputs.pop_back();
        refuses(std::move(time), "Time declares both of its units or neither");
    }

    // A Switch: both branches carry the result's kind, and the condition is Boolean.
    {
        auto mixedSwitch =
            builtInDefinition(document::kScalarSwitchNodeType, document::kValueNodeSchemaVersion);
        mixedSwitch.inputs.front().valueKind = runtime::SocketValueKind::Scalar;
        refuses(std::move(mixedSwitch), "a Switch's condition is Boolean and only Boolean");
    }
}

void testLargeFrozenRegistryLookup(Expectations& expectations) {
    using namespace bloom::runtime;
    constexpr std::size_t definitionCount = 4'096;
    NodeDefinitionRegistry registry;
    bool registered = true;
    for (std::size_t index = definitionCount; index > 0; --index) {
        const auto typeId = "example.bulk." + std::to_string(index - 1);
        registered = registered && registry.registerDefinition(unsupportedDefinition(typeId)) ==
                                       NodeRegistrationStatus::Registered;
    }
    expectations.expect(registered && registry.definitions().size() == definitionCount,
                        "large registry fixture registers in deliberately unsorted order");
    expectations.expect(registry.find("example.bulk.0", 1) != nullptr &&
                            registry.containsType("example.bulk.4095"),
                        "pre-freeze lookup remains correct for startup duplicate checks");

    registry.freeze();
    expectations.expect(registry.find("example.bulk.0", 1) != nullptr &&
                            registry.find("example.bulk.2048", 1) != nullptr &&
                            registry.find("example.bulk.4095", 1) != nullptr &&
                            registry.find("example.bulk.2048", 2) == nullptr &&
                            registry.find("example.bulk.missing", 1) == nullptr &&
                            registry.containsType("example.bulk.2048") &&
                            !registry.containsType("example.bulk.missing"),
                        "freeze-sorted lookup resolves large registries and exact versions");
}

} // namespace

// #383: every Grade, Color Management and Filters image effect ends with the optional effect mask
// (a Mask input that also lands an Image, then a constant Alpha-default selector); Glow keeps its
// own mask; Mask Combine and Image to Mask are Matte lowerings under Compositing.
void testMatteWorkflowDefinitions(Expectations& expectations) {
    using namespace bloom;
    runtime::NodeDefinitionRegistry registry;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    std::size_t masked = 0;
    bool shaped = true;
    for (const auto& definition : registry.definitions()) {
        const bool effect = definition.lowering == runtime::NodeLoweringKind::ImageEffect &&
                            (definition.category == document::NodeCategory::Grade ||
                             definition.category == document::NodeCategory::Color ||
                             definition.category == document::NodeCategory::Filters);
        if (!document::hasEffectMask(definition)) {
            // Only an effect whose input is a Mask (the matte filters) or that has a mask of its
            // own goes without.
            shaped = shaped && !(effect && document::takesEffectMask(definition));
            continue;
        }
        ++masked;
        const auto& input = definition.inputs.back();
        const auto& channel = definition.parameters.back();
        shaped = shaped && effect && input.name == "mask" &&
                 input.valueKind == runtime::SocketValueKind::Mask && !input.required &&
                 input.acceptsImageMatte && channel.role == "maskChannel" &&
                 !channel.supportsAnimation &&
                 channel.defaultValue == document::ParameterValue{std::int64_t{0}};
    }
    const bool named = std::ranges::all_of(
        std::array<std::string_view, 26>{"bloom.grade", "bloom.levels", "bloom.hue-saturation",
                                         "bloom.exposure", "bloom.offset", "bloom.multiply",
                                         "bloom.invert", "bloom.colorize",
                                         "bloom.ocio-colour-space-transform",
                                         "bloom.ocio-file-transform", "bloom.gaussian-blur",
                                         "bloom.box-blur", "bloom.vignette", "bloom.curves",
                                         "bloom.z-defocus", "bloom.depth-fog", "bloom.film-grain",
                                         "bloom.motion-blur", "bloom.echo", "bloom.frame-rate-hold", "bloom.directional-blur",
                                         "bloom.spin-blur", "bloom.zoom-blur", "bloom.vector-motion-blur",
                                         "bloom.sharpen", "bloom.codec-degrade"},
        [&](const std::string_view type) {
            return std::ranges::any_of(registry.definitions(), [&](const auto& definition) {
                return definition.key.typeId == type && document::hasEffectMask(definition);
            });
        });
    expectations.expect(shaped && named && masked == 26,
                        "every grade, colour and filter effect carries the effect mask");
    const auto* glow = registry.find("bloom.glow", 2);
    const auto* matteBlur = registry.find("bloom.matte-blur", 1);
    const auto* exposure = registry.find("bloom.exposure", 1);
    expectations.expect((glow == nullptr || !document::hasEffectMask(*glow)) && matteBlur &&
                            !document::hasEffectMask(*matteBlur) && exposure &&
                            document::hasEffectMask(*exposure),
                        "Glow keeps its own mask input, a matte filter takes none, Exposure one");
    const auto* combine = registry.find(document::kMaskCombineNodeType, 1);
    expectations.expect(
        combine && combine->lowering == runtime::NodeLoweringKind::Matte &&
            combine->category == document::NodeCategory::Compositing &&
            combine->inputs.size() == 2 &&
            combine->inputs[0].valueKind == runtime::SocketValueKind::Mask &&
            !combine->inputs[0].required && !combine->inputs[1].required &&
            combine->outputs ==
                std::vector<document::OutputPortDefinition>{
                    {"mask", runtime::SocketValueKind::Mask}} &&
            combine->parameters.size() == 1 && combine->parameters[0].role == "mode",
        "Mask Combine takes two optional Masks and gives one, by a mode");
    const auto* toMask = registry.find(document::kImageToMaskNodeType, 1);
    expectations.expect(
        toMask && toMask->lowering == runtime::NodeLoweringKind::Matte &&
            toMask->inputs.size() == 1 &&
            toMask->inputs[0].valueKind == runtime::SocketValueKind::Image &&
            toMask->outputs ==
                std::vector<document::OutputPortDefinition>{
                    {"mask", runtime::SocketValueKind::Mask}} &&
            toMask->parameters.size() == 1 && toMask->parameters[0].role == "channel",
        "Image to Mask takes an Image and gives a Mask, by a channel");
}

int main() try {
    Expectations expectations;
    testValidationAndDuplicates(expectations);
    testFreezeAndBuiltIns(expectations);
    testMatteWorkflowDefinitions(expectations);
    testStructuralLoweringsRequireCanonicalKeys(expectations);
    testValueLoweringShapeContract(expectations);
    testLargeFrozenRegistryLookup(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception& error) {
    std::cerr << "Unexpected test exception: " << error.what() << '\n';
    return EXIT_FAILURE;
} catch (...) {
    std::cerr << "Unexpected non-standard test exception\n";
    return EXIT_FAILURE;
}
