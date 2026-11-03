#pragma once
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <span>
#include <string>
#include <string_view>

namespace bloom::output {
// The built-in OCIO config an output colour intent names, resolved exactly as
// PreparedOutputDisplayV1::prepare resolves it (an empty revision selects Bloom Neutral).
[[nodiscard]] std::optional<color::ResolvedBloomNeutralConfig>
resolveOutputDisplayConfigV1(const runtime::EvaluationColorIntent&);
// Issue #409: the display/view an output resolves to. An output that names neither uses the
// Viewer's default pair (color::viewerDefaultDisplayView: srgb_rec709_display under Bloom Neutral,
// sRGB - Display / Un-tone-mapped under ACES CG), never the config's own default view, which under
// ACES CG is a tone map. A named member is kept. An empty view takes the Viewer's default view
// when the display offers it, otherwise the config's default view; an empty display takes the
// Viewer's default display when it offers the named view, otherwise the config's default display.
struct OutputDisplayViewV1 final {
    std::string display;
    std::string view;
    friend bool operator==(const OutputDisplayViewV1&, const OutputDisplayViewV1&) = default;
};
[[nodiscard]] OutputDisplayViewV1 outputDisplayViewV1(std::span<const color::DisplayViewEntry>,
                                                      std::string_view display,
                                                      std::string_view view);
[[nodiscard]] OutputDisplayViewV1 outputDisplayViewV1(const color::ResolvedBloomNeutralConfig&,
                                                      std::string_view display,
                                                      std::string_view view);
class PreparedOutputDisplayV1 final {
  public:
    [[nodiscard]] static std::shared_ptr<const PreparedOutputDisplayV1>
    prepare(const runtime::EvaluationColorIntent&, std::string_view display = {},
            std::string_view view = {});
    [[nodiscard]] const color::PreparedCpuDisplayProcessorHandle& processor() const noexcept {
        return *processor_;
    }
    [[nodiscard]] const std::string& description() const noexcept { return description_; }
    [[nodiscard]] const core::Sha256Digest& digest() const noexcept { return digest_; }
    // The resolved display and view (outputDisplayViewV1 of the requested names).
    [[nodiscard]] const std::string& displayName() const noexcept { return display_; }
    [[nodiscard]] const std::string& viewName() const noexcept { return view_; }

  private:
    PreparedOutputDisplayV1() = default;
    std::string display_;
    std::string view_;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor_;
    std::string description_;
    core::Sha256Digest digest_;
};
// Issue #369: the container colour tags (H.273 primaries, transfer and matrix; FFmpeg range
// convention) that describe pixels encoded through `display`. Known display families map to
// their standard code points; an unknown display is tagged unspecified rather than guessed.
struct OutputColourTagsV1 final {
    std::int32_t primaries = 2, transfer = 2, matrix = 1, range = 1;
    friend bool operator==(const OutputColourTagsV1&, const OutputColourTagsV1&) = default;
};
[[nodiscard]] OutputColourTagsV1 outputColourTagsV1(std::string_view display) noexcept;
[[nodiscard]] std::uint64_t
outputLookEffectCountV1(const runtime::CompiledCompositionPlan&) noexcept;
[[nodiscard]] std::string outputLookDescriptionV1(const runtime::ProcessFrameIdentity&);
} // namespace bloom::output
