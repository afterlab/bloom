#pragma once

#include <array>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document {
enum class AssetKind : std::uint8_t { Image, Sequence, Audio, Font, Video, Lut };
enum class AssetColorSpace : std::uint8_t { Auto, Srgb, Linear, Raw };
enum class AssetAlphaAssociation : std::uint8_t { Straight, Premultiplied };
inline constexpr std::size_t kMaxAssetInputColorSpaceIdBytes = 256;
// Interpret Footage (#381, document 1.34). How the asset's alpha channel is read. Guess follows the
// association the file declared at import (`alphaAssociation`), and leaves each image node's own
// Premultiply switch in charge; the explicit modes apply to every use of the asset.
enum class AssetAlphaMode : std::uint8_t { Guess, Ignore, Straight, Premultiplied };
// The field order the asset is read with. File uses the order captured at import.
enum class AssetFieldOrder : std::uint8_t { File, Progressive, UpperFirst, LowerFirst };
// What a sequence shows for a frame its manifest does not have.
enum class AssetMissingFrames : std::uint8_t { HoldPrevious, Nothing };
inline constexpr std::uint32_t kMaxAssetLoopCount = 1000;
struct AssetInterpretation {
    AssetColorSpace colorSpace = AssetColorSpace::Auto;
    // Empty means Auto. Non-empty values are exact ids from the qualified project OCIO config.
    std::string inputColorSpaceId;
    // The association the file declared at import (what Guess follows).
    AssetAlphaAssociation alphaAssociation = AssetAlphaAssociation::Straight;
    // Document 1.34. A sequence's assumed rate (absent: one member per composition frame, the
    // pre-1.34 timing) or a video's conformed rate (absent: the file's rate).
    std::optional<FrameRate> frameRate;
    // Absent: the source's own start timecode.
    std::optional<CompositionTimecode> startTimecode;
    AssetAlphaMode alphaMode = AssetAlphaMode::Guess;
    bool invertAlpha = false;
    // The colour a Premultiplied alpha was matted against, in the file's own encoding, [0, 1].
    std::array<double, 3> matteColor{0.0, 0.0, 0.0};
    AssetFieldOrder fieldOrder = AssetFieldOrder::File;
    bool deinterlace = false;
    // Absent: the file's pixel aspect ratio.
    std::optional<core::PixelAspectRatio> pixelAspect;
    // The footage plays this many times end to end; its natural length is multiplied.
    std::uint32_t loopCount = 1;
    AssetMissingFrames missingFrames = AssetMissingFrames::HoldPrevious;
    friend bool operator==(const AssetInterpretation&, const AssetInterpretation&) = default;
};
// The validation of the 1.34 Interpret Footage members, shared by AssetRecord::validate() and the
// commands that edit them. Empty when valid.
[[nodiscard]] std::string_view footageInterpretationIssue(const AssetInterpretation& value);
// The 1.34 members that only some asset kinds may set (a frame rate on a still is refused).
[[nodiscard]] std::string_view footageInterpretationKindIssue(AssetKind kind,
                                                              const AssetInterpretation& value);
// The frame rate and pixel aspect bounds an Interpret Footage override may use.
[[nodiscard]] bool isValidFootageFrameRate(FrameRate rate) noexcept;
[[nodiscard]] bool isValidFootagePixelAspect(core::PixelAspectRatio ratio) noexcept;
struct EffectiveAssetInputColor final {
    AssetColorSpace colorSpace = AssetColorSpace::Auto;
    std::string inputColorSpaceId;
};
// A source's explicit config id wins first, then its explicit legacy choice. Only when the
// source inherits both does the asset's config id (or legacy choice) apply. Keep this single
// precedence rule shared by stills, video and UI proxies so every visible choice changes pixels.
[[nodiscard]] inline EffectiveAssetInputColor
effectiveAssetInputColor(const AssetInterpretation& asset, const std::int64_t sourceColorSpace,
                         const std::string_view sourceInputColorSpaceId) {
    if (!sourceInputColorSpaceId.empty())
        return {AssetColorSpace::Auto, std::string(sourceInputColorSpaceId)};
    if (sourceColorSpace != 0)
        return {static_cast<AssetColorSpace>(sourceColorSpace), {}};
    if (!asset.inputColorSpaceId.empty())
        return {AssetColorSpace::Auto, asset.inputColorSpaceId};
    return {asset.colorSpace, {}};
}
struct AssetLocator {
    std::string kind = "file";
    std::string portability = "project-relative";
    std::string path;
    std::string relinkHint;
    friend bool operator==(const AssetLocator&, const AssetLocator&) = default;
};
struct AssetSequenceMember {
    std::int64_t frame = 0;
    AssetLocator locator;
    core::Sha256Digest contentDigest;
    friend bool operator==(const AssetSequenceMember&, const AssetSequenceMember&) = default;
};
struct AssetSequenceManifest {
    std::string pattern;
    std::uint32_t padding = 0;
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::vector<AssetSequenceMember> members;
    std::vector<std::int64_t> gaps;
    [[nodiscard]] bool hasGaps() const noexcept {
        if (!gaps.empty())
            return true;
        for (std::size_t index = 1; index < members.size(); ++index)
            if (members[index].frame > members[index - 1].frame &&
                static_cast<std::uint64_t>(members[index].frame) -
                        static_cast<std::uint64_t>(members[index - 1].frame) >
                    1)
                return true;
        return false;
    }
    friend bool operator==(const AssetSequenceManifest&, const AssetSequenceManifest&) = default;
};
struct AssetFolder {
    AssetFolderId id;
    std::string name;
    std::optional<AssetFolderId> parent;
    friend bool operator==(const AssetFolder&, const AssetFolder&) = default;
};
struct AssetVideoStream {
    std::uint32_t id = 0, kind = 1;
    std::string codec, profile, pixelFormat, timecode;
    core::RationalTime timebase{}, framePeriod{}, duration{};
    std::uint32_t width = 1, height = 1, sampleRate = 0;
    std::int64_t primaries = -1, transfer = -1, matrix = -1, range = -1;
    // Document 1.25. Coded field order (0 unknown, 1 progressive, 2 top first, 3 bottom first,
    // 4 top coded/bottom displayed first, 5 the reverse) and timecode type (0 none, 1 non-drop,
    // 2 drop-frame), captured at import and shown in Assets and Properties.
    std::uint32_t fieldOrder = 0, timecodeType = 0;
    std::vector<std::string> channelLayout;
    // Document 1.34. The coded pixel aspect ratio the decoder already resamples to square pixels
    // (width above is the display width); square when the file declares none or predates 1.34.
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    friend bool operator==(const AssetVideoStream&, const AssetVideoStream&) = default;
};
// SMPTE timecode type from its text: drop-frame timecode separates frames with ';' (or '.'),
// non-drop with ':'; no timecode is type 0. Import and the 1.23 -> 1.25 migration share it.
[[nodiscard]] inline std::uint32_t timecodeTypeFor(const std::string_view timecode) noexcept {
    if (timecode.empty())
        return 0;
    return timecode.find_first_of(";.") != std::string_view::npos ? 2U : 1U;
}
struct AssetRecord {
    AssetId id;
    AssetKind kind = AssetKind::Image;
    AssetLocator locator;
    core::Sha256Digest contentDigest;
    AssetInterpretation interpretation;
    AssetSequenceManifest manifest;
    // Source descriptor captured by import, used without filesystem I/O by artist-facing views.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    core::RationalTime duration{};
    // Font metadata is captured when the artist picks a face. It keeps the Properties and Assets
    // surfaces useful even when the system font later disappears; the renderer still verifies the
    // locator bytes against contentDigest at compile time.
    std::vector<AssetVideoStream> videoStreams;
    std::string fontFamily;
    std::string fontStyle;
    std::uint32_t fontIndex = 0;
    std::string name;
    std::optional<AssetFolderId> folder;
    std::vector<std::string> tags;
    std::uint64_t order = 0;
    [[nodiscard]] ValidationResult validate() const;
    friend bool operator==(const AssetRecord&, const AssetRecord&) = default;
};
// Interpret Footage (#381) resolution, shared by the runtime, commands and every UI surface so a
// visible choice and the rendered result cannot disagree.
//
// The file's own frame rate: the first video stream's, else nullopt (sequences have none).
[[nodiscard]] std::optional<FrameRate> footageFileFrameRate(const AssetRecord& asset);
// The rate the asset's frames advance at: a video's conformed rate or file rate, a sequence's
// assumed rate. Nullopt for a sequence that matches the composition and for non-timed assets.
[[nodiscard]] std::optional<FrameRate> footageFrameRate(const AssetRecord& asset);
// Frames in one pass of timed footage (sequence first..last, video frames); 0 otherwise.
[[nodiscard]] std::uint64_t footageFrameCount(const AssetRecord& asset);
// The natural length of timed footage including Loop, at `compositionRate` for a sequence that
// matches the composition. Nullopt for stills, audio and anything without a length.
[[nodiscard]] std::optional<core::RationalTime> footageDuration(const AssetRecord& asset,
                                                                FrameRate compositionRate);
// The field order in the stream numbering (0 unknown, 1 progressive, 2 top first, 3 bottom first,
// 4 and 5 the coded/displayed reverses), after any override.
[[nodiscard]] std::uint32_t footageFieldOrder(const AssetRecord& asset);
// The file's pixel aspect ratio (a video stream's coded ratio; square for stills and sequences).
[[nodiscard]] core::PixelAspectRatio footageFilePixelAspect(const AssetRecord& asset);
// The pixel aspect the asset is read with: the override, else the file's.
[[nodiscard]] core::PixelAspectRatio footagePixelAspect(const AssetRecord& asset);
// The source start timecode: the override, else a video stream's timecode text at its rate.
[[nodiscard]] std::optional<CompositionTimecode> footageStartTimecode(const AssetRecord& asset);
// Lexical only: importing and migration share a portable UTF-8 file-stem default.
[[nodiscard]] std::string defaultAssetName(const AssetRecord& asset);
} // namespace bloom::document
