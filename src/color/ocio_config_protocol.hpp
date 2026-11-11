#pragma once

// The config request group of the bloom-color-worker protocol (docs/architecture/
// color-management.md, "Supervised OCIO Execution"). A project-relative archive, an external
// archive or an external loose config is parsed, turned into processors and applied only in the
// helper; the host holds opaque tokens. The fixed packet of lut_worker_protocol.hpp carries each
// command, and everything of variable size travels in sealed shared files as the byte records
// below: fixed-width little-endian fields, length-prefixed text, no pointers and no OCIO objects.
//
// This header is platform-neutral. How the helper is confined and how shared files are sealed
// stays with each platform in lut_worker.cpp and lut_worker_protocol.hpp.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_loose_content_revision.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::color::detail {

// Commands 0 to 3 are the handshake and the LUT group. One helper serves one request group: a
// helper that has begun a config never serves a LUT command, and the reverse.
inline constexpr std::uint64_t kConfigCommandBegin = 16;
inline constexpr std::uint64_t kConfigCommandOpen = 17;
inline constexpr std::uint64_t kConfigCommandBuildProcessor = 18;
inline constexpr std::uint64_t kConfigCommandApplySlab = 19;
inline constexpr std::uint64_t kConfigCommandExtractGpuProgram = 20;
inline constexpr std::uint64_t kConfigCommandLast = kConfigCommandExtractGpuProgram;

inline constexpr std::uint32_t kConfigRecordVersion = 1;
// One config archive is bounded like a loose table's aggregate.
inline constexpr std::uint64_t kConfigMaximumArchiveBytes = kOcioLooseMaximumAggregateBytes;
inline constexpr std::uint64_t kConfigMaximumParameterBytes = std::uint64_t{4} * 1024U * 1024U;
inline constexpr std::uint64_t kConfigMaximumReplyBytes = std::uint64_t{16} * 1024U * 1024U;
inline constexpr std::uint32_t kConfigMaximumContextVariables = 256;
inline constexpr std::uint32_t kConfigMaximumProcessors = 256;
inline constexpr std::uint32_t kConfigMaximumNames = 65536;

// How the helper answered ConfigOpen. Anything other than Ready carries no description.
enum class ConfigOpenOutcome : std::uint32_t {
    Ready = 0,
    Invalid = 1,
    // The config parsed, but a file it references is not in the supplied resources.
    MissingResource = 2,
};

enum class ConfigProcessorKind : std::uint32_t {
    // a = display, b = view: the working space to that display and view.
    DisplayForward = 1,
    // The same pair inverted, built without lossy optimization (authoring colours).
    DisplayInverse = 2,
    // a = source space, b = destination space.
    ColorSpace = 3,
    // The resolved display and view inverted to the working space, as CpuInputProcessor uses.
    InputInverseDisplay = 4,
};

enum class ConfigProcessorError : std::uint32_t {
    None = 0,
    UnknownKind = 1,
    MissingSource = 2,
    SourceIsData = 3,
    MissingDestination = 4,
    DestinationIsData = 5,
    BuildFailed = 6,
    CpuUnavailable = 7,
    TooManyProcessors = 8,
    MemoryLimit = 9,
};

// How a slab is applied, so the helper runs the exact call the in-process path of the same
// processor runs.
enum class ConfigApplyMode : std::uint64_t {
    // CPUProcessor::applyRGB on each pixel.
    PerPixel = 0,
    // CPUProcessor::apply on one packed RGB description with a four-float stride.
    Packed = 1,
};

struct ConfigContextVariable final {
    std::string name;
    std::string value;
    friend bool operator==(const ConfigContextVariable&, const ConfigContextVariable&) = default;
};

struct ConfigParameters final {
    OcioConfigLocatorKind kind = OcioConfigLocatorKind::ExternalLooseConfig;
    std::string workingColorSpaceId;
    std::vector<ConfigContextVariable> context;
};

struct ConfigProcessorRecipe final {
    ConfigProcessorKind kind = ConfigProcessorKind::ColorSpace;
    std::string a;
    std::string b;
};

struct ConfigGpuProgramRecipe final {
    ConfigProcessorRecipe processor;
    render::OcioGpuProgramStage stage = render::OcioGpuProgramStage::ProcessEffect;
    std::string semanticsId;
    render::OcioGpuProgramLimits limits;
};

// What the helper learned from a config it accepted: exactly the fields of
// ResolvedBloomNeutralConfig, plus the facts execution provenance reports.
struct ConfigDescription final {
    std::string processColorSpaceId;
    std::string outputColorSpaceId;
    std::string displayName;
    std::string viewName;
    std::string configName;
    std::vector<OcioColorSpaceInfo> colorSpaces;
    std::vector<DisplayViewEntry> displays;
    std::string sRgbTextureColorSpaceId;
    std::string rec709VideoColorSpaceId;
    std::string rec2100PqColorSpaceId;
    std::string rec2100HlgColorSpaceId;
    std::string ocioVersion;
    std::string compilerId;
    std::string compilerVersion;
    std::string targetTriple;
};

struct ConfigOpenReply final {
    ConfigOpenOutcome outcome = ConfigOpenOutcome::Invalid;
    OcioBuiltInInvalidReason invalidReason = OcioBuiltInInvalidReason::None;
    // Every resource key the config can read, config.ocio included, strictly ordered. Empty for
    // an archive, whose revision covers the whole file.
    std::vector<std::string> consumedKeys;
    // File references that resolve to no supplied resource (MissingResource).
    std::vector<std::string> missing;
    ConfigDescription description;
};

struct ConfigProcessorReply final {
    ConfigProcessorError error = ConfigProcessorError::None;
    std::uint64_t token = 0;
    bool identity = false;
    std::string cacheId;
};

class ConfigWriter final {
  public:
    void u32(const std::uint32_t value) { raw(&value, sizeof(value)); }
    void u64(const std::uint64_t value) { raw(&value, sizeof(value)); }
    void text(const std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        raw(value.data(), value.size());
    }
    void bytes(const std::span<const std::byte> value) {
        u64(value.size());
        raw(value.data(), value.size());
    }
    [[nodiscard]] std::vector<std::byte> take() && { return std::move(bytes_); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

  private:
    void raw(const void* const data, const std::size_t size) {
        const auto* const first = static_cast<const std::byte*>(data);
        bytes_.insert(bytes_.end(), first, first + size);
    }
    std::vector<std::byte> bytes_;
};

// Every read is bounds-checked; one failed read fails the reader for good, and the caller checks
// ok() once at the end. Text and name counts are bounded by the caller's own limits.
class ConfigReader final {
  public:
    explicit ConfigReader(const std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] std::uint32_t u32() {
        std::uint32_t value = 0;
        raw(&value, sizeof(value));
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        std::uint64_t value = 0;
        raw(&value, sizeof(value));
        return value;
    }
    [[nodiscard]] std::string text(const std::size_t limit = 65536) {
        const auto size = u32();
        if (!ok_ || size > limit || size > bytes_.size() - at_) {
            ok_ = false;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(bytes_.data() + at_), size);
        at_ += size;
        return value;
    }
    // A view of the next length-prefixed byte run, valid while the underlying bytes are.
    [[nodiscard]] std::span<const std::byte> bytes(const std::uint64_t limit) {
        const auto size = u64();
        if (!ok_ || size > limit || size > bytes_.size() - at_) {
            ok_ = false;
            return {};
        }
        const auto value = bytes_.subspan(at_, static_cast<std::size_t>(size));
        at_ += static_cast<std::size_t>(size);
        return value;
    }
    [[nodiscard]] std::size_t offset() const noexcept { return at_; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool finished() const noexcept { return ok_ && at_ == bytes_.size(); }
    void fail() noexcept { ok_ = false; }

  private:
    void raw(void* const data, const std::size_t size) {
        if (!ok_ || size > bytes_.size() - at_) {
            ok_ = false;
            return;
        }
        std::memcpy(data, bytes_.data() + at_, size);
        at_ += size;
    }
    std::span<const std::byte> bytes_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

[[nodiscard]] inline std::vector<std::byte> encodeConfigParameters(const ConfigParameters& value) {
    ConfigWriter writer;
    writer.u32(kConfigRecordVersion);
    writer.u32(static_cast<std::uint32_t>(value.kind));
    writer.text(value.workingColorSpaceId);
    writer.u32(static_cast<std::uint32_t>(value.context.size()));
    for (const auto& variable : value.context) {
        writer.text(variable.name);
        writer.text(variable.value);
    }
    return std::move(writer).take();
}

[[nodiscard]] inline std::optional<ConfigParameters>
decodeConfigParameters(const std::span<const std::byte> bytes) {
    ConfigReader reader(bytes);
    ConfigParameters value;
    if (reader.u32() != kConfigRecordVersion)
        return std::nullopt;
    const auto kind = reader.u32();
    if (kind < static_cast<std::uint32_t>(OcioConfigLocatorKind::ProjectRelativeArchive) ||
        kind > static_cast<std::uint32_t>(OcioConfigLocatorKind::ExternalLooseConfig))
        return std::nullopt;
    value.kind = static_cast<OcioConfigLocatorKind>(kind);
    value.workingColorSpaceId = reader.text(4096);
    const auto count = reader.u32();
    if (!reader.ok() || count > kConfigMaximumContextVariables)
        return std::nullopt;
    for (std::uint32_t index = 0; index < count && reader.ok(); ++index) {
        ConfigContextVariable variable;
        variable.name = reader.text(4096);
        variable.value = reader.text(16384);
        value.context.push_back(std::move(variable));
    }
    if (!reader.finished())
        return std::nullopt;
    return value;
}

// The sealed resource table of a loose config: canonical root-relative keys and exact bytes, in
// strict key order. The helper reads it in place and never copies more than one resource.
struct ConfigResourceSlice final {
    std::string key;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

inline void appendConfigResource(ConfigWriter& writer, const std::string_view key,
                                 const std::span<const std::byte> payload) {
    writer.text(key);
    writer.bytes(payload);
}

// Reads only the table's index: each key and where its payload sits in `bytes`.
[[nodiscard]] inline std::optional<std::vector<ConfigResourceSlice>>
decodeConfigResourceIndex(const std::span<const std::byte> bytes) {
    ConfigReader reader(bytes);
    if (reader.u32() != kConfigRecordVersion)
        return std::nullopt;
    const auto count = reader.u32();
    if (!reader.ok() || count == 0 || count > kOcioLooseMaximumEntryCount)
        return std::nullopt;
    std::vector<ConfigResourceSlice> index;
    index.reserve(count);
    for (std::uint32_t entry = 0; entry < count && reader.ok(); ++entry) {
        ConfigResourceSlice slice;
        slice.key = reader.text(kOcioLooseMaximumKeyBytes);
        const auto payload = reader.bytes(kOcioLooseMaximumResourceBytes);
        slice.size = payload.size();
        slice.offset = reader.offset() - payload.size();
        if (!index.empty() && !(index.back().key < slice.key))
            return std::nullopt;
        index.push_back(std::move(slice));
    }
    if (!reader.finished())
        return std::nullopt;
    return index;
}

inline void encodeConfigRecipe(ConfigWriter& writer, const ConfigProcessorRecipe& value) {
    writer.u32(static_cast<std::uint32_t>(value.kind));
    writer.text(value.a);
    writer.text(value.b);
}

[[nodiscard]] inline ConfigProcessorRecipe decodeConfigRecipe(ConfigReader& reader) {
    ConfigProcessorRecipe value;
    value.kind = static_cast<ConfigProcessorKind>(reader.u32());
    value.a = reader.text(4096);
    value.b = reader.text(4096);
    return value;
}

[[nodiscard]] inline std::vector<std::byte>
encodeConfigProcessorRecipe(const ConfigProcessorRecipe& value) {
    ConfigWriter writer;
    writer.u32(kConfigRecordVersion);
    encodeConfigRecipe(writer, value);
    return std::move(writer).take();
}

[[nodiscard]] inline std::optional<ConfigProcessorRecipe>
decodeConfigProcessorRecipe(const std::span<const std::byte> bytes) {
    ConfigReader reader(bytes);
    if (reader.u32() != kConfigRecordVersion)
        return std::nullopt;
    auto value = decodeConfigRecipe(reader);
    if (!reader.finished())
        return std::nullopt;
    return value;
}

[[nodiscard]] inline std::vector<std::byte>
encodeConfigGpuProgramRecipe(const ConfigGpuProgramRecipe& value) {
    ConfigWriter writer;
    writer.u32(kConfigRecordVersion);
    encodeConfigRecipe(writer, value.processor);
    writer.u32(static_cast<std::uint32_t>(value.stage));
    writer.text(value.semanticsId);
    writer.u64(value.limits.maxShaderBytes);
    writer.u32(value.limits.maxTextures);
    writer.u32(value.limits.max3dEdge);
    writer.u64(value.limits.maxAggregateLutBytes);
    writer.u32(value.limits.maxUniforms);
    writer.u32(value.limits.maxDynamicPropertyCount);
    writer.u64(value.limits.maxUniformBufferBytes);
    writer.u64(value.limits.maxStorageBufferBytes);
    return std::move(writer).take();
}

[[nodiscard]] inline std::optional<ConfigGpuProgramRecipe>
decodeConfigGpuProgramRecipe(const std::span<const std::byte> bytes) {
    ConfigReader reader(bytes);
    if (reader.u32() != kConfigRecordVersion)
        return std::nullopt;
    ConfigGpuProgramRecipe value;
    value.processor = decodeConfigRecipe(reader);
    const auto stage = reader.u32();
    if (stage > static_cast<std::uint32_t>(render::OcioGpuProgramStage::ProcessNeighborhood))
        return std::nullopt;
    value.stage = static_cast<render::OcioGpuProgramStage>(stage);
    value.semanticsId = reader.text(4096);
    value.limits.maxShaderBytes = reader.u64();
    value.limits.maxTextures = reader.u32();
    value.limits.max3dEdge = reader.u32();
    value.limits.maxAggregateLutBytes = reader.u64();
    value.limits.maxUniforms = reader.u32();
    value.limits.maxDynamicPropertyCount = reader.u32();
    value.limits.maxUniformBufferBytes = reader.u64();
    value.limits.maxStorageBufferBytes = reader.u64();
    if (!reader.finished())
        return std::nullopt;
    return value;
}

inline void encodeConfigNames(ConfigWriter& writer, const std::vector<std::string>& names) {
    writer.u32(static_cast<std::uint32_t>(names.size()));
    for (const auto& name : names)
        writer.text(name);
}

[[nodiscard]] inline std::vector<std::string> decodeConfigNames(ConfigReader& reader) {
    std::vector<std::string> names;
    const auto count = reader.u32();
    if (!reader.ok() || count > kConfigMaximumNames) {
        reader.fail();
        return names;
    }
    for (std::uint32_t index = 0; index < count && reader.ok(); ++index)
        names.push_back(reader.text(kOcioLooseMaximumKeyBytes));
    return names;
}

[[nodiscard]] inline std::vector<std::byte> encodeConfigOpenReply(const ConfigOpenReply& value) {
    ConfigWriter writer;
    writer.u32(kConfigRecordVersion);
    writer.u32(static_cast<std::uint32_t>(value.outcome));
    writer.u32(static_cast<std::uint32_t>(value.invalidReason));
    encodeConfigNames(writer, value.consumedKeys);
    encodeConfigNames(writer, value.missing);
    const auto& d = value.description;
    for (const auto* text :
         {&d.processColorSpaceId, &d.outputColorSpaceId, &d.displayName, &d.viewName, &d.configName,
          &d.sRgbTextureColorSpaceId, &d.rec709VideoColorSpaceId, &d.rec2100PqColorSpaceId,
          &d.rec2100HlgColorSpaceId, &d.ocioVersion, &d.compilerId, &d.compilerVersion,
          &d.targetTriple})
        writer.text(*text);
    writer.u32(static_cast<std::uint32_t>(d.colorSpaces.size()));
    for (const auto& space : d.colorSpaces) {
        writer.text(space.id);
        writer.text(space.family);
        writer.u32(space.sceneLinear ? 1U : 0U);
    }
    writer.u32(static_cast<std::uint32_t>(d.displays.size()));
    for (const auto& entry : d.displays) {
        writer.text(entry.display);
        writer.text(entry.view);
        writer.text(entry.colourSpaceId);
        writer.u32((entry.isDefault ? 1U : 0U) | (entry.isViewerDefault ? 2U : 0U));
    }
    return std::move(writer).take();
}

[[nodiscard]] inline std::optional<ConfigOpenReply>
decodeConfigOpenReply(const std::span<const std::byte> bytes) {
    ConfigReader reader(bytes);
    if (reader.u32() != kConfigRecordVersion)
        return std::nullopt;
    ConfigOpenReply value;
    const auto outcome = reader.u32();
    const auto reason = reader.u32();
    if (outcome > static_cast<std::uint32_t>(ConfigOpenOutcome::MissingResource) ||
        reason > static_cast<std::uint32_t>(OcioBuiltInInvalidReason::ResourceTableLimitExceeded))
        return std::nullopt;
    value.outcome = static_cast<ConfigOpenOutcome>(outcome);
    value.invalidReason = static_cast<OcioBuiltInInvalidReason>(reason);
    value.consumedKeys = decodeConfigNames(reader);
    value.missing = decodeConfigNames(reader);
    auto& d = value.description;
    for (auto* text : {&d.processColorSpaceId, &d.outputColorSpaceId, &d.displayName, &d.viewName,
                       &d.configName, &d.sRgbTextureColorSpaceId, &d.rec709VideoColorSpaceId,
                       &d.rec2100PqColorSpaceId, &d.rec2100HlgColorSpaceId, &d.ocioVersion,
                       &d.compilerId, &d.compilerVersion, &d.targetTriple})
        *text = reader.text(4096);
    const auto spaces = reader.u32();
    if (!reader.ok() || spaces > kConfigMaximumNames)
        return std::nullopt;
    for (std::uint32_t index = 0; index < spaces && reader.ok(); ++index) {
        OcioColorSpaceInfo space;
        space.id = reader.text(4096);
        space.family = reader.text(4096);
        space.sceneLinear = reader.u32() == 1U;
        d.colorSpaces.push_back(std::move(space));
    }
    const auto displays = reader.u32();
    if (!reader.ok() || displays > kConfigMaximumNames)
        return std::nullopt;
    for (std::uint32_t index = 0; index < displays && reader.ok(); ++index) {
        DisplayViewEntry entry;
        entry.display = reader.text(4096);
        entry.view = reader.text(4096);
        entry.colourSpaceId = reader.text(4096);
        const auto flags = reader.u32();
        entry.isDefault = (flags & 1U) != 0;
        entry.isViewerDefault = (flags & 2U) != 0;
        d.displays.push_back(std::move(entry));
    }
    if (!reader.finished())
        return std::nullopt;
    return value;
}

[[nodiscard]] inline std::vector<std::byte>
encodeConfigProcessorReply(const ConfigProcessorReply& value) {
    ConfigWriter writer;
    writer.u32(kConfigRecordVersion);
    writer.u32(static_cast<std::uint32_t>(value.error));
    writer.u64(value.token);
    writer.u32(value.identity ? 1U : 0U);
    writer.text(value.cacheId);
    return std::move(writer).take();
}

[[nodiscard]] inline std::optional<ConfigProcessorReply>
decodeConfigProcessorReply(const std::span<const std::byte> bytes) {
    ConfigReader reader(bytes);
    if (reader.u32() != kConfigRecordVersion)
        return std::nullopt;
    ConfigProcessorReply value;
    const auto error = reader.u32();
    if (error > static_cast<std::uint32_t>(ConfigProcessorError::MemoryLimit))
        return std::nullopt;
    value.error = static_cast<ConfigProcessorError>(error);
    value.token = reader.u64();
    value.identity = reader.u32() == 1U;
    value.cacheId = reader.text(65536);
    if (!reader.finished())
        return std::nullopt;
    return value;
}

// An .ocioz archive is a zip file: it starts with a local file header. Anything else under that
// name is refused before OpenColorIO sees it, so a plain config cannot arrive as an "archive" and
// be read as a file on disk.
[[nodiscard]] inline bool looksLikeConfigArchive(const std::span<const std::byte> head) noexcept {
    constexpr std::array<std::byte, 4> magic{std::byte{'P'}, std::byte{'K'}, std::byte{3},
                                             std::byte{4}};
    return head.size() >= magic.size() && std::memcmp(head.data(), magic.data(), magic.size()) == 0;
}

// The canonical key of a path the config asks for: root-relative, '/' separated, no empty, "."
// or ".." segment. Empty when the path cannot name a supplied resource.
[[nodiscard]] inline std::string canonicalConfigResourceKey(std::string_view path) {
    std::string key;
    if (path.empty() || path.front() == '/' || path.find('\0') != std::string_view::npos)
        return {};
    while (!path.empty()) {
        const auto cut = path.find_first_of("/\\");
        const auto segment = path.substr(0, cut);
        path = cut == std::string_view::npos ? std::string_view{} : path.substr(cut + 1);
        if (segment.empty() || segment == ".")
            continue;
        if (segment == "..")
            return {};
        if (!key.empty())
            key += '/';
        key += segment;
    }
    return key;
}

} // namespace bloom::color::detail
