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
// Interpret Footage (#381). How the asset's alpha channel is read. Guess follows the
// association the file declared at import (`alphaAssociation`), and leaves each image node's own
// Premultiply switch in charge; the explicit modes apply to every use of the asset.
enum class AssetAlphaMode : std::uint8_t { Guess, Ignore, Straight, Premultiplied };
// The field order the asset is read with. File uses the order captured at import.
enum class AssetFieldOrder : std::uint8_t { File, Progressive, UpperFirst, LowerFirst };
// What a sequence shows for a frame its manifest does not have. Skip plays the frames it has back
// to back, as if the missing ones had never been numbered (a compositor's "Force alphabetical
// order"), so the sequence is as long as the frames it holds.
enum class AssetMissingFrames : std::uint8_t { HoldPrevious, Nothing, Skip };
inline constexpr std::uint32_t kMaxAssetLoopCount = 1000;
// The longest still duration an import gives a still: 24 hours.
inline constexpr std::int64_t kMaxAssetStillDurationSeconds = 86400;
// #718: whether Bloom keeps a local copy of the asset's source media. Automatic follows the
// user's folder rules and preference and then the storage the source is on.
enum class AssetLocalCopies : std::uint8_t { Automatic, Always, Never };
struct AssetInterpretation {
    AssetColorSpace colorSpace = AssetColorSpace::Auto;
    // Empty means Auto. Non-empty values are exact ids from the qualified project OCIO config.
    std::string inputColorSpaceId;
    // The association the file declared at import (what Guess follows).
    AssetAlphaAssociation alphaAssociation = AssetAlphaAssociation::Straight;
    // A sequence's assumed rate (absent: one member per composition frame) or a video's conformed
    // rate (absent: the file's rate).
    std::optional<FrameRate> frameRate;
    // Absent: the source's own start timecode.
    std::optional<CompositionTimecode> startTimecode;
    AssetAlphaMode alphaMode = AssetAlphaMode::Guess;
    bool invertAlpha = false;
    // The color a Premultiplied alpha was matted against, in the file's own encoding, [0, 1].
    std::array<double, 3> matteColor{0.0, 0.0, 0.0};
    AssetFieldOrder fieldOrder = AssetFieldOrder::File;
    bool deinterlace = false;
    // Absent: the file's pixel aspect ratio.
    std::optional<core::PixelAspectRatio> pixelAspect;
    // The footage plays this many times end to end; its natural length is multiplied.
    std::uint32_t loopCount = 1;
    AssetMissingFrames missingFrames = AssetMissingFrames::HoldPrevious;
    // A still's length, from the Import preference "Still duration" (absent: the composition's
    // length, how Bloom has always placed a still). A layer made from the still ends there.
    std::optional<core::RationalTime> stillDuration;
    // #718: this asset's override of "Keep local copies of source media".
    AssetLocalCopies localCopies = AssetLocalCopies::Automatic;
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
// #473: a sequence member whose bytes import did not read. Its file's length and
// modification time (nanoseconds since the Unix epoch) when import listed it stand for the bytes
// until a read proves them; a file whose length or time differs is a changed frame.
struct AssetFileStamp {
    std::uint64_t size = 0;
    std::int64_t modifiedNanoseconds = 0;
    friend bool operator==(const AssetFileStamp&, const AssetFileStamp&) = default;
};
struct AssetSequenceMember {
    std::int64_t frame = 0;
    AssetLocator locator;
    // At least one of the two. A digest alone: the SHA-256 of the bytes import read (the member it
    // probed). A stamp alone (pending): the stamp import listed. Both (verified): the digest a full
    // hash proved for the bytes that carried that stamp.
    std::optional<core::Sha256Digest> contentDigest;
    std::optional<AssetFileStamp> stamp;
    friend bool operator==(const AssetSequenceMember&, const AssetSequenceMember&) = default;
};
// A member's cache and manifest identity: its content digest when it has no stamp, else the
// provisional SHA-256 over "BloomSequenceMemberStamp", NUL, the locator path, NUL, the relink hint,
// NUL, the decimal size, NUL and the decimal modification time. Path + size + time, so two frames
// with one size and time never share an identity. A stamped member keeps that identity when a
// verified digest is recorded beside it, so recording a digest never changes a
// cache key.
[[nodiscard]] core::Sha256Digest sequenceMemberIdentity(const AssetSequenceMember& member);
// The numbered frames an import was limited to: Reload Footage reads only these.
struct AssetSequenceRange {
    std::int64_t first = 0;
    std::int64_t last = 0;
    friend bool operator==(const AssetSequenceRange&, const AssetSequenceRange&) = default;
};
struct AssetSequenceManifest {
    std::string pattern;
    std::uint32_t padding = 0;
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::vector<AssetSequenceMember> members;
    std::vector<std::int64_t> gaps;
    // Absent: every numbered file of the sequence.
    std::optional<AssetSequenceRange> range;
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
    // Coded field order (0 unknown, 1 progressive, 2 top first, 3 bottom first,
    // 4 top coded/bottom displayed first, 5 the reverse) and timecode type (0 none, 1 non-drop,
    // 2 drop-frame), captured at import and shown in Assets and Properties.
    std::uint32_t fieldOrder = 0, timecodeType = 0;
    std::vector<std::string> channelLayout;
    // The coded pixel aspect ratio the decoder already resamples to square pixels
    // (width above is the display width); square when the file declares none or predates 1.34.
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    friend bool operator==(const AssetVideoStream&, const AssetVideoStream&) = default;
};
// SMPTE timecode type from its text: drop-frame timecode separates frames with ';' (or '.'),
// non-drop with ':'; no timecode is type 0.
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
// Frames in one pass of timed footage (sequence first..last, or its members when it skips missing
// frames; video frames); 0 otherwise.
[[nodiscard]] std::uint64_t footageFrameCount(const AssetRecord& asset);
// The sequence member frame `offset` (0-based, within footageFrameCount) shows: the member itself,
// the one a gap holds, or none for a gap that shows nothing. `frame` is the numbered frame wanted.
struct FootageSequenceFrame final {
    const AssetSequenceMember* member = nullptr;
    std::int64_t frame = 0;
};
[[nodiscard]] FootageSequenceFrame footageSequenceFrame(const AssetRecord& asset,
                                                        std::uint64_t offset);
// The natural length of timed footage including Loop, at `compositionRate` for a sequence that
// matches the composition, or a still's duration. Nullopt for a still without one, audio and
// anything without a length.
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
// A frame rate as typed: "24", an exact decimal such as "12.5", "n/d", or the NTSC decimals
// 23.976, 29.97, 47.952, 59.94, 119.88 and 14.985, which name the exact n/1001 rates. Nullopt when
// unreadable (the bounds are isValidFootageFrameRate's).
[[nodiscard]] std::optional<FrameRate> parseFootageFrameRate(std::string_view text);
// SMPTE "HH:MM:SS:FF" (';' or '.' before the frames for drop-frame, honoured at 30000/1001 and
// 60000/1001) at `rate`, as a start time.
[[nodiscard]] std::optional<CompositionTimecode> parseFootageTimecode(std::string_view text,
                                                                      FrameRate rate);
// Lexical only: import names an asset by its portable UTF-8 file stem.
[[nodiscard]] std::string defaultAssetName(const AssetRecord& asset);
} // namespace bloom::document
