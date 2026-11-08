#ifndef BLOOM_RUNTIME_TESTS_CODEC_DEGRADE_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_CODEC_DEGRADE_TEST_SUPPORT_HPP

// #563: plans that run Codec Degrade the way the snapshot compiler lowers it (codec_degrade_
// recipes.hpp, the function the compiler lowers with) over synthetic media only: an HDR checker and
// opaque blocks that move on an animated Transform 2D (or stand still), so the CPU reference tests,
// the GPU coverage fixtures and the native parity harness exercise what an authored node evaluates.

#include "codec_degrade_recipes.hpp"
#include "plan_time_scopes.hpp"

#include <bloom/document/codec_degrade_nodes.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bloom::codec_test {

using document::ParameterId;
using runtime::CompiledScalarParameter;
using runtime::OperationIndex;

inline constexpr std::uint32_t kCodecWidth = 32;
inline constexpr std::uint32_t kCodecHeight = 24;
// The animated translate moves 3 pixels right and 1 down a frame; frames are at 24 fps.
inline constexpr double kCodecPixelsPerFrameX = 3.0;
inline constexpr double kCodecPixelsPerFrameY = 1.0;

// A value differing from the node's default; every other value keeps its default.
struct CodecValues final {
    double quality = 40.0;
    double persistence = 0.5;
    std::int64_t blockPixels = 8;
    std::int64_t gopFrames = 4;
    bool chromaBleed = true;
    // The picture stands still: every frame is the same image.
    bool still = false;
    // Every edge on a multiple of four pixels, so a proxy at half or quarter size lands them on
    // whole pixels and the CPU and the GPU rasterise the same picture.
    bool aligned = false;
    // The whole picture, its format and its motion, enlarged by this whole factor (for pictures to
    // look at; the tests use 1).
    std::uint32_t scale = 1;
};

struct CodecPlan final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
};

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return CompiledScalarParameter{ParameterId::fromRaw(id), value};
}

// The moving picture -> Codec Degrade -> plain Merge -> Composition Output.
[[nodiscard]] inline CodecPlan codecPlan(const CodecValues& values = {},
                                         const std::uint64_t idBase = 56300) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kCodecWidth * values.scale, kCodecHeight * values.scale),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 30), operations,
                                  detail::definitionValueSink(definition));
    const auto vec2 = [](const std::uint64_t id, const double x, const double y) {
        return CompiledVec2Parameter{ParameterId::fromRaw(id), Vec2d{x, y}};
    };
    // Opaque rectangles at whole-pixel places over a checker: sharp edges, HDR values, and no
    // rounding for the optical flow to amplify.
    std::uint64_t nextId = idBase + 100;
    const auto solidAt = [&](const core::Color4d colour, const double x, const double y,
                             const double width, const double height) {
        const auto id = nextId;
        nextId += 8;
        const auto solid = OperationIndex::fromRaw(operations.size());
        operations.emplace_back(CompiledSolid{NodeId::fromRaw(id), {ParameterId::fromRaw(id + 1), colour},
                                              scalar(id + 2, width), scalar(id + 3, height)});
        return emitter.effect(solid, Transform2DKernel{vec2(id + 4, x, y), vec2(id + 5, 0.0, 0.0),
                                                       vec2(id + 6, 1.0, 1.0), scalar(id + 7, 0.0)});
    };
    struct Block final {
        double x, y, width, height, red, green, blue;
    };
    static constexpr std::array<Block, 6> kBlocks{{{2, 2, 7, 5, 0.9, 0.1, 0.1},
                                                   {11, 1, 6, 9, 0.1, 0.8, 0.2},
                                                   {19, 4, 9, 5, 0.1, 0.2, 0.95},
                                                   {3, 12, 8, 8, 0.8, 0.7, 0.1},
                                                   {14, 13, 11, 6, 0.6, 0.6, 0.6},
                                                   {26, 15, 5, 7, 2.0, 0.5, 0.3}}};
    std::vector<std::pair<OperationIndex, core::BlendMode>> layers;
    const double k = static_cast<double>(values.scale);
    const auto snap = [&](const double value) {
        return values.aligned ? std::max(4.0, 4.0 * std::round(value / 4.0)) : value;
    };
    for (const auto& block : kBlocks)
        layers.emplace_back(solidAt(core::Color4d{block.red, block.green, block.blue, 1.0},
                                    k * (values.aligned ? 4.0 * std::round(block.x / 4.0) : block.x),
                                    k * (values.aligned ? 4.0 * std::round(block.y / 4.0) : block.y),
                                    k * snap(block.width), k * snap(block.height)),
                            core::BlendMode::Normal);
    const auto checker = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledChecker{
        NodeId::fromRaw(idBase + 1),
        {ParameterId::fromRaw(idBase + 2), core::Color4d{0.35, 0.3, 0.25, 1.0}},
        {ParameterId::fromRaw(idBase + 3), core::Color4d{0.05, 0.1, 0.15, 1.0}},
        (values.aligned ? 4U : 5U) * values.scale,
        4U * values.scale,
        kCodecWidth * values.scale,
        kCodecHeight * values.scale});
    layers.emplace_back(checker, core::BlendMode::Normal);
    const auto texture = emitter.merge(layers);
    CompiledVec2Parameter translate = vec2(idBase + 20, 0.0, 0.0);
    if (!values.still) {
        CompiledVec2Curve curve;
        curve.id = document::AnimationCurveId::fromRaw(idBase + 21);
        const auto key = [&](const std::uint64_t id, const std::int64_t frame, const double value) {
            return CompiledScalarKeyframe{document::KeyframeId::fromRaw(id),
                                          *core::RationalTime::create(frame, 24), value,
                                          CompiledKeyframeInterpolation::Linear};
        };
        curve.components[0] = {key(idBase + 22, -20, -24.0 * k * kCodecPixelsPerFrameX),
                               key(idBase + 23, 28, 24.0 * k * kCodecPixelsPerFrameX)};
        curve.components[1] = {key(idBase + 24, -20, -24.0 * k * kCodecPixelsPerFrameY),
                               key(idBase + 25, 28, 24.0 * k * kCodecPixelsPerFrameY)};
        definition.vec2Curves.push_back(std::move(curve));
        translate.source = Vec2CurveIndex::fromRaw(0);
    }
    const auto moving = emitter.effect(
        texture, Transform2DKernel{translate, vec2(idBase + 26, 0.0, 0.0), vec2(idBase + 27, 1.0, 1.0),
                                   scalar(idBase + 28, 0.0)});
    const auto degraded = detail::appendCodecDegrade(
        emitter, moving,
        {values.blockPixels, values.gopFrames, values.chromaBleed, scalar(idBase + 90, values.quality),
         scalar(idBase + 91, values.persistence)});
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 95), {{{}, {}, degraded}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 96), merged});
    if (const auto failure = detail::scopeLayerTime(definition))
        throw std::logic_error("the codec plan scopes its time samples: " + failure->summary);
    return {std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition))};
}

} // namespace bloom::codec_test

#endif // BLOOM_RUNTIME_TESTS_CODEC_DEGRADE_TEST_SUPPORT_HPP
