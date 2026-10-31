#pragma once

// Interpret Footage (#381) test plans: mediaPlan() with the Layer Output's footage pixel aspect.

#include "gpu_media_scene_preparation_test_support.hpp"

namespace {

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
aspectPlan(const CompositionFormat compositionFormat, const document::AssetRecord& asset,
           const LayerValues layer, const std::uint64_t idBase, const double pixelAspect) {
    const LayerIds ids{bloom::document::ParameterId::fromRaw(idBase + 0),
                       bloom::document::ParameterId::fromRaw(idBase + 1),
                       bloom::document::ParameterId::fromRaw(idBase + 2),
                       bloom::document::ParameterId::fromRaw(idBase + 3),
                       bloom::document::ParameterId::fromRaw(idBase + 4),
                       bloom::document::ParameterId::fromRaw(idBase + 5)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(bloom::runtime::CompiledImageSource{
        bloom::document::NodeId::fromRaw(idBase + 10), asset, 0, 0, 0, std::string{}, false});
    auto output = layerOutput(bloom::document::NodeId::fromRaw(idBase + 11),
                              bloom::document::LayerId::fromRaw(idBase + 12),
                              OperationIndex::fromRaw(0), ids, layer);
    output.sourcePixelAspect = pixelAspect;
    operations.emplace_back(std::move(output));
    operations.emplace_back(CompiledMerge{
        bloom::document::NodeId::fromRaw(idBase + 13),
        std::vector<CompiledMergeInput>{CompiledMergeInput{
            bloom::document::LayerSlotId::fromRaw(idBase + 14),
            bloom::document::LayerId::fromRaw(idBase + 12), OperationIndex::fromRaw(1)}}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 15),
                                                      OperationIndex::fromRaw(2)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(3)});
}

// Both test programs include the shared media helpers but use different subsets of them.
[[maybe_unused]] void footageTestHelpersUsed() {
    (void)&mediaPlan;
    (void)&signedHdrPixels;
}

} // namespace
