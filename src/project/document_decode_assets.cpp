#include "document_decode_internal.hpp"
#include <array>
#include <bloom/document/asset.hpp>
#include <bloom/document/persisted_text.hpp>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace bloom::project::detail {
namespace {
bool text(const JsonValue& node, DecodeState& state, const std::string& path, std::string& out) {
    std::string_view value;
    if (!decodeStringMember(node, state, path, value))
        return false;
    out = value;
    return true;
}
bool number(const JsonValue& node, DecodeState& state, const std::string& path,
            std::uint32_t& out) {
    const auto token = node.asNumberToken();
    if (!token) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsed = parseCanonicalAllocatorHighWater(*token);
    if (!parsed || *parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out = static_cast<std::uint32_t>(*parsed.value());
    return true;
}
bool integer(const JsonValue& node, DecodeState& state, const std::string& path,
             std::int64_t& out) {
    const auto token = node.asString();
    if (!token) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsed = parseCanonicalInt64(*token);
    if (!parsed) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out = *parsed.value();
    return true;
}
bool digest(const JsonValue& node, DecodeState& state, const std::string& path,
            core::Sha256Digest& out) {
    const auto token = node.asString();
    const auto value = token ? core::Sha256Digest::fromLowercaseHex(*token) : std::nullopt;
    if (!value) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out = *value;
    return true;
}
bool locator(const JsonValue& node, DecodeState& state, const std::string& path,
             document::AssetLocator& out) {
    constexpr std::array<std::string_view, 4> keys{"kind", "portability", "path", "relinkHint"};
    std::vector<const JsonValue*> members;
    return matchOrderedMembers(node, keys, false, state, path, members) &&
           text(*members[0], state, path, out.kind) &&
           text(*members[1], state, path, out.portability) &&
           text(*members[2], state, path, out.path) &&
           text(*members[3], state, path, out.relinkHint);
}
bool manifest(const JsonValue& node, DecodeState& state, const std::string& path,
              document::AssetSequenceManifest& out) {
    constexpr std::array<std::string_view, 6> keys{"pattern", "padding", "first",
                                                   "last",    "members", "gaps"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, keys, false, state, path, members) ||
        !text(*members[0], state, path, out.pattern) ||
        !number(*members[1], state, path, out.padding) ||
        !integer(*members[2], state, path, out.first) ||
        !integer(*members[3], state, path, out.last))
        return false;
    if (members[4]->kind() != JsonValueKind::Array || members[5]->kind() != JsonValueKind::Array ||
        members[4]->arrayElements().size() > 100000 ||
        members[5]->arrayElements().size() > 100000) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    constexpr std::array<std::string_view, 3> memberKeys{"frame", "locator", "contentDigest"};
    for (const auto& value : members[4]->arrayElements()) {
        std::vector<const JsonValue*> fields;
        document::AssetSequenceMember member;
        if (!matchOrderedMembers(value, memberKeys, false, state, path, fields) ||
            !integer(*fields[0], state, path, member.frame) ||
            !locator(*fields[1], state, path, member.locator) ||
            !digest(*fields[2], state, path, member.contentDigest))
            return false;
        out.members.push_back(std::move(member));
    }
    for (const auto& value : members[5]->arrayElements()) {
        std::int64_t frame = 0;
        if (!integer(value, state, path, frame))
            return false;
        out.gaps.push_back(frame);
    }
    return true;
}
bool audio(const JsonValue& node, DecodeState& state, const std::string& path,
           document::AssetRecord& out) {
    constexpr std::array<std::string_view, 4> keys{"rate", "channels", "frames", "duration"};
    std::vector<const JsonValue*> fields;
    if (!matchOrderedMembers(node, keys, false, state, path, fields) ||
        !number(*fields[0], state, path, out.rate) ||
        !number(*fields[1], state, path, out.channels))
        return false;
    const auto frameText = fields[2]->asString();
    if (!frameText) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsedFrames = parseCanonicalObjectId(*frameText);
    if (!parsedFrames || *parsedFrames.value() == 0) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out.frames = *parsedFrames.value();
    return decodeRationalTimeValue(*fields[3], state, path, out.duration);
}
bool font(const JsonValue& node, DecodeState& state, const std::string& path,
          document::AssetRecord& out) {
    constexpr std::array<std::string_view, 3> keys{"family", "style", "faceIndex"};
    std::vector<const JsonValue*> fields;
    if (!matchOrderedMembers(node, keys, false, state, path, fields) ||
        !text(*fields[0], state, path, out.fontFamily) ||
        !text(*fields[1], state, path, out.fontStyle) ||
        !number(*fields[2], state, path, out.fontIndex))
        return false;
    return true;
}
bool float64(const JsonValue& node, DecodeState& state, const std::string& path, double& out) {
    const auto token = node.kind() == JsonValueKind::Number ? node.asNumberToken() : std::nullopt;
    if (!token) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsed = parseKnownFloat64(*token);
    if (!parsed) {
        state.fail(DocumentDecodeError::InvalidFloat64, path);
        return false;
    }
    out = *parsed.value();
    return true;
}
bool trueFlag(const JsonValue& node, DecodeState& state, const std::string& path, bool& out) {
    if (node.asBoolean() != std::optional<bool>{true}) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out = true;
    return true;
}
// Document 1.34 (#381): the optional Interpret Footage members after alphaAssociation, each present
// only when it differs from its default and always in this order.
constexpr std::array<std::string_view, 10> kFootageInterpretationKeys{
    "frameRate",  "startTimecode", "alphaMode",   "invertAlpha", "matteColor",
    "fieldOrder", "deinterlace",   "pixelAspect", "loopCount",   "missingFrames"};
bool footageInterpretation(const JsonValue& node, const std::span<const JsonValue* const> values,
                           const std::span<const std::string_view> keys, DecodeState& state,
                           const std::string& path, document::AssetInterpretation& out) {
    using namespace document;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto key = keys[index];
        const auto& value = *values[index];
        const auto choice = [&](std::initializer_list<std::string_view> tokens) -> int {
            std::string_view text;
            if (!decodeStringMember(value, state, path, text))
                return -1;
            int position = 1;
            for (const auto token : tokens) {
                if (token == text)
                    return position;
                ++position;
            }
            state.fail(DocumentDecodeError::DomainViolation, path);
            return -1;
        };
        if (key == "frameRate") {
            FrameRate rate = FrameRate::framesPerSecond24();
            if (!decodeFrameRateValue(value, state, path, rate))
                return false;
            out.frameRate = rate;
        } else if (key == "startTimecode") {
            constexpr std::array<std::string_view, 2> parts{"start", "dropFrame"};
            std::vector<const JsonValue*> fields;
            CompositionTimecode timecode;
            if (!matchOrderedMembers(value, parts, true, state, path, fields) ||
                !decodeRationalTimeValue(*fields[0], state, path, timecode.start) ||
                !fields[1]->asBoolean()) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
            timecode.dropFrame = *fields[1]->asBoolean();
            out.startTimecode = timecode;
        } else if (key == "alphaMode") {
            const auto mode = choice({"ignore", "straight", "premultiplied"});
            if (mode < 0)
                return false;
            out.alphaMode = static_cast<AssetAlphaMode>(mode);
        } else if (key == "invertAlpha") {
            if (!trueFlag(value, state, path, out.invertAlpha))
                return false;
        } else if (key == "matteColor") {
            if (value.kind() != JsonValueKind::Array || value.arrayElements().size() != 3) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
            for (std::size_t channel = 0; channel < 3; ++channel)
                if (!float64(value.arrayElements()[channel], state, path, out.matteColor[channel]))
                    return false;
            if (out.matteColor == std::array<double, 3>{}) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
        } else if (key == "fieldOrder") {
            const auto order = choice({"progressive", "upperFirst", "lowerFirst"});
            if (order < 0)
                return false;
            out.fieldOrder = static_cast<AssetFieldOrder>(order);
        } else if (key == "deinterlace") {
            if (!trueFlag(value, state, path, out.deinterlace))
                return false;
        } else if (key == "pixelAspect") {
            FrameRate ratio = FrameRate::framesPerSecond24();
            if (!decodeFrameRateValue(value, state, path, ratio))
                return false;
            out.pixelAspect =
                core::PixelAspectRatio::create(ratio.numerator(), ratio.denominator());
        } else if (key == "loopCount") {
            if (!number(value, state, path, out.loopCount) || out.loopCount < 2) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
        } else if (key == "missingFrames") {
            if (choice({"nothing"}) < 0)
                return false;
            out.missingFrames = AssetMissingFrames::Nothing;
        }
    }
    (void)node;
    return true;
}
bool video(const JsonValue& node, DecodeState& state, const std::string& path,
           document::AssetRecord& out) {
    constexpr std::array<std::string_view, 3> keys{"frames", "duration", "streams"};
    std::vector<const JsonValue*> fields;
    if (!matchOrderedMembers(node, keys, false, state, path, fields) ||
        node.objectMembers().size() != keys.size())
        return false;
    const auto frameText = fields[0]->asString();
    if (!frameText)
        return false;
    const auto frameCount = parseCanonicalObjectId(*frameText);
    if (!frameCount)
        return false;
    out.frames = *frameCount.value();
    if (!decodeRationalTimeValue(*fields[1], state, path, out.duration) ||
        fields[2]->kind() != JsonValueKind::Array || fields[2]->arrayElements().empty() ||
        fields[2]->arrayElements().size() > 256)
        return false;
    constexpr std::array<std::string_view, 19> streamKeys{
        "id",       "kind",       "codec",        "profile",      "pixelFormat",
        "timecode", "timebase",   "framePeriod",  "duration",     "width",
        "height",   "sampleRate", "primaries",    "transfer",     "matrix",
        "range",    "fieldOrder", "timecodeType", "channelLayout"};
    for (const auto& value : fields[2]->arrayElements()) {
        document::AssetVideoStream stream;
        std::vector<const JsonValue*> members;
        // Document 1.34 appends an optional coded pixel aspect ratio.
        const auto* pixelAspect =
            state.documentMinor >= 34 ? value.findMember("pixelAspect") : nullptr;
        if (!matchOrderedMembers(value, streamKeys, false, state, path, members) ||
            value.objectMembers().size() != streamKeys.size() + (pixelAspect ? 1U : 0U) ||
            (pixelAspect != nullptr &&
             value.objectMembers().back().key() != std::string_view("pixelAspect")))
            return false;
        if (pixelAspect != nullptr) {
            document::FrameRate ratio = document::FrameRate::framesPerSecond24();
            if (!decodeFrameRateValue(*pixelAspect, state, path, ratio))
                return false;
            const auto created =
                core::PixelAspectRatio::create(ratio.numerator(), ratio.denominator());
            if (!created || *created == core::PixelAspectRatio::square())
                return false;
            stream.pixelAspect = *created;
        }
        if (!number(*members[0], state, path, stream.id))
            return false;
        if (!number(*members[1], state, path, stream.kind))
            return false;
        if (!text(*members[2], state, path, stream.codec))
            return false;
        if (!text(*members[3], state, path, stream.profile))
            return false;
        if (!text(*members[4], state, path, stream.pixelFormat))
            return false;
        if (!text(*members[5], state, path, stream.timecode))
            return false;
        if (!decodeRationalTimeValue(*members[6], state, path, stream.timebase))
            return false;
        if (!decodeRationalTimeValue(*members[7], state, path, stream.framePeriod))
            return false;
        if (!decodeRationalTimeValue(*members[8], state, path, stream.duration))
            return false;
        if (!number(*members[9], state, path, stream.width))
            return false;
        if (!number(*members[10], state, path, stream.height))
            return false;
        if (!number(*members[11], state, path, stream.sampleRate))
            return false;
        if (!integer(*members[12], state, path, stream.primaries))
            return false;
        if (!integer(*members[13], state, path, stream.transfer))
            return false;
        if (!integer(*members[14], state, path, stream.matrix))
            return false;
        if (!integer(*members[15], state, path, stream.range))
            return false;
        if (!number(*members[16], state, path, stream.fieldOrder) ||
            !number(*members[17], state, path, stream.timecodeType))
            return false;
        if (members[18]->kind() != JsonValueKind::Array || members[18]->arrayElements().size() > 64)
            return false;
        for (const auto& channel : members[18]->arrayElements()) {
            std::string name;
            if (!text(channel, state, path, name))
                return false;
            stream.channelLayout.push_back(std::move(name));
        }
        if (stream.kind == 2 && out.channels == 0) {
            out.rate = stream.sampleRate;
            out.channels = static_cast<std::uint32_t>(stream.channelLayout.size());
        }
        out.videoStreams.push_back(std::move(stream));
    }
    return true;
}
} // namespace
bool decodeAssets(const JsonValue& node, DecodeState& state, const std::string& path,
                  std::vector<document::AssetRecord>& out) {
    if (node.kind() != JsonValueKind::Array || node.arrayElements().size() > 100000) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    constexpr std::array<std::string_view, 8> keys{
        "id", "kind", "locator", "contentDigest", "interpretation", "width", "height", "manifest"};
    std::uint64_t previous = 0;
    for (const auto& value : node.arrayElements()) {
        std::vector<const JsonValue*> fields;
        document::AssetRecord asset;
        std::string kind;
        if (!matchOrderedMembers(value, keys, false, state, path, fields) ||
            !decodeObjectId(*fields[0], state, path, asset.id) ||
            !text(*fields[1], state, path, kind) ||
            !locator(*fields[2], state, path, asset.locator) ||
            !digest(*fields[3], state, path, asset.contentDigest))
            return false;
        if (asset.id.value() <= previous ||
            (kind != "image" && kind != "sequence" && kind != "audio" && kind != "font" &&
             kind != "video" && kind != "lut")) {
            state.fail(DocumentDecodeError::DomainViolation, path);
            return false;
        }
        // Font assets arrive with document 1.15; 1.14 carries path values but no font kind, so an
        // earlier minor naming one is a domain violation rather than a tolerated unknown. Mirrors
        // the `path` constant's own minor gate in document_decode_composition.cpp.
        if ((kind == "font" && state.documentMinor < 15) ||
            (kind == "video" && state.documentMinor < 17) ||
            (kind == "lut" && state.documentMinor < 21)) {
            state.fail(DocumentDecodeError::DomainViolation, path);
            return false;
        }
        previous = asset.id.value();
        asset.kind = kind == "image"      ? document::AssetKind::Image
                     : kind == "sequence" ? document::AssetKind::Sequence
                     : kind == "audio"    ? document::AssetKind::Audio
                     : kind == "lut"      ? document::AssetKind::Lut
                     : kind == "video"    ? document::AssetKind::Video
                                          : document::AssetKind::Font;
        std::vector<std::string_view> interpretationKeys{"colorSpace", "inputColorSpaceId",
                                                         "alphaAssociation"};
        if (state.documentMinor >= 34 && fields[4]->kind() == JsonValueKind::Object)
            for (const auto key : kFootageInterpretationKeys)
                if (fields[4]->findMember(key) != nullptr)
                    interpretationKeys.push_back(key);
        constexpr std::array<std::string_view, 2> legacyInterpretationKeys{"colorSpace",
                                                                           "alphaAssociation"};
        std::vector<const JsonValue*> interpretation;
        std::uint32_t space = 0, alpha = 0;
        std::string inputColorSpaceId;
        const bool currentInterpretation = state.documentMinor >= 20;
        if ((currentInterpretation ? !matchOrderedMembers(*fields[4], interpretationKeys, false,
                                                          state, path, interpretation)
                                   : !matchOrderedMembers(*fields[4], legacyInterpretationKeys,
                                                          false, state, path, interpretation)) ||
            !number(*interpretation[0], state, path, space) ||
            (currentInterpretation && !text(*interpretation[1], state, path, inputColorSpaceId)) ||
            !number(*(currentInterpretation ? interpretation[2] : interpretation[1]), state, path,
                    alpha) ||
            space > 3 || alpha > 1 || !number(*fields[5], state, path, asset.width) ||
            !number(*fields[6], state, path, asset.height) ||
            !manifest(*fields[7], state, path, asset.manifest)) {
            state.fail(DocumentDecodeError::DomainViolation, path);
            return false;
        }
        asset.interpretation.colorSpace = static_cast<document::AssetColorSpace>(space);
        asset.interpretation.inputColorSpaceId = std::move(inputColorSpaceId);
        asset.interpretation.alphaAssociation = static_cast<document::AssetAlphaAssociation>(alpha);
        if (currentInterpretation && interpretationKeys.size() > 3 &&
            !footageInterpretation(*fields[4], std::span(interpretation).subspan(3),
                                   std::span<const std::string_view>(interpretationKeys).subspan(3),
                                   state, path, asset.interpretation)) {
            state.fail(DocumentDecodeError::DomainViolation, path);
            return false;
        }
        if (asset.kind == document::AssetKind::Font) {
            const auto* fontNode = value.findMember("font");
            if (fontNode == nullptr || !font(*fontNode, state, path, asset)) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
        } else if (asset.kind == document::AssetKind::Audio) {
            const auto* audioNode = value.findMember("audio");
            if (audioNode == nullptr || !audio(*audioNode, state, path, asset)) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
        }
        if (asset.kind == document::AssetKind::Video) {
            const auto* descriptor = value.findMember("video");
            if (!descriptor || !video(*descriptor, state, path, asset)) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
        }
        if (state.documentMinor >= 16) {
            auto allKeys = std::vector<std::string_view>(keys.begin(), keys.end());
            if (asset.kind == document::AssetKind::Font)
                allKeys.push_back("font");
            if (asset.kind == document::AssetKind::Audio)
                allKeys.push_back("audio");
            if (asset.kind == document::AssetKind::Video)
                allKeys.push_back("video");
            allKeys.push_back("name");
            if (value.findMember("folder"))
                allKeys.push_back("folder");
            allKeys.push_back("tags");
            allKeys.push_back("order");
            if (!matchOrderedMembers(value, allKeys, false, state, path, fields) ||
                value.objectMembers().size() != allKeys.size()) {
                state.fail(DocumentDecodeError::UnknownMember, path);
                return false;
            }
            const auto* nameNode = value.findMember("name");
            if (!text(*nameNode, state, path, asset.name) ||
                !document::isValidHumanFacingName(asset.name)) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
            if (const auto* folderNode = value.findMember("folder")) {
                document::AssetFolderId folderId;
                if (!decodeObjectId(*folderNode, state, path, folderId))
                    return false;
                asset.folder = folderId;
            }
            const auto* tags = value.findMember("tags");
            if (tags->kind() != JsonValueKind::Array || tags->arrayElements().size() > 64) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
            for (const auto& tagNode : tags->arrayElements()) {
                std::string tag;
                if (!text(tagNode, state, path, tag))
                    return false;
                asset.tags.push_back(std::move(tag));
            }
            const auto orderText = value.findMember("order")->asString();
            const auto order = parseCanonicalAllocatorHighWater(orderText.value_or(""));
            if (!order) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
            asset.order = *order.value();
        } else {
            auto legacyKeys = std::vector<std::string_view>(keys.begin(), keys.end());
            if (asset.kind == document::AssetKind::Font)
                legacyKeys.push_back("font");
            if (asset.kind == document::AssetKind::Audio)
                legacyKeys.push_back("audio");
            if (!matchOrderedMembers(value, legacyKeys, false, state, path, fields) ||
                value.objectMembers().size() != legacyKeys.size()) {
                state.fail(DocumentDecodeError::UnknownMember, path);
                return false;
            }
            asset.name = document::defaultAssetName(asset);
            asset.order = out.size();
        }
        if (!asset.validate().ok()) {
            state.fail(DocumentDecodeError::DomainViolation, path);
            return false;
        }
        out.push_back(std::move(asset));
    }
    return true;
}
bool decodeAssetFolders(const JsonValue& node, DecodeState& state, const std::string& path,
                        std::vector<document::AssetFolder>& out) {
    if (node.kind() != JsonValueKind::Array || node.arrayElements().size() > 100000) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    std::uint64_t previous = 0;
    for (const auto& value : node.arrayElements()) {
        std::vector<std::string_view> keys{"id", "name"};
        if (value.findMember("parent"))
            keys.push_back("parent");
        std::vector<const JsonValue*> fields;
        document::AssetFolder folder;
        if (!matchOrderedMembers(value, keys, false, state, path, fields) ||
            !decodeObjectId(*fields[0], state, path, folder.id) ||
            !text(*fields[1], state, path, folder.name))
            return false;
        if (value.objectMembers().size() != keys.size() || folder.id.value() <= previous ||
            !document::isValidHumanFacingName(folder.name)) {
            state.fail(DocumentDecodeError::DomainViolation, path);
            return false;
        }
        previous = folder.id.value();
        if (fields.size() == 3) {
            document::AssetFolderId parent;
            if (!decodeObjectId(*fields[2], state, path, parent))
                return false;
            folder.parent = parent;
        }
        out.push_back(std::move(folder));
    }
    return true;
}
} // namespace bloom::project::detail
