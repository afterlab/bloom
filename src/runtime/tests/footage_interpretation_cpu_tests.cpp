// Interpret Footage (#381) on the CPU reference: a missing sequence frame that shows nothing, the
// held frame it replaces, and a pixel aspect override that widens the layer's content.
#include "footage_interpretation_plan_support.hpp"

#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <filesystem>
#include <iostream>

namespace {
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
[[nodiscard]] bool transparent(const bloom::render::Rgba32fImage& image) {
    for (const auto& pixel : image.pixels())
        if (pixel.alpha() != 0.0F)
            return false;
    return true;
}
} // namespace

int main() {
    try {
        const auto directory =
            std::filesystem::temp_directory_path() / "bloom_footage_interpretation_cpu_test";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        std::vector<ExrPixel> opaque(16, ExrPixel{0.5F, 0.25F, 0.125F, 1.0F});
        for (const int frame : {1, 3})
            writeExrRgba(directory / ("gap." + std::to_string(frame) + ".exr"), 4, 4, opaque);
        auto sequence = imageAsset(directory / "gap.1.exr", "gap", 800);
        sequence.kind = document::AssetKind::Sequence;
        sequence.manifest.pattern = "gap.#.exr";
        sequence.manifest.padding = 1;
        sequence.manifest.first = 1;
        sequence.manifest.last = 3;
        sequence.manifest.gaps = {2};
        for (const int frame : {1, 3})
            sequence.manifest.members.push_back(
                sequenceMember(directory / ("gap." + std::to_string(frame) + ".exr"), frame, 0));
        bloom::runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(directory);
        const auto evaluate = [&](const document::AssetRecord& asset, const std::int64_t frame) {
            const auto plan = mediaPlan(format(8, 8), asset,
                                        LayerValues{.position = {4.0, 4.0}, .opacity = 1.0}, 94000);
            auto request = requestFor(*plan);
            request.time = *bloom::core::RationalTime::create(frame, 24);
            request.bypassOperationCache = true;
            return evaluator.evaluate(plan, request, {});
        };
        auto held = evaluate(sequence, 1);
        expect(held.frame() != nullptr && !transparent(held.frame()->processImage()),
               "a missing frame holds the previous frame by default");
        auto nothing = sequence;
        nothing.interpretation.missingFrames = document::AssetMissingFrames::Nothing;
        auto empty = evaluate(nothing, 1);
        expect(empty.frame() != nullptr && transparent(empty.frame()->processImage()),
               "Show Nothing leaves a missing frame transparent");
        auto present = evaluate(nothing, 2);
        expect(present.frame() != nullptr && !transparent(present.frame()->processImage()),
               "a present frame still shows");
        if (empty.frame() != nullptr && present.frame() != nullptr)
            expect(empty.frame()->evaluatedBounds()[1].local ==
                       present.frame()->evaluatedBounds()[1].local,
                   "a missing frame keeps the sequence's frame bounds");

        // Pixel aspect: 2:1 doubles the layer's content width about its anchor.
        const auto still = imageAsset(directory / "gap.1.exr", "still", 801);
        const auto aspectFrame = [&](const double aspect) {
            const auto plan =
                aspectPlan(format(16, 8), still,
                           LayerValues{.position = {8.0, 4.0}, .opacity = 1.0}, 94100, aspect);
            auto request = requestFor(*plan);
            request.bypassOperationCache = true;
            return evaluator.evaluate(plan, request, {});
        };
        const auto square = aspectFrame(1.0);
        const auto wide = aspectFrame(2.0);
        expect(square.frame() != nullptr && wide.frame() != nullptr, "the aspect plans evaluate");
        if (square.frame() != nullptr && wide.frame() != nullptr) {
            const auto& a = square.frame()->evaluatedBounds()[1].output;
            const auto& b = wide.frame()->evaluatedBounds()[1].output;
            expect(std::abs((b.right - b.left) - 2.0 * (a.right - a.left)) < 1e-9 &&
                       std::abs((b.bottom - b.top) - (a.bottom - a.top)) < 1e-9 &&
                       std::abs((b.left + b.right) - (a.left + a.right)) < 1e-9,
                   "a 2:1 pixel aspect doubles the layer's width about its centre");
        }
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
