// #430: parented text renders at every size.
//
// The reported repro, built through commands on a real document: a 2x2 null Solid with anchor
// (0, 0) at the composition centre and a Text layer parented to it. Sizes 12..48 are swept in two
// placements: the text's default position (which, read in the parent's space, puts it at the
// bottom-right corner of the frame) and position (0, 0) in the parent's space, which composes to
// exactly the matrix of an unparented text layer at the parent's top-left. The CPU reference of
// that parented layer must equal the unparented one pixel for pixel; then every state is prepared
// by the production CpuGpuSceneBuilder, run by the production GpuSceneExecutor, and must match the
// CPU reference pixels (2e-6) and bounds, with a warm rerun that dispatches nothing new.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>

#include <cstring>
#include <iostream>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 160;

document::CompositionFormat format() {
    return *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                                *document::FrameRate::create(24, 1));
}

enum class Placement { ParentedDefault, ParentedOrigin, Unparented };

[[nodiscard]] std::optional<Reference> textState(Expectations& expectations, const int size,
                                                 const Placement placement,
                                                 const std::string& label) {
    auto initial = document::makeNewProject("Parented text", "Main",
                                            core::RationalTime::fromInteger(2), format());
    const auto main = initial.initialCompositionId;
    document::Document document(std::move(initial.project));
    commands::CommandStack commands(document);
    // The null: a 2x2 Solid at the composition centre with its default anchor (0, 0).
    execute<commands::AddSolidLayer>(document, commands, main, "Null",
                                     core::Color4d{0.0, 0.0, 0.0, 0.0},
                                     document::Vec2d{kWidth / 2.0, kHeight / 2.0});
    const auto null = lastLayer(document, main);
    const auto solid = lastNode(document, main, document::kSolidSourceNodeType);
    for (const auto* role : {"width", "height"})
        execute<commands::SetParameterSource>(
            document, commands, main, parameter(document, main, solid, role),
            document::ConstantValueSource{document::ParameterValue{2.0}});
    // The parent's space starts at its content top-left, (159, 79) in the composition.
    const std::optional<document::Vec2d> position =
        placement == Placement::ParentedDefault ? std::nullopt
        : placement == Placement::ParentedOrigin
            ? std::optional{document::Vec2d{0.0, 0.0}}
            : std::optional{document::Vec2d{kWidth / 2.0 - 1.0, kHeight / 2.0 - 1.0}};
    execute<commands::AddTextLayer>(document, commands, main, "Text", "Hello", position, 1.0,
                                    static_cast<double>(size));
    // This sweep pins parented placement of a text block centred on its position, which is what a
    // stored Left/Center/Right anchor mode (every project saved before the anchor followed the
    // alignment) still does; the following anchor is covered by the text anchor tests.
    {
        const auto text = lastNode(document, main, document::kTextSourceNodeType);
        execute<commands::SetParameterSource>(
            document, commands, main, parameter(document, main, text, "anchorMode"),
            document::ConstantValueSource{document::ParameterValue{std::int64_t{0}}});
    }
    if (placement != Placement::Unparented)
        execute<commands::SetLayerParent>(document, commands, main, lastLayer(document, main),
                                          null);
    return cpuReference(expectations, document, main, label);
}

[[nodiscard]] bool samePixels(const Reference& a, const Reference& b) {
    const auto left = a.frame->processImage().pixels();
    const auto right = b.frame->processImage().pixels();
    return left.size_bytes() == right.size_bytes() &&
           std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        std::vector<std::pair<std::string, Reference>> references;
        for (int size = 12; size <= 48; ++size) {
            const auto name = "size " + std::to_string(size);
            const auto atDefault =
                textState(expectations, size, Placement::ParentedDefault, name + " default");
            const auto atOrigin =
                textState(expectations, size, Placement::ParentedOrigin, name + " origin");
            const auto unparented =
                textState(expectations, size, Placement::Unparented, name + " unparented");
            if (!atDefault || !atOrigin || !unparented)
                continue;
            // At its default position the text's centre lands on the frame's bottom-right
            // corner, so its top-left quadrant is visible.
            const auto corner = coverage(*atDefault);
            expectations.expect(!corner.empty() && corner.left >= kWidth / 2 &&
                                    corner.top >= kHeight / 2,
                                name + ": parented text at its default position renders at the "
                                       "bottom-right corner");
            expectations.expect(!coverage(*atOrigin).empty(),
                                name + ": parented text at the parent origin renders");
            expectations.expect(samePixels(*atOrigin, *unparented),
                                name + ": parented text equals the unparented layer with the "
                                       "same composed matrix");
            references.emplace_back(name + " default", *atDefault);
            references.emplace_back(name + " origin", *atOrigin);
            references.emplace_back(name + " unparented", *unparented);
        }
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " parented text expectation(s) failed\n";
            return 1;
        }

        render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = render::GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: CPU sweep passed; no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 77;
        }
        auto cache =
            runtime::GpuSceneCache::create(*device.device, runtime::GpuSceneCacheBudgets{});
        expectations.expect(cache.hasValue(), "the scene cache creates");
        if (!cache)
            return 1;
        auto executor = runtime::GpuSceneExecutor::create(*device.device, *cache.cache);
        expectations.expect(executor.hasValue(), "the scene executor creates");
        if (!executor)
            return 1;
        for (const auto& [label, reference] : references)
            gpuParity(expectations, *executor.executor, reference, label);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " parented text expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: parented text renders at every size on the GPU and the CPU reference "
                     "alike\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
