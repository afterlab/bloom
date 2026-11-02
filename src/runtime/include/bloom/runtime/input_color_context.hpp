#pragma once

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime::detail {

struct InputColorSpaceResolution final {
    std::string id;
    std::string name;
    std::string warning;
    bool noConversion = false;
    bool automatic = false;
};

[[nodiscard]] std::optional<color::ResolvedBloomNeutralConfig>
resolveInputColorConfig(const EvaluationColorIntent& intent);

[[nodiscard]] InputColorSpaceResolution
resolveImageInputColorSpace(const color::ResolvedBloomNeutralConfig& config,
                            const media::ImageProbe& probe, media::ImageColorSpace legacy,
                            std::string_view explicitId);

[[nodiscard]] std::shared_ptr<const color::CpuColorSpaceProcessor>
prepareInputColorProcessor(const color::ResolvedBloomNeutralConfig& config,
                           const InputColorSpaceResolution& resolution, std::string& diagnostic);

// ADR 0032 compatibility: true for a video asset whose input colour is Auto (no explicit config
// space and no legacy choice) and whose captured stream tags make Auto show it as ordinary SDR
// video through the sRGB texture space. Projects saved before document 1.28 showed exactly these
// assets through the Rec.709 video space, so opening one shows a one-time note.
[[nodiscard]] bool assetUsesSdrVideoColourDefault(const document::AssetRecord& asset);

// Issue #407: true for an asset Bloom interprets through the synthesized Rec.2100 HLG input space,
// chosen explicitly or, for a video in Auto, from its HLG transfer tag. That space used to keep
// OCIO's scale and now follows BT.2408 (the 75% signal is 1.0), so the same footage looks about
// 1.26 times brighter; opening a project saved before document 1.32 with such an asset shows a
// one-time note.
[[nodiscard]] bool assetUsesHlgInput(const document::AssetRecord& asset);

} // namespace bloom::runtime::detail
