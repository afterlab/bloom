#ifndef BLOOM_RUNTIME_TESTS_MOTION_BLUR_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_MOTION_BLUR_TEST_SUPPORT_HPP

// #484: plans that run every motion-blur kernel through the PRODUCTION chains (motion_blur_recipes
// .hpp, the functions the snapshot compiler lowers with) over synthetic media only: an HDR checker
// and block texture moving on an animated Transform 2D, vector strips, depth bands and a coverage
// matte. The CPU reference tests, the GPU coverage fixtures and the native parity harness all
// exercise exactly what an authored node evaluates.

#include "motion_blur_recipes.hpp"
#include "motion_shutter.hpp"
#include "plan_time_scopes.hpp"

#include <bloom/document/grade_curves.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bloom::motion_test {

using document::ParameterId;
using runtime::CompiledScalarParameter;
using runtime::CompiledVec2Parameter;
using runtime::OperationIndex;

inline constexpr std::uint32_t kMotionWidth = 32;
inline constexpr std::uint32_t kMotionHeight = 24;
// The animated translate moves 3 pixels right and 1 down a frame; frames are at 24 fps.
inline constexpr double kMotionPixelsPerFrameX = 3.0;
inline constexpr double kMotionPixelsPerFrameY = 1.0;

enum class Scene : std::uint8_t {
    Directional,
    Spin,
    Zoom,
    TransformBlur,
    SubFrame,
    Echo,
    VectorGather,
    VectorSplat,
    VectorLayered,
    FlowDis,
    FlowTvl1,
    FlowConsistency,
    FlowCut,
    FlowBlur,
    FlowBlurMatte,
    FlowBlurTemporal,
};

struct MotionValues final {
    Scene scene = Scene::Directional;
    std::int64_t quality = document::kMotionQualityStandard;
    std::int64_t curve = document::kShutterCurveBox;
    std::int64_t offset = document::kShutterOffsetCentred;
    double shutter = 0.5;
    bool jitter = false;
    bool alphaValid = false;
    bool matteSolve = false;
    bool curved = false;
    bool depth = false;
    std::int64_t border = document::kFlowBorderExtend;
    runtime::AccumulateOperator op = runtime::AccumulateOperator::Add;
    bool posterize = false;
    // The texture is still (no animated translate): every time sample is the same image.
    bool still = false;
    // Echo: the moving texture in a Layer active over these frames, so a time sample outside them
    // reads no image (the accumulator's input or its sample is then absent).
    std::optional<std::int64_t> layerIn{};
    std::optional<std::int64_t> layerOut{};
};

struct MotionPlan final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    // The scene's effect before time scoping (a plan with time samples renumbers its operations).
    OperationIndex effect = OperationIndex::fromRaw(0);
};

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return CompiledScalarParameter{ParameterId::fromRaw(id), value};
}
[[nodiscard]] inline CompiledVec2Parameter vec2(const std::uint64_t id, const double x,
                                                const double y) {
    return CompiledVec2Parameter{ParameterId::fromRaw(id), document::Vec2d{x, y}};
}

[[nodiscard]] inline runtime::CompiledShutter shutterOf(const MotionValues& values) {
    runtime::CompiledShutter shutter;
    shutter.shutter = values.shutter;
    shutter.offset = values.offset;
    shutter.curve = values.curve;
    shutter.quality = values.quality;
    shutter.minSamples = 4;
    shutter.maxSamples = 16;
    shutter.jitter = values.jitter;
    shutter.maxBlur = 64.0;
    if (values.curve == document::kShutterCurveCustom)
        shutter.customCurve = document::gradeCurvePath(
            {document::Vec2d{0.0, 0.25}, document::Vec2d{0.5, 1.0}, document::Vec2d{1.0, 0.25}});
    return shutter;
}

// The scene's plan: the texture (moving unless `still`), the scene's production chain, a plain
// Merge and the Composition Output.
[[nodiscard]] inline MotionPlan motionPlan(const MotionValues& values = {},
                                           const std::uint64_t idBase = 48400) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    const auto rate = document::FrameRate::framesPerSecond24();
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kMotionWidth, kMotionHeight),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 30), operations,
                                  detail::definitionValueSink(definition));

    // The texture: an HDR checker under opaque rectangles at whole-pixel places, so no shift of a
    // few pixels repeats. Solids, whole-pixel moves and an opaque Normal blend are bit-identical on
    // the CPU and every device, so a solver downstream sees the same binary32 input on both (a
    // smooth texture's rounding, within the 2e-6 gate, would be amplified by its iterations).
    const auto solidAt = [&](std::uint64_t& id, const core::Color4d colour, const double x,
                             const double y, const double width, const double height) {
        const auto solid = OperationIndex::fromRaw(operations.size());
        operations.emplace_back(CompiledSolid{NodeId::fromRaw(id), {ParameterId::fromRaw(id + 1), colour},
                                              scalar(id + 2, width), scalar(id + 3, height)});
        const auto placed = emitter.effect(
            solid, Transform2DKernel{vec2(id + 4, x, y), vec2(id + 5, 0.0, 0.0),
                                     vec2(id + 6, 1.0, 1.0), scalar(id + 7, 0.0)});
        id += 8;
        return placed;
    };
    std::uint64_t nextId = idBase + 100;
    struct Block final {
        double x, y, width, height, red, green, blue;
    };
    static constexpr std::array<Block, 10> kBlocks{{{2, 2, 5, 3, 3.0, 0.4, 0.2},
                                                    {9, 1, 3, 6, 0.3, 2.2, 0.5},
                                                    {14, 3, 6, 2, 1.4, 1.1, 3.0},
                                                    {23, 2, 4, 5, 0.1, 0.2, 0.9},
                                                    {4, 8, 3, 7, 2.5, 2.0, 0.1},
                                                    {11, 10, 7, 3, 0.6, 0.1, 1.8},
                                                    {21, 9, 5, 4, 4.0, 1.2, 1.0},
                                                    {2, 18, 6, 4, 0.2, 1.6, 2.4},
                                                    {12, 16, 4, 6, 1.9, 0.3, 0.7},
                                                    {20, 17, 8, 3, 0.8, 3.2, 1.3}}};
    std::vector<std::pair<OperationIndex, core::BlendMode>> layers;
    for (const auto& block : kBlocks)
        layers.emplace_back(solidAt(nextId, core::Color4d{block.red, block.green, block.blue, 1.0},
                                    block.x, block.y, block.width, block.height),
                            core::BlendMode::Normal);
    const auto checker = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledChecker{
        NodeId::fromRaw(idBase + 1),
        {ParameterId::fromRaw(idBase + 2), core::Color4d{2.0, 0.5, 0.25, 1.0}},
        {ParameterId::fromRaw(idBase + 3), core::Color4d{0.1, 0.6, 1.5, 1.0}},
        5,
        4,
        kMotionWidth,
        kMotionHeight});
    layers.emplace_back(checker, core::BlendMode::Normal);
    const auto texture = emitter.merge(layers);

    // The animated translate: 3 px right and 1 px down a frame, through frame 4 at (0, 0).
    const bool usesMoving = values.scene == Scene::SubFrame || values.scene == Scene::Echo ||
                            values.scene == Scene::VectorGather ||
                            values.scene == Scene::VectorSplat ||
                            values.scene == Scene::VectorLayered ||
                            values.scene == Scene::FlowBlur ||
                            values.scene == Scene::FlowBlurMatte ||
                            values.scene == Scene::FlowBlurTemporal;
    CompiledVec2Parameter translate = vec2(idBase + 20, 0.0, 0.0);
    if (!values.still && (usesMoving || values.scene == Scene::TransformBlur)) {
        CompiledVec2Curve curve;
        curve.id = document::AnimationCurveId::fromRaw(idBase + 21);
        const auto key = [&](const std::uint64_t id, const std::int64_t frame, const double value) {
            return CompiledScalarKeyframe{document::KeyframeId::fromRaw(id),
                                          *core::RationalTime::create(frame, 24), value,
                                          CompiledKeyframeInterpolation::Linear};
        };
        curve.components[0] = {key(idBase + 22, -20, -24.0 * kMotionPixelsPerFrameX),
                               key(idBase + 23, 28, 24.0 * kMotionPixelsPerFrameX)};
        curve.components[1] = {key(idBase + 24, -20, -24.0 * kMotionPixelsPerFrameY),
                               key(idBase + 25, 28, 24.0 * kMotionPixelsPerFrameY)};
        definition.vec2Curves.push_back(std::move(curve));
        translate.source = Vec2CurveIndex::fromRaw(0);
    }
    Transform2DKernel transform{translate, vec2(idBase + 26, 0.0, 0.0), vec2(idBase + 27, 1.0, 1.0),
                                scalar(idBase + 28, 0.0)};
    const auto moving =
        usesMoving ? emitter.effect(texture, transform) : OperationIndex::fromRaw(0);
    const auto shutter = shutterOf(values);

    // The other synthetic inputs: vectors (stored 0..4, decoded with offset -2), depth, a matte.
    // The vectors are an Image input split as Vector Motion Blur reads one (forward red and green,
    // backward blue and alpha): opaque strips, bit-identical on the CPU and every device, decoded
    // with offset -2 to a forward vector shrinking across the frame and a backward (x, -1).
    const auto vectorRamp = [&]() {
        std::vector<std::pair<OperationIndex, core::BlendMode>> strips;
        const std::array<std::array<double, 3>, 4> stored{{{3.5, 2.5, 0.5},
                                                           {2.75, 2.25, 0.75},
                                                           {1.5, 1.75, 1.25},
                                                           {0.5, 1.5, 1.5}}};
        for (std::size_t i = 0; i < stored.size(); ++i)
            strips.emplace_back(
                solidAt(nextId, core::Color4d{stored[i][0], stored[i][1], stored[i][2], 1.0},
                        static_cast<double>(i * kMotionWidth / 4), 0.0,
                        static_cast<double>(kMotionWidth / 4), static_cast<double>(kMotionHeight)),
                core::BlendMode::Normal);
        return detail::splitVectorImage(emitter, emitter.merge(strips));
    };
    const auto depthRamp = [&]() {
        std::vector<std::pair<OperationIndex, core::BlendMode>> bands;
        const std::array<double, 3> depths{1.0, 4.0, 10.0};
        for (std::size_t i = 0; i < depths.size(); ++i)
            bands.emplace_back(
                solidAt(nextId, core::Color4d{depths[i], depths[i], depths[i], 1.0}, 0.0,
                        static_cast<double>(i * kMotionHeight / 3),
                        static_cast<double>(kMotionWidth), static_cast<double>(kMotionHeight / 3)),
                core::BlendMode::Normal);
        return emitter.merge(bands);
    };
    const auto matte = [&]() {
        return solidAt(nextId, core::Color4d{1.0, 1.0, 1.0, 1.0}, 10.0, 7.0, 12.0, 10.0);
    };
    const auto flow = [&](const std::int64_t method) {
        auto kernel = detail::flowKernel(method, values.quality, values.border);
        return kernel;
    };

    MotionPlan result;
    switch (values.scene) {
    case Scene::Directional:
    case Scene::Spin:
    case Scene::Zoom: {
        MotionPathBlurKernel kernel;
        kernel.mode = values.scene == Scene::Directional ? MotionPathMode::Directional
                      : values.scene == Scene::Spin      ? MotionPathMode::Spin
                                                         : MotionPathMode::Zoom;
        kernel.shutter = shutter;
        kernel.shutter.shutter = 1.0;
        kernel.shutter.maxBlur = 64.0;
        kernel.angle = scalar(idBase + 70, 30.0);
        kernel.length = scalar(idBase + 71, 9.0);
        kernel.centre = vec2(idBase + 72, 3.0, -2.0);
        kernel.amount = scalar(idBase + 73, values.scene == Scene::Spin ? 20.0 : 0.3);
        kernel.seed = 0x484;
        result.effect = emitter.effect(texture, std::move(kernel));
        break;
    }
    case Scene::TransformBlur: {
        MotionPathBlurKernel kernel;
        kernel.mode = MotionPathMode::Transform;
        kernel.shutter = shutter;
        kernel.transform = transform;
        kernel.seed = 0x484;
        result.effect = emitter.effect(texture, std::move(kernel));
        break;
    }
    case Scene::SubFrame: {
        const auto n = detail::shutterSampleCount(shutter, static_cast<double>(shutter.maxSamples));
        const auto samples = detail::shutterSamples(shutter, n);
        const auto accumulated = detail::appendTimeSamples(
            emitter, rate, moving, samples.centre, samples.weight, AccumulateOperator::Add, true, 0,
            1);
        if (!accumulated)
            throw std::logic_error("sub-frame samples are representable");
        result.effect = *accumulated;
        break;
    }
    case Scene::Echo: {
        const std::vector<double> offsets{0.0, -1.0, -2.0, -3.0};
        const std::vector<double> weights{1.0, 0.5, 0.25, 0.125};
        auto source = moving;
        if (values.layerIn || values.layerOut) {
            CompiledLayerOutput layer{NodeId::fromRaw(idBase + 84),
                                      document::LayerId::fromRaw(idBase + 85),
                                      moving,
                                      vec2(idBase + 86, 16.0, 12.0),
                                      vec2(idBase + 87, 0.0, 0.0),
                                      vec2(idBase + 88, 1.0, 1.0),
                                      scalar(idBase + 89, 0.0),
                                      scalar(idBase + 90, 1.0),
                                      ParameterId::fromRaw(idBase + 91)};
            if (values.layerIn)
                layer.inPoint = *core::RationalTime::create(*values.layerIn, 24);
            if (values.layerOut)
                layer.outPoint = *core::RationalTime::create(*values.layerOut, 24);
            const auto placed = OperationIndex::fromRaw(operations.size());
            operations.emplace_back(std::move(layer));
            source = OperationIndex::fromRaw(operations.size());
            operations.emplace_back(CompiledMerge{
                NodeId::fromRaw(idBase + 92),
                {{document::LayerSlotId::fromRaw(idBase + 93),
                  document::LayerId::fromRaw(idBase + 85), placed}}});
        }
        const auto echoed = detail::appendTimeSamples(emitter, rate, source, offsets, weights,
                                                      values.op, false,
                                                      values.posterize ? 8000 : 0,
                                                      values.posterize ? 1000 : 1);
        if (!echoed)
            throw std::logic_error("echo samples are representable");
        result.effect = *echoed;
        break;
    }
    case Scene::VectorGather:
    case Scene::VectorSplat:
    case Scene::VectorLayered: {
        const auto vectors = vectorRamp();
        VectorBlurKernel kernel;
        kernel.stage = values.scene == Scene::VectorGather ? VectorBlurStage::Gather
                                                           : VectorBlurStage::Splat;
        kernel.decode.offset = -2.0;
        kernel.decode.layout = document::kVectorLayoutForwardBackward;
        kernel.shutter = shutter;
        kernel.curved = values.curved;
        kernel.zMode = values.depth ? 1 : 0;
        kernel.decode.alphaValid = values.alphaValid;
        kernel.seed = 0x484;
        std::vector<std::pair<std::size_t, OperationIndex>> slots{
            {kVectorForwardSlot, vectors[0]}, {kVectorBackwardSlot, vectors[1]}};
        if (values.depth)
            slots.emplace_back(kVectorDepthSlot, depthRamp());
        if (values.scene != Scene::VectorLayered) {
            result.effect = detail::appendVectorStage(emitter, moving, kernel, slots);
            break;
        }
        const auto m = matte();
        VectorBlurKernel fill;
        fill.stage = VectorBlurStage::Fill;
        fill.threshold = 0.5;
        fill.shutter = shutter;
        const auto background =
            detail::appendVectorStage(emitter, moving, fill, {{kVectorMatteSlot, m}});
        auto foregroundSlots = slots;
        foregroundSlots.emplace_back(kVectorMatteSlot, m);
        auto foreground = kernel;
        foreground.layer = 1;
        // Merge entries are top first: the foreground over the blurred background.
        const auto blurredForeground =
            detail::appendVectorStage(emitter, moving, foreground, foregroundSlots);
        result.effect = emitter.merge(
            {{blurredForeground, core::BlendMode::Normal},
             {detail::appendVectorStage(emitter, background, kernel, slots),
              core::BlendMode::Normal}});
        break;
    }
    case Scene::FlowDis:
    case Scene::FlowTvl1:
    case Scene::FlowConsistency:
    case Scene::FlowCut: {
        // The flow from the still texture to the same texture shifted 2 right and 1 down (the
        // unrelated cut frame: a vertical ramp).
        const auto kernel = flow(values.scene == Scene::FlowDis ? document::kFlowMethodDis
                                                                : document::kFlowMethodTvl1);
        OperationIndex target = OperationIndex::fromRaw(0);
        if (values.scene == Scene::FlowCut && values.depth) {
            target = emitter.merge(
                {{solidAt(nextId, core::Color4d{4.0, 0.0, 4.0, 1.0}, 0.0, 0.0, 16.0, 24.0),
                  core::BlendMode::Normal},
                 {solidAt(nextId, core::Color4d{0.0, 4.0, 0.0, 1.0}, 16.0, 0.0, 16.0, 24.0),
                  core::BlendMode::Normal}});
        } else {
            target = emitter.effect(
                texture, Transform2DKernel{vec2(idBase + 80, 2.0, 1.0), vec2(idBase + 81, 0.0, 0.0),
                                           vec2(idBase + 82, 1.0, 1.0), scalar(idBase + 83, 0.0)});
        }
        auto solve = kernel;
        std::optional<OperationIndex> solveMatte;
        if (values.matteSolve) {
            // Optical Flow's Matte: the data term of the matte's layer alone.
            solve.layer = 1;
            solveMatte = matte();
        }
        const auto forward = detail::appendFlowSolve(emitter, texture, target, solve, solveMatte);
        if (values.scene == Scene::FlowDis || values.scene == Scene::FlowTvl1) {
            result.effect = forward;
        } else if (values.scene == Scene::FlowConsistency) {
            const auto back = detail::appendFlowSolve(emitter, target, texture, kernel);
            VectorBlurKernel check;
            check.stage = VectorBlurStage::Consistency;
            check.border = values.border;
            check.shutter = shutter;
            check.layer = values.depth ? 1 : 0;
            result.effect =
                detail::appendVectorStage(emitter, forward, check, {{kVectorForwardSlot, back}});
        } else {
            auto cut = kernel;
            cut.stage = OpticalFlowStage::CutDetect;
            cut.cutThreshold = 0.5;
            cut.slotImages = {0, -1, 1};
            result.effect = emitter.effect(texture, cut);
            emitter.setImages(result.effect, {target, forward});
        }
        break;
    }
    case Scene::FlowBlur:
    case Scene::FlowBlurMatte:
    case Scene::FlowBlurTemporal: {
        detail::FlowBlurOptions options;
        options.shutter = shutter;
        options.shutter.offset = document::kShutterOffsetCentred;
        options.border = values.border;
        options.flow = flow(document::kFlowMethodAuto);
        options.seed = 0x484;
        if (values.scene != Scene::FlowBlur) {
            options.matte = matte();
            options.matteShrink = 1.0;
            options.alphaBoost = 1.25;
            options.holeFill = values.scene == Scene::FlowBlurTemporal
                                   ? document::kHoleFillTemporal
                                   : document::kHoleFillOutsideIn;
        }
        const auto blurred = detail::appendFlowMotionBlur(emitter, rate, moving, options);
        if (!blurred)
            throw std::logic_error("optical-flow motion blur is representable");
        result.effect = blurred->image;
        break;
    }
    }
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 95), {{{}, {}, result.effect}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 96), merged});
    if (const auto failure = detail::scopeLayerTime(definition))
        throw std::logic_error("the motion plan scopes its time samples: " + failure->summary);
    result.plan = std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
    return result;
}

} // namespace bloom::motion_test

#endif // BLOOM_RUNTIME_TESTS_MOTION_BLUR_TEST_SUPPORT_HPP
