#include <algorithm>
#include <bloom/color/ocio_config_resolution.hpp>
#include <bloom/output/display_output.hpp>

namespace bloom::output {
std::optional<color::ResolvedBloomNeutralConfig>
resolveOutputDisplayConfigV1(const runtime::EvaluationColorIntent& intent) {
    const auto revision = intent.ocioConfigRevision == core::Sha256Digest{}
                              ? color::kBloomNeutralV1ConfigDigest
                              : intent.ocioConfigRevision;
    const auto uri =
        revision == color::kBloomNeutralV1ConfigDigest ? color::kBloomNeutralV1ConfigUri
        : intent.ocioConfigUri == runtime::kBloomNeutralOcioConfigUri ? color::kAcesCgV1ConfigUri
                                                                      : intent.ocioConfigUri;
    // An external config resolves through the supervised helper.
    auto resolution = color::resolveOcioConfig(uri, revision, intent.workingColorSpaceId);
    return std::move(resolution).takeResolved();
}
OutputDisplayViewV1 outputDisplayViewV1(const std::span<const color::DisplayViewEntry> displays,
                                        const std::string_view display,
                                        const std::string_view view) {
    const auto* viewer = color::viewerDefaultDisplayView(displays);
    const auto configDefault =
        std::ranges::find_if(displays, [](const auto& entry) { return entry.isDefault; });
    const auto offers = [&](const std::string_view d, const std::string_view v) {
        return std::ranges::any_of(displays, [&](const color::DisplayViewEntry& entry) {
            return entry.display == d && entry.view == v;
        });
    };
    if (viewer == nullptr || configDefault == displays.end())
        return {std::string(display), std::string(view)};
    if (display.empty() && view.empty())
        return {viewer->display, viewer->view};
    OutputDisplayViewV1 result{std::string(display), std::string(view)};
    if (result.view.empty())
        result.view = offers(display, viewer->view) ? viewer->view : configDefault->view;
    if (result.display.empty())
        result.display = offers(viewer->display, view) ? viewer->display : configDefault->display;
    return result;
}
OutputDisplayViewV1 outputDisplayViewV1(const color::ResolvedBloomNeutralConfig& config,
                                        const std::string_view display,
                                        const std::string_view view) {
    return outputDisplayViewV1(config.displays(), display, view);
}
std::shared_ptr<const PreparedOutputDisplayV1>
PreparedOutputDisplayV1::prepare(const runtime::EvaluationColorIntent& intent,
                                 std::string_view display, std::string_view view) {
    const auto revision = intent.ocioConfigRevision == core::Sha256Digest{}
                              ? color::kBloomNeutralV1ConfigDigest
                              : intent.ocioConfigRevision;
    auto config = resolveOutputDisplayConfigV1(intent);
    if (!config)
        return {};
    const auto chosen = outputDisplayViewV1(*config, display, view);
    display = chosen.display;
    view = chosen.view;
    auto built = color::buildCpuDisplayProcessorForView(*config, display, view);
    auto handle = std::move(built).takeHandle();
    if (!handle)
        return {};
    auto result = std::shared_ptr<PreparedOutputDisplayV1>(new PreparedOutputDisplayV1());
    result->processor_ =
        std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(std::move(*handle));
    result->display_ = std::string(display);
    result->view_ = std::string(view);
    result->description_ = "display: " + std::string(display) + "; view: " + std::string(view);
    const auto hex = revision.toLowercaseHex();
    result->description_ += "; OCIO: " + std::string(hex.data(), hex.size());
    core::Sha256Hasher hasher;
    (void)hasher.update(result->processor_->identity().canonicalBytes());
    result->digest_ = hasher.finalize();
    return result;
}
OutputColourTagsV1 outputColourTagsV1(const std::string_view display) noexcept {
    // Case-insensitive: Bloom Neutral names its display with an interchange identifier
    // ("srgb_rec709_display"), which a case-sensitive "sRGB" missed, so every Neutral video export
    // was tagged with unspecified primaries and transfer (and ProRes carried no colour atom).
    const auto lower = [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const auto has = [&](const std::string_view text) {
        return std::ranges::search(display, text, {}, lower, lower).begin() != display.end();
    };
    // H.273: primaries 1 BT.709, 9 BT.2020, 12 P3-D65; transfer 1 BT.709, 13 sRGB, 16 PQ,
    // 18 HLG; matrix 1 BT.709, 9 BT.2020 non-constant.
    if (has("2100") && has("PQ"))
        return {9, 16, 9, 1};
    if (has("2100") && has("HLG"))
        return {9, 18, 9, 1};
    if (has("P3-D65") || has("Display P3"))
        return {12, has("1886") || has("Gamma 2.6") ? 1 : 13, 1, 1};
    // sRGB before Rec.709: "srgb_rec709_display" is the sRGB display with Rec.709 primaries.
    if (has("sRGB"))
        return {1, 13, 1, 1};
    if (has("1886") || has("Rec.709") || has("BT.709") || has("Rec709"))
        return {1, 1, 1, 1};
    return {2, 2, 1, 1};
}
std::uint64_t outputLookEffectCountV1(const runtime::CompiledCompositionPlan& plan) noexcept {
    std::uint64_t result = 0;
    for (const auto& operation : plan.operations())
        if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
            effect && effect->look && !effect->bypass)
            ++result;
    for (const auto& nested : plan.nestedPlans())
        if (nested)
            result += outputLookEffectCountV1(*nested);
    return result;
}
std::string outputLookDescriptionV1(const runtime::ProcessFrameIdentity& identity) {
    if (identity.bypassLookNodes)
        return "Look: OFF (handoff)";
    return "look: baked (" +
           std::to_string(identity.plan ? outputLookEffectCountV1(*identity.plan) : 0) +
           " look-tagged effects)";
}
} // namespace bloom::output
