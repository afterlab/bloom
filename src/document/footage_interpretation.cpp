// Interpret Footage (#381, document 1.34): validation and resolution of an asset's interpretation.
#include <bloom/document/asset.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace bloom::document {
namespace {
[[nodiscard]] bool timed(const AssetKind kind) noexcept {
    return kind == AssetKind::Sequence || kind == AssetKind::Video;
}
[[nodiscard]] bool visual(const AssetKind kind) noexcept {
    return kind == AssetKind::Image || timed(kind);
}
[[nodiscard]] const AssetVideoStream* pictureStream(const AssetRecord& asset) {
    const auto found = std::ranges::find(asset.videoStreams, 1U, &AssetVideoStream::kind);
    return found == asset.videoStreams.end() ? nullptr : &*found;
}
// SMPTE "HH:MM:SS:FF" (or ';'/'.' before the frames for drop-frame) at `rate`, as a time.
[[nodiscard]] std::optional<CompositionTimecode> parseTimecode(const std::string_view text,
                                                               const FrameRate rate) {
    std::array<std::uint32_t, 4> fields{};
    std::size_t position = 0;
    bool drop = false;
    for (std::size_t field = 0; field < fields.size(); ++field) {
        if (field > 0) {
            if (position >= text.size())
                return std::nullopt;
            const auto separator = text[position++];
            if (separator == ';' || separator == '.') {
                if (field != 3)
                    return std::nullopt;
                drop = true;
            } else if (separator != ':') {
                return std::nullopt;
            }
        }
        const auto* begin = text.data() + position;
        const auto [end, error] = std::from_chars(begin, text.data() + text.size(), fields[field]);
        if (error != std::errc{} || end == begin)
            return std::nullopt;
        position = static_cast<std::size_t>(end - text.data());
    }
    if (position != text.size())
        return std::nullopt;
    const auto nominal = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(rate.numerator()) / rate.denominator()));
    if (nominal == 0 || fields[1] > 59 || fields[2] > 59 || fields[3] >= nominal)
        return std::nullopt;
    const std::uint64_t minutes = std::uint64_t{fields[0]} * 60 + fields[1];
    std::uint64_t frames = (minutes * 60 + fields[2]) * nominal + fields[3];
    drop = drop && hasDropFrameTimecode(rate);
    if (drop) {
        // SMPTE drop-frame: two frame numbers (four at 60000/1001) are skipped each minute except
        // every tenth.
        const std::uint64_t dropped = nominal / 15;
        frames -= dropped * (minutes - minutes / 10);
    }
    const auto time = core::RationalTime::create(
        static_cast<std::int64_t>(frames) * rate.denominator(), rate.numerator());
    if (!time)
        return std::nullopt;
    return CompositionTimecode{*time, drop};
}
} // namespace

bool isValidFootageFrameRate(const FrameRate rate) noexcept {
    // 1/100 to 1000 frames per second, the composition settings' practical bounds.
    return std::uint64_t{rate.numerator()} * 100 >= rate.denominator() &&
           rate.numerator() <= std::uint64_t{rate.denominator()} * 1000;
}

bool isValidFootagePixelAspect(const core::PixelAspectRatio ratio) noexcept {
    return std::uint64_t{ratio.numerator()} * 100 >= ratio.denominator() &&
           ratio.numerator() <= std::uint64_t{ratio.denominator()} * 100;
}

std::string_view footageInterpretationIssue(const AssetInterpretation& value) {
    if (value.alphaMode > AssetAlphaMode::Premultiplied)
        return "Invalid alpha interpretation";
    if (value.fieldOrder > AssetFieldOrder::LowerFirst)
        return "Invalid field order";
    if (value.missingFrames > AssetMissingFrames::Nothing)
        return "Invalid missing-frame interpretation";
    if (value.frameRate && !isValidFootageFrameRate(*value.frameRate))
        return "The frame rate must be between 0.01 and 1000 frames per second";
    if (value.pixelAspect && !isValidFootagePixelAspect(*value.pixelAspect))
        return "The pixel aspect ratio must be between 0.01 and 100";
    if (value.loopCount < 1 || value.loopCount > kMaxAssetLoopCount)
        return "Loop must be 1 to 1000 times";
    if (value.startTimecode && !value.startTimecode->isValid())
        return "The start timecode must not be negative";
    for (const auto component : value.matteColor)
        if (!std::isfinite(component) || component < 0.0 || component > 1.0)
            return "The matte color must be within 0 to 1";
    return {};
}

std::string_view footageInterpretationKindIssue(const AssetKind kind,
                                                const AssetInterpretation& value) {
    const AssetInterpretation neutral{};
    if (!timed(kind) &&
        (value.frameRate || value.startTimecode || value.loopCount != neutral.loopCount))
        return "Only a video or an image sequence has a frame rate, a start timecode or a loop";
    if (kind != AssetKind::Sequence && value.missingFrames != neutral.missingFrames)
        return "Only an image sequence has missing frames";
    if (!timed(kind) && (value.fieldOrder != neutral.fieldOrder || value.deinterlace))
        return "Only a video or an image sequence has fields";
    if (!visual(kind) && (value.alphaMode != neutral.alphaMode || value.invertAlpha ||
                          value.matteColor != neutral.matteColor || value.pixelAspect))
        return "Only an image, a sequence or a video has alpha and a pixel aspect ratio";
    return {};
}

std::optional<FrameRate> footageFileFrameRate(const AssetRecord& asset) {
    const auto* stream = pictureStream(asset);
    if (stream == nullptr || stream->framePeriod.numerator() <= 0 ||
        stream->framePeriod.numerator() > std::numeric_limits<std::uint32_t>::max() ||
        stream->framePeriod.denominator() > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return FrameRate::create(static_cast<std::uint32_t>(stream->framePeriod.denominator()),
                             static_cast<std::uint32_t>(stream->framePeriod.numerator()));
}

std::optional<FrameRate> footageFrameRate(const AssetRecord& asset) {
    if (asset.kind == AssetKind::Video)
        return asset.interpretation.frameRate ? asset.interpretation.frameRate
                                              : footageFileFrameRate(asset);
    if (asset.kind == AssetKind::Sequence)
        return asset.interpretation.frameRate;
    return std::nullopt;
}

std::uint64_t footageFrameCount(const AssetRecord& asset) {
    if (asset.kind == AssetKind::Video)
        return asset.frames;
    if (asset.kind == AssetKind::Sequence && asset.manifest.last >= asset.manifest.first)
        return static_cast<std::uint64_t>(asset.manifest.last) -
               static_cast<std::uint64_t>(asset.manifest.first) + 1;
    return 0;
}

std::optional<core::RationalTime> footageDuration(const AssetRecord& asset,
                                                  const FrameRate compositionRate) {
    const auto frames = footageFrameCount(asset);
    if (frames == 0)
        return std::nullopt;
    // An unconformed video keeps its probed duration, which a final short frame may make
    // differ from frames / rate.
    if (asset.kind == AssetKind::Video && !asset.interpretation.frameRate &&
        asset.interpretation.loopCount == 1 && asset.duration > core::RationalTime{})
        return asset.duration;
    const auto rate = footageFrameRate(asset).value_or(compositionRate);
    const auto loops = std::max<std::uint32_t>(1, asset.interpretation.loopCount);
    if (frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / loops /
                     rate.denominator())
        return std::nullopt;
    return core::RationalTime::create(
        static_cast<std::int64_t>(frames * loops * rate.denominator()), rate.numerator());
}

std::uint32_t footageFieldOrder(const AssetRecord& asset) {
    switch (asset.interpretation.fieldOrder) {
    case AssetFieldOrder::Progressive:
        return 1;
    case AssetFieldOrder::UpperFirst:
        return 2;
    case AssetFieldOrder::LowerFirst:
        return 3;
    case AssetFieldOrder::File:
        break;
    }
    const auto* stream = pictureStream(asset);
    return stream == nullptr ? 0 : stream->fieldOrder;
}

core::PixelAspectRatio footageFilePixelAspect(const AssetRecord& asset) {
    const auto* stream = pictureStream(asset);
    return stream == nullptr ? core::PixelAspectRatio::square() : stream->pixelAspect;
}

core::PixelAspectRatio footagePixelAspect(const AssetRecord& asset) {
    return asset.interpretation.pixelAspect.value_or(footageFilePixelAspect(asset));
}

std::optional<CompositionTimecode> footageStartTimecode(const AssetRecord& asset) {
    if (asset.interpretation.startTimecode)
        return asset.interpretation.startTimecode;
    const auto* stream = pictureStream(asset);
    const auto rate = footageFileFrameRate(asset);
    if (stream == nullptr || stream->timecode.empty() || !rate)
        return std::nullopt;
    return parseTimecode(stream->timecode, *rate);
}

} // namespace bloom::document
