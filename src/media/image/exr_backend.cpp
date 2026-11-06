#include "exr_backend.hpp"
#include "image_budget.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_cpu_input_processor.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticities.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfCompression.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfPartType.h>
#include <ImfIntAttribute.h>
#include <ImfPixelType.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfVersion.h>
#include <cstring>
#include <stdexcept>

#include <algorithm>
#include <cctype>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bloom::media::detail {
namespace {

constexpr std::array<std::uint32_t, 8> kRec709D65ChromaticityBits{
    0x3f23d70aU, 0x3ea8f5c3U, 0x3e99999aU, 0x3f19999aU,
    0x3e19999aU, 0x3d75c28fU, 0x3ea01a37U, 0x3ea872b0U};
constexpr std::array<float, 8> kAcesAp0Chromaticities{0.7347F, 0.2653F,  0.0F,     1.0F,
                                                      0.0001F, -0.0770F, 0.32168F, 0.33767F};

class RevisionStream final : public Imf::IStream {
  public:
    explicit RevisionStream(const std::span<const std::byte> bytes)
        : Imf::IStream("immutable-image-source"), bytes_(bytes) {}
    bool read(char* destination, const int count) override {
        if (count < 0 || static_cast<std::size_t>(count) > bytes_.size() - position_)
            throw std::runtime_error("truncated immutable image source");
        std::memcpy(destination, bytes_.data() + position_, static_cast<std::size_t>(count));
        position_ += static_cast<std::size_t>(count);
        return position_ != bytes_.size();
    }
    std::uint64_t tellg() override { return position_; }
    void seekg(const std::uint64_t position) override {
        if (position > bytes_.size())
            throw std::runtime_error("invalid immutable image seek");
        position_ = static_cast<std::size_t>(position);
    }

  private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

template <typename T>
[[nodiscard]] ImageResult<T> failure(const ImageDiagnosticCode code, std::string message) {
    return {{}, std::move(message), false, code};
}

[[nodiscard]] bool cancelled(const CancelImageWork& cancel) { return cancel && cancel(); }

[[nodiscard]] std::string utf8Path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

[[nodiscard]] bool readVersionField(const std::filesystem::path& path, std::uint32_t& version,
                                    ImageDiagnosticCode& code) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        code = ImageDiagnosticCode::FileUnavailable;
        return false;
    }
    std::array<unsigned char, 8> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        code = ImageDiagnosticCode::Truncated;
        return false;
    }
    version = static_cast<std::uint32_t>(bytes[4]) | (static_cast<std::uint32_t>(bytes[5]) << 8U) |
              (static_cast<std::uint32_t>(bytes[6]) << 16U) |
              (static_cast<std::uint32_t>(bytes[7]) << 24U);
    return true;
}

[[nodiscard]] std::optional<std::uint64_t> boxExtent(const int minimum, const int maximum) {
    const auto distance = static_cast<std::int64_t>(maximum) - minimum;
    if (distance < 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(distance) + 1U;
}

[[nodiscard]] std::optional<ImageDiagnosticCode> validateVersion(const std::uint32_t version) {
    const auto raw = static_cast<int>(version);
    if (Imf::getVersion(raw) != Imf::EXR_VERSION)
        return ImageDiagnosticCode::UnsupportedVersion;
    if (!Imf::supportsFlags(Imf::getFlags(raw)))
        return ImageDiagnosticCode::UnsupportedVersion;
    if (Imf::isNonImage(raw))
        return ImageDiagnosticCode::UnsupportedDeepImage;
    return std::nullopt;
}

struct ExrLayout final {
    Imath::Box2i dataWindow;
    Imath::Box2i displayWindow;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t displayWidth = 0;
    std::uint64_t displayHeight = 0;
    std::uint8_t bitDepth = 0;
    bool hasAlpha = false;
    bool luminance = false;
    // The selected layer carries R/G/B or Y colour channels (ADR 0026); without them the part has
    // no colour image and only its channel set is available.
    bool colour = false;
    bool layerFound = false;
    // Colour channel names in R, G, B, A order (Y in slot 0 for luminance); empty when absent.
    std::array<std::string, 4> colourNames;
    bool tiled = false;
    std::uint32_t tileHeight = 1U;
    ImageAlphaAssociation alphaAssociation = ImageAlphaAssociation::Premultiplied;
    std::string colorSpaceTag;
    std::string interpretationAssumption;
};

[[nodiscard]] bool sameBits(const float value, const std::uint32_t expected) {
    return std::bit_cast<std::uint32_t>(value) == expected;
}

[[nodiscard]] bool isRec709D65(const Imf::Chromaticities& chromaticities) {
    const std::array<float, 8> values{chromaticities.red.x,   chromaticities.red.y,
                                      chromaticities.green.x, chromaticities.green.y,
                                      chromaticities.blue.x,  chromaticities.blue.y,
                                      chromaticities.white.x, chromaticities.white.y};
    for (std::size_t index = 0; index < values.size(); ++index)
        if (!std::isfinite(values[index]) ||
            !sameBits(values[index], kRec709D65ChromaticityBits[index]))
            return false;
    return true;
}

[[nodiscard]] bool isAcesAp1(const Imf::Chromaticities& chromaticities) {
    const std::array<std::uint32_t, 8> expected = bloom::color::kAcesCgV1ChromaticityBits;
    const std::array<float, 8> values{chromaticities.red.x,   chromaticities.red.y,
                                      chromaticities.green.x, chromaticities.green.y,
                                      chromaticities.blue.x,  chromaticities.blue.y,
                                      chromaticities.white.x, chromaticities.white.y};
    for (std::size_t index = 0; index < values.size(); ++index)
        if (!std::isfinite(values[index]) ||
            std::bit_cast<std::uint32_t>(values[index]) != expected[index])
            return false;
    return true;
}

[[nodiscard]] bool isAcesAp0(const Imf::Chromaticities& chromaticities) {
    const std::array<float, 8> values{chromaticities.red.x,   chromaticities.red.y,
                                      chromaticities.green.x, chromaticities.green.y,
                                      chromaticities.blue.x,  chromaticities.blue.y,
                                      chromaticities.white.x, chromaticities.white.y};
    for (std::size_t index = 0; index < values.size(); ++index)
        if (!std::isfinite(values[index]) ||
            std::abs(values[index] - kAcesAp0Chromaticities[index]) > 1.0e-6F)
            return false;
    return true;
}

[[nodiscard]] std::optional<ImageAlphaAssociation> alphaAssociation(const Imf::Header& header) {
    // OpenEXR has no required alpha-association attribute. OIIO and a few production tools use
    // these bounded metadata spellings; accepting them makes the interpretation explicit while
    // retaining the OpenEXR convention (associated/premultiplied) when absent.
    if (const auto* attribute =
            header.findTypedAttribute<Imf::StringAttribute>("alphaAssociation")) {
        const auto& value = attribute->value();
        if (value == "straight" || value == "unassociated")
            return ImageAlphaAssociation::Straight;
        if (value == "premultiplied" || value == "associated")
            return ImageAlphaAssociation::Premultiplied;
        return std::nullopt;
    }
    if (const auto* attribute = header.findTypedAttribute<Imf::IntAttribute>("unassociatedAlpha"))
        return attribute->value() != 0 ? ImageAlphaAssociation::Straight
                                       : ImageAlphaAssociation::Premultiplied;
    if (const auto* attribute =
            header.findTypedAttribute<Imf::IntAttribute>("oiio:UnassociatedAlpha"))
        return attribute->value() != 0 ? ImageAlphaAssociation::Straight
                                       : ImageAlphaAssociation::Premultiplied;
    return ImageAlphaAssociation::Premultiplied;
}

[[nodiscard]] bool partIsTiled(const Imf::Header& header) {
    return header.hasType() ? Imf::isTiled(header.type()) : header.hasTileDescription();
}

[[nodiscard]] std::optional<ExrLayout> inspectHeader(const Imf::Header& header,
                                                     const std::string_view layer,
                                                     ImageDiagnosticCode& error) {
    if (header.hasType() && Imf::isDeepData(header.type())) {
        error = ImageDiagnosticCode::UnsupportedDeepImage;
        return std::nullopt;
    }
    const bool tiled = partIsTiled(header);
    const auto dataWidth = boxExtent(header.dataWindow().min.x, header.dataWindow().max.x);
    const auto dataHeight = boxExtent(header.dataWindow().min.y, header.dataWindow().max.y);
    const auto displayWidth = boxExtent(header.displayWindow().min.x, header.displayWindow().max.x);
    const auto displayHeight =
        boxExtent(header.displayWindow().min.y, header.displayWindow().max.y);
    if (!dataWidth || !dataHeight || !displayWidth || !displayHeight) {
        error = ImageDiagnosticCode::InvalidDataWindow;
        return std::nullopt;
    }
    // There is no fixed dimension or pixel ceiling: OpenEXR is admitted at decode time by the
    // caller's pixelBudget. The only geometry constraint is what the image types can represent
    // (32-bit extent) plus multiplication overflow, and this check never allocates pixel storage.
    const auto maximumExtent =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    const auto productOverflows = [](const std::uint64_t a, const std::uint64_t b) {
        return b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b;
    };
    if (*dataWidth > maximumExtent || *dataHeight > maximumExtent ||
        *displayWidth > maximumExtent || *displayHeight > maximumExtent ||
        productOverflows(*dataWidth, *dataHeight) ||
        productOverflows(*displayWidth, *displayHeight)) {
        error = ImageDiagnosticCode::DimensionsExceeded;
        return std::nullopt;
    }
    std::uint32_t tileHeight = 1U;
    if (tiled) {
        const auto* tiles = header.findTypedAttribute<Imf::TileDescriptionAttribute>("tiles");
        if (tiles == nullptr || tiles->value().mode != Imf::ONE_LEVEL) {
            error = ImageDiagnosticCode::UnsupportedTiledLevels;
            return std::nullopt;
        }
        tileHeight = static_cast<std::uint32_t>(tiles->value().ySize);
        if (tileHeight == 0U) {
            error = ImageDiagnosticCode::InvalidHeader;
            return std::nullopt;
        }
    }

    // Every channel of the part must be a supported, full-resolution sample kind, whichever layer
    // is selected; the colour image is formed from the selected layer's R/G/B[/A] or Y[/A].
    const auto& channels = header.channels();
    std::size_t channelCount = 0;
    bool layerFound = false;
    std::array<const Imf::Channel*, 5> colour{}; // R, G, B, A, Y
    std::array<std::string, 5> colourNames;
    std::uint8_t firstBitDepth = 0;
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        ++channelCount;
        const auto& channel = it.channel();
        if (channel.type != Imf::HALF && channel.type != Imf::FLOAT && channel.type != Imf::UINT) {
            error = ImageDiagnosticCode::UnsupportedChannelType;
            return std::nullopt;
        }
        if (channel.xSampling != 1 || channel.ySampling != 1) {
            error = ImageDiagnosticCode::UnsupportedChannels;
            return std::nullopt;
        }
        if (firstBitDepth == 0)
            firstBitDepth = channel.type == Imf::HALF ? 16U : 32U;
        const std::string_view name = it.name();
        if (render::channelLayerName(name) != layer)
            continue;
        layerFound = true;
        if (channel.type == Imf::UINT)
            continue;
        const auto suffix = name.substr(layer.empty() ? 0 : layer.size() + 1);
        constexpr std::array<std::string_view, 5> kSuffixes{"R", "G", "B", "A", "Y"};
        for (std::size_t slot = 0; slot < kSuffixes.size(); ++slot)
            if (suffix == kSuffixes[slot]) {
                colour[slot] = &channel;
                colourNames[slot] = std::string(name);
            }
    }
    if (channelCount == 0) {
        error = ImageDiagnosticCode::UnsupportedChannels;
        return std::nullopt;
    }
    const bool rgb = colour[0] != nullptr && colour[1] != nullptr && colour[2] != nullptr;
    const bool luminance = !rgb && colour[4] != nullptr;
    const bool hasColour = rgb || luminance;
    const bool hasAlpha = hasColour && colour[3] != nullptr;
    std::uint8_t bitDepth = hasColour ? 16U : firstBitDepth;
    for (const auto* channel : colour)
        if (hasColour && channel != nullptr && channel->type == Imf::FLOAT)
            bitDepth = 32U;
    const auto alpha = alphaAssociation(header);
    if (!alpha) {
        error = ImageDiagnosticCode::InvalidHeader;
        return std::nullopt;
    }

    ExrLayout result;
    result.dataWindow = header.dataWindow();
    result.displayWindow = header.displayWindow();
    result.width = *dataWidth;
    result.height = *dataHeight;
    result.displayWidth = *displayWidth;
    result.displayHeight = *displayHeight;
    result.bitDepth = bitDepth;
    result.hasAlpha = hasAlpha;
    result.luminance = luminance;
    result.colour = hasColour;
    result.layerFound = layerFound;
    if (rgb) {
        result.colourNames = {colourNames[0], colourNames[1], colourNames[2],
                              hasAlpha ? colourNames[3] : std::string{}};
    } else if (luminance) {
        result.colourNames = {colourNames[4], {}, {}, hasAlpha ? colourNames[3] : std::string{}};
    }
    result.tiled = tiled;
    result.tileHeight = tileHeight;
    result.alphaAssociation = *alpha;
    if (const auto* chromaticities =
            header.findTypedAttribute<Imf::ChromaticitiesAttribute>("chromaticities")) {
        result.colorSpaceTag = isAcesAp0(chromaticities->value())     ? "ACES2065-1"
                               : isAcesAp1(chromaticities->value())   ? "ACEScg"
                               : isRec709D65(chromaticities->value()) ? "lin_rec709_scene"
                                                                      : "exr.chromaticities";
        result.interpretationAssumption =
            result.colorSpaceTag == "ACES2065-1" ? "EXR chromaticities identify ACES AP0"
            : result.colorSpaceTag == "ACEScg"   ? "EXR chromaticities identify ACES AP1"
            : result.colorSpaceTag == "lin_rec709_scene"
                ? "EXR chromaticities identify Rec.709/D65 scene-linear RGB"
                : "EXR chromaticities identify scene-linear RGB outside the supported "
                  "automatic tags";
    } else {
        result.colorSpaceTag = "lin_rec709_scene";
        result.interpretationAssumption =
            "EXR scene-linear convention assumed; chromaticities attribute is absent";
    }
    return result;
}

[[nodiscard]] ImageChannelSampleKind sampleKind(const Imf::PixelType type) {
    return type == Imf::HALF    ? ImageChannelSampleKind::Half
           : type == Imf::FLOAT ? ImageChannelSampleKind::Float
                                : ImageChannelSampleKind::Uint;
}

[[nodiscard]] ImagePartInfo makePartInfo(const Imf::Header& header, const ExrLayout& layout) {
    ImagePartInfo part;
    if (header.hasName())
        part.name = header.name();
    if (header.hasView())
        part.view = header.view();
    part.tiled = layout.tiled;
    part.dataX = layout.dataWindow.min.x;
    part.dataY = layout.dataWindow.min.y;
    part.dataWidth = static_cast<std::uint32_t>(layout.width);
    part.dataHeight = static_cast<std::uint32_t>(layout.height);
    part.displayX = layout.displayWindow.min.x;
    part.displayY = layout.displayWindow.min.y;
    part.displayWidth = static_cast<std::uint32_t>(layout.displayWidth);
    part.displayHeight = static_cast<std::uint32_t>(layout.displayHeight);
    part.alphaAssociation = layout.alphaAssociation;
    part.colorSpaceTag = layout.colorSpaceTag;
    for (auto it = header.channels().begin(); it != header.channels().end(); ++it) {
        part.channels.push_back({it.name(), sampleKind(it.channel().type)});
        const auto layer = std::string(render::channelLayerName(it.name()));
        if (std::ranges::find(part.layers, layer) == part.layers.end())
            part.layers.push_back(layer);
    }
    // A Cryptomatte rank layer (`<type>NN`, named by a `cryptomatte/<key>/name` attribute) carries
    // ID bits and coverage in its R/G/B/A channels: data, never a colour layer (#364).
    std::vector<std::string> cryptomatteTypes;
    for (auto attribute = header.begin(); attribute != header.end(); ++attribute) {
        const std::string_view name = attribute.name();
        if (!name.starts_with("cryptomatte/") || !name.ends_with("/name"))
            continue;
        if (const auto* text = dynamic_cast<const Imf::StringAttribute*>(&attribute.attribute()))
            cryptomatteTypes.push_back(text->value());
    }
    const auto isCryptomatteRank = [&](const std::string_view layer) {
        return layer.size() > 2 && std::isdigit(static_cast<unsigned char>(layer.back())) &&
               std::isdigit(static_cast<unsigned char>(layer[layer.size() - 2])) &&
               std::ranges::find(cryptomatteTypes, layer.substr(0, layer.size() - 2)) !=
                   cryptomatteTypes.end();
    };
    for (const auto& layer : part.layers) {
        if (isCryptomatteRank(layer))
            continue;
        ImageDiagnosticCode ignored = ImageDiagnosticCode::None;
        if (const auto selected = inspectHeader(header, layer, ignored); selected && selected->colour)
            part.colourLayers.push_back(layer);
    }
    return part;
}

[[nodiscard]] ImageProbe makeProbe(const ExrLayout& layout, const core::Sha256Digest digest) {
    auto probe = ImageProbe{static_cast<std::uint32_t>(layout.width),
                            static_cast<std::uint32_t>(layout.height),
                            layout.bitDepth,
                            digest,
                            ImageFormat::Exr,
                            ImageColorSpace::Linear,
                            layout.alphaAssociation,
                            layout.colorSpaceTag,
                            layout.interpretationAssumption};
    probe.dataOffsetX = static_cast<std::int64_t>(layout.dataWindow.min.x) -
                        static_cast<std::int64_t>(layout.displayWindow.min.x);
    probe.dataOffsetY = static_cast<std::int64_t>(layout.dataWindow.min.y) -
                        static_cast<std::int64_t>(layout.displayWindow.min.y);
    probe.frameWidth = static_cast<std::uint32_t>(layout.displayWidth);
    probe.frameHeight = static_cast<std::uint32_t>(layout.displayHeight);
    return probe;
}

// Every part is validated; the scalar probe fields describe the first part's root layer.
[[nodiscard]] ImageResult<ImageProbe> probeParts(const Imf::MultiPartInputFile& input,
                                                 const core::Sha256Digest digest,
                                                 const CancelImageWork& cancel) {
    if (const auto error = validateVersion(static_cast<std::uint32_t>(input.version())))
        return failure<ImageProbe>(*error, "OpenEXR version or flags are unsupported");
    if (input.parts() < 1)
        return failure<ImageProbe>(ImageDiagnosticCode::InvalidHeader, "OpenEXR has no parts");
    std::vector<ImagePartInfo> parts;
    std::optional<ExrLayout> first;
    for (int index = 0; index < input.parts(); ++index) {
        if (cancelled(cancel))
            return {{}, "Image probe cancelled", true, ImageDiagnosticCode::Cancelled};
        ImageDiagnosticCode error = ImageDiagnosticCode::InvalidHeader;
        const auto layout = inspectHeader(input.header(index), {}, error);
        if (!layout)
            return failure<ImageProbe>(error, "OpenEXR header is unsupported or invalid");
        parts.push_back(makePartInfo(input.header(index), *layout));
        if (index == 0)
            first = layout;
    }
    auto probe = makeProbe(*first, digest);
    probe.parts = std::move(parts);
    return {std::move(probe), {}};
}

[[nodiscard]] std::optional<int> findPartIndex(const Imf::MultiPartInputFile& input,
                                               const std::string_view name) {
    if (name.empty())
        return input.parts() > 0 ? std::optional<int>{0} : std::nullopt;
    for (int index = 0; index < input.parts(); ++index)
        if (input.header(index).hasName() && input.header(index).name() == name)
            return index;
    return std::nullopt;
}

// ADR 0026 / #549: named channels of a part as float planes over its whole data window, with the
// part's string metadata. Each plane is its own OpenEXR slice of the data window, so whatever read
// pass inserts them fills them from the chunks it inflates anyway (a colour decode's, or a channel
// decode's own). Half and float widen to Float32; uint keeps its 32-bit pattern in the lane.
struct ExrChannelPlanes final {
    struct Selected final {
        std::string name;
        Imf::PixelType type = Imf::FLOAT;
    };
    std::vector<Selected> selected;
    std::vector<std::vector<float>> planes;
    render::ChannelMetadata metadata;
    // Every selected plane plus the metadata text.
    std::uint64_t bytes = 0;
};

// `names` in order (every channel of the part when empty); planes are not allocated yet.
[[nodiscard]] ImageResult<ExrChannelPlanes>
selectExrChannels(const Imf::Header& header, const ExrLayout& layout,
                  const std::span<const std::string> names) {
    ExrChannelPlanes result;
    if (names.empty()) {
        for (auto it = header.channels().begin(); it != header.channels().end(); ++it)
            result.selected.push_back({it.name(), it.channel().type});
    } else {
        for (const auto& name : names) {
            const auto* channel = header.channels().findChannel(name);
            if (channel == nullptr)
                return failure<ExrChannelPlanes>(ImageDiagnosticCode::ChannelNotFound,
                                                 "A selected OpenEXR channel is not present");
            if (std::ranges::any_of(result.selected, [&](const ExrChannelPlanes::Selected& item) {
                    return item.name == name;
                }))
                return failure<ExrChannelPlanes>(ImageDiagnosticCode::UnsupportedChannels,
                                                 "A channel is selected more than once");
            result.selected.push_back({name, channel->type});
        }
    }
    std::uint64_t metadataBytes = 0;
    for (auto it = header.begin(); it != header.end(); ++it)
        if (const auto* text = dynamic_cast<const Imf::StringAttribute*>(&it.attribute())) {
            metadataBytes += std::strlen(it.name()) + text->value().size();
            result.metadata.emplace_back(it.name(), text->value());
        }
    std::uint64_t pixels = 0;
    std::uint64_t planeBytes = 0;
    if (!checkedSizeProduct(layout.width, layout.height, pixels) ||
        !checkedSizeProduct(pixels, result.selected.size() * sizeof(float), planeBytes) ||
        planeBytes > std::numeric_limits<std::uint64_t>::max() - metadataBytes)
        return failure<ExrChannelPlanes>(
            ImageDiagnosticCode::PixelBudgetExceeded,
            "Decoded channel set exceeds the pixel working-set budget");
    result.bytes = planeBytes + metadataBytes;
    return {std::move(result), {}};
}

void allocateExrChannelPlanes(ExrChannelPlanes& planes, const ExrLayout& layout) {
    planes.planes.resize(planes.selected.size());
    for (auto& plane : planes.planes)
        plane.assign(static_cast<std::size_t>(layout.width * layout.height), 0.0F);
}

// Slices address absolute data-window coordinates; each plane starts at the data window.
void insertExrChannelSlices(Imf::FrameBuffer& frameBuffer, ExrChannelPlanes& planes,
                            const ExrLayout& layout) {
    const auto origin = Imath::V2i(layout.dataWindow.min.x, layout.dataWindow.min.y);
    const auto rowStride = static_cast<std::size_t>(layout.width) * sizeof(float);
    for (std::size_t channel = 0; channel < planes.selected.size(); ++channel) {
        const auto readType = planes.selected[channel].type == Imf::UINT ? Imf::UINT : Imf::FLOAT;
        frameBuffer.insert(planes.selected[channel].name,
                           Imf::Slice::Make(readType, planes.planes[channel].data(), origin,
                                            static_cast<std::int64_t>(layout.width),
                                            static_cast<std::int64_t>(layout.height), sizeof(float),
                                            rowStride));
    }
}

[[nodiscard]] ImageResult<std::shared_ptr<const render::ChannelSetImage>>
finishExrChannelPlanes(ExrChannelPlanes&& planes, const ExrLayout& layout,
                       const std::string& inputColorSpaceId) {
    using Decoded = std::shared_ptr<const render::ChannelSetImage>;
    std::vector<render::ChannelDescriptor> descriptors;
    descriptors.reserve(planes.selected.size());
    for (const auto& item : planes.selected)
        descriptors.push_back(render::describeChannel(item.name, sampleKind(item.type)));
    const auto dataWindow = render::ImageWindow::create(
        layout.dataWindow.min.x, layout.dataWindow.min.y, layout.width, layout.height);
    const auto displayWindow =
        render::ImageWindow::create(layout.displayWindow.min.x, layout.displayWindow.min.y,
                                    layout.displayWidth, layout.displayHeight);
    if (!dataWindow || !displayWindow)
        return failure<Decoded>(ImageDiagnosticCode::InvalidDataWindow,
                                "OpenEXR image window is invalid");
    auto created = render::ChannelSetImage::create(
        *dataWindow.value(), *displayWindow.value(), core::PixelAspectRatio::square(),
        std::move(descriptors), std::move(planes.planes), std::move(planes.metadata),
        std::numeric_limits<std::size_t>::max(),
        inputColorSpaceId.empty() ? layout.colorSpaceTag : inputColorSpaceId);
    if (!created)
        return failure<Decoded>(ImageDiagnosticCode::DecodeFailed, "Channel set validation failed");
    return {std::make_shared<const render::ChannelSetImage>(std::move(*created.value())), {}};
}

// #549: the planes a colour decode reads beside its layer, or nullopt (the caller decodes the
// channels separately) when a name is missing or repeated or they do not fit `room`.
[[nodiscard]] std::optional<ExrChannelPlanes>
alongsidePlanes(const ImageChannelsAlongside* alongside, const Imf::Header& header,
                const ExrLayout& layout, const std::uint64_t room) {
    if (alongside == nullptr || alongside->channels.empty())
        return std::nullopt;
    // A colour channel already has the colour decode's slice; such a read decodes on its own.
    // A subsampled channel cannot share the colour layer's full-resolution read.
    for (const auto& name : alongside->channels) {
        if (std::ranges::find(layout.colourNames, name) != layout.colourNames.end())
            return std::nullopt;
        if (const auto* channel = header.channels().findChannel(name);
            channel != nullptr && (channel->xSampling != 1 || channel->ySampling != 1))
            return std::nullopt;
    }
    auto selected = selectExrChannels(header, layout, alongside->channels);
    if (!selected.value || selected.value->bytes > room)
        return std::nullopt;
    try {
        allocateExrChannelPlanes(*selected.value, layout);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
    return std::move(*selected.value);
}

void publishAlongside(ImageChannelsAlongside* alongside, std::optional<ExrChannelPlanes>& planes,
                      const ExrLayout& layout) {
    if (alongside == nullptr || !planes)
        return;
    auto finished = finishExrChannelPlanes(std::move(*planes), layout, {});
    planes.reset();
    if (finished.value)
        alongside->decoded = std::move(*finished.value);
}

} // namespace

ImageResult<ImageProbe> probeExr(const std::filesystem::path& path,
                                 const core::Sha256Digest contentDigest,
                                 const CancelImageWork& cancel) {
    if (cancelled(cancel))
        return failure<ImageProbe>(ImageDiagnosticCode::Cancelled, "Image probe cancelled");
    try {
        std::uint32_t version = 0;
        ImageDiagnosticCode versionError = ImageDiagnosticCode::InvalidHeader;
        if (!readVersionField(path, version, versionError))
            return failure<ImageProbe>(versionError, "OpenEXR version field is truncated");
        if (const auto versionFailure = validateVersion(version); versionFailure.has_value())
            return failure<ImageProbe>(*versionFailure, "OpenEXR version or flags are unsupported");
        const Imf::MultiPartInputFile input(utf8Path(path).c_str(), 1);
        return probeParts(input, contentDigest, cancel);
    } catch (const std::bad_alloc&) {
        return failure<ImageProbe>(ImageDiagnosticCode::AllocationFailure,
                                   "OpenEXR probe allocation failed");
    } catch (...) {
        return failure<ImageProbe>(ImageDiagnosticCode::InvalidHeader,
                                   "OpenEXR header could not be read");
    }
}
static ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExrInput(Imf::InputPart& input, const ExrLayout& selected,
               const ImageInterpretation& interpretation,
               std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
               const CancelImageWork& cancel, const ImageProgress& progress,
               const std::size_t pixelBudget, ImageChannelsAlongside* alongside) {
    if (cancelled(cancel))
        return failure<std::shared_ptr<const render::Rgba32fImage>>(ImageDiagnosticCode::Cancelled,
                                                                    "Image decode cancelled");
    try {
        const ExrLayout* const layout = &selected;

        const auto dataWindow = render::ImageWindow::create(
            layout->dataWindow.min.x, layout->dataWindow.min.y, layout->width, layout->height);
        const auto displayWindow =
            render::ImageWindow::create(layout->displayWindow.min.x, layout->displayWindow.min.y,
                                        layout->displayWidth, layout->displayHeight);
        if (!dataWindow || !displayWindow)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::InvalidDataWindow, "OpenEXR image window is invalid");
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *dataWindow.value(), *displayWindow.value(), core::PixelAspectRatio::square());
        if (!descriptor)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::InvalidDataWindow, "OpenEXR image descriptor is invalid");
        // Peak admission: final RGBA32F storage plus the bounded scanline band and the one-row
        // scratch must fit the caller's explicit budget. This replaces four full-frame planes (peak
        // ~2x the decoded bytes) with a small band, so admission matches real peak memory.
        const auto width = layout->width;
        const auto height = layout->height;
        const std::uint64_t channelCount =
            static_cast<std::uint64_t>(layout->luminance ? 1U : 3U) + (layout->hasAlpha ? 1U : 0U);
        const std::uint64_t finalBytes = descriptor.value()->layout().pixelStorageBytes;
        constexpr std::uint64_t kBandTargetBytes = std::uint64_t{1024} * 1024U;
        std::uint64_t chunkRows = 1U;
        if (layout->tiled) {
            chunkRows = std::max<std::uint64_t>(1U, layout->tileHeight);
        } else {
            std::uint64_t bytesPerRow = 0;
            if (!detail::checkedSizeProduct(width, channelCount * sizeof(float), bytesPerRow))
                bytesPerRow = std::numeric_limits<std::uint64_t>::max();
            chunkRows = std::max<std::uint64_t>(1U, kBandTargetBytes /
                                                        std::max<std::uint64_t>(bytesPerRow, 1U));
        }
        chunkRows = std::min(chunkRows, height);
        // Every scratch term is both overflow-checked and admitted by subtraction, so no removed
        // geometry cap can wrap the working-set comparison past the budget.
        std::uint64_t rowBytes = 0;
        std::uint64_t bandRows = 0;
        std::uint64_t bandBytes = 0;
        const bool scratchSized =
            detail::checkedSizeProduct(width, sizeof(std::array<float, 4>), rowBytes) &&
            detail::checkedSizeProduct(chunkRows, width, bandRows) &&
            detail::checkedSizeProduct(bandRows, channelCount * sizeof(float), bandBytes);
        if (!scratchSized ||
            !detail::decodeWorkingSetFits(finalBytes, bandBytes, rowBytes, pixelBudget))
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::PixelBudgetExceeded,
                "Decoded image exceeds the pixel working-set budget");
        // #549: the channels read beside the layer, in the room the working set leaves.
        auto extras = alongsidePlanes(alongside, input.header(), *layout,
                                      pixelBudget - finalBytes - bandBytes - rowBytes);
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), pixelBudget);
        if (!builder)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::AllocationFailure, "Process image allocation failed");

        const bool sourcePremultiplied =
            (interpretation.alphaAssociation == ImageAlphaAssociation::Auto
                 ? layout->alphaAssociation
                 : interpretation.alphaAssociation) == ImageAlphaAssociation::Premultiplied;
        const bool stored = interpretation.storedSamples;
        bool convert =
            !stored && (!interpretation.inputColorSpaceId.empty() ||
                        interpretation.colorSpace == ImageColorSpace::Srgb || processor != nullptr);
        std::shared_ptr<const color::CpuInputProcessor> compatibilityProcessor;
        if (convert && !processor) {
            const auto resolved = color::resolveBloomNeutralV1BuiltIn(
                color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
                color::kBloomNeutralV1ConfigDigest);
            if (!resolved)
                return failure<std::shared_ptr<const render::Rgba32fImage>>(
                    ImageDiagnosticCode::DecodeFailed,
                    "Bloom Neutral input config could not be resolved");
            if (interpretation.inputColorSpaceId.empty()) {
                compatibilityProcessor = color::CpuInputProcessor::prepare(*resolved.resolved());
            } else {
                auto prepared = color::CpuColorSpaceProcessor::prepare(
                    *resolved.resolved(), interpretation.inputColorSpaceId,
                    resolved.resolved()->processColorSpaceId());
                if (!prepared)
                    return failure<std::shared_ptr<const render::Rgba32fImage>>(
                        ImageDiagnosticCode::ColorSpaceUnavailable,
                        "Input color space is unavailable in the selected config");
                processor = std::move(prepared).takeProcessor();
            }
        }
        if (convert && !processor && !compatibilityProcessor)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::ColorSpaceUnavailable,
                "Input color processor could not be prepared");
        // An identity input transform (the input space is the working space) has nothing to
        // convert: the samples are kept as decoded, with no un-premultiply/premultiply round trip
        // that would move partial-alpha colour by an ULP. The GPU route uploads the same samples
        // (#545).
        if (convert && processor && processor->isIdentity())
            convert = false;

        const auto bandSamples =
            static_cast<std::size_t>(chunkRows) * static_cast<std::size_t>(width);
        std::array<std::vector<float>, 4> band;
        const std::size_t rgbPlanes = layout->luminance ? 1U : 3U;
        for (std::size_t index = 0; index < rgbPlanes; ++index)
            band[index].assign(bandSamples, 0.0F);
        if (layout->hasAlpha)
            band[3].assign(bandSamples, 1.0F);
        std::vector<std::array<float, 4>> row(static_cast<std::size_t>(width));
        const auto rowStride = static_cast<std::size_t>(width) * sizeof(float);

        std::uint64_t completed = 0;
        std::uint64_t repairedPixels = 0;
        std::vector<bool> repairedRow(static_cast<std::size_t>(width));
        if (progress)
            progress(0, height);
        while (completed < height) {
            if (cancelled(cancel))
                return failure<std::shared_ptr<const render::Rgba32fImage>>(
                    ImageDiagnosticCode::Cancelled, "Image decode cancelled");
            // A tiled read can touch a whole tile row, so tiled chunks are aligned to the tile
            // grid; scanline chunks are a bounded run of consecutive rows.
            const auto first = layout->tiled ? (completed / chunkRows) * chunkRows : completed;
            const auto count = std::min(chunkRows, height - first);
            const auto firstY = static_cast<std::int64_t>(layout->dataWindow.min.y) +
                                static_cast<std::int64_t>(first);
            const auto lastY = firstY + static_cast<std::int64_t>(count) - 1;
            Imf::FrameBuffer frameBuffer;
            const auto origin = Imath::V2i(layout->dataWindow.min.x, static_cast<int>(firstY));
            const auto insert = [&](const char* name, std::vector<float>& plane) {
                frameBuffer.insert(name, Imf::Slice::Make(Imf::FLOAT, plane.data(), origin,
                                                          static_cast<std::int64_t>(width),
                                                          static_cast<std::int64_t>(count),
                                                          sizeof(float), rowStride));
            };
            if (layout->luminance)
                insert(layout->colourNames[0].c_str(), band[0]);
            else {
                insert(layout->colourNames[0].c_str(), band[0]);
                insert(layout->colourNames[1].c_str(), band[1]);
                insert(layout->colourNames[2].c_str(), band[2]);
            }
            if (layout->hasAlpha)
                insert(layout->colourNames[3].c_str(), band[3]);
            if (extras)
                insertExrChannelSlices(frameBuffer, *extras, *layout);
            input.setFrameBuffer(frameBuffer);
            input.readPixels(static_cast<int>(firstY), static_cast<int>(lastY));

            for (std::uint64_t local = 0; local < count; ++local) {
                if (cancelled(cancel))
                    return failure<std::shared_ptr<const render::Rgba32fImage>>(
                        ImageDiagnosticCode::Cancelled, "Image decode cancelled");
                const auto bandOffset =
                    static_cast<std::size_t>(local) * static_cast<std::size_t>(width);
                for (std::uint64_t x = 0; x < width; ++x) {
                    const auto offset = bandOffset + static_cast<std::size_t>(x);
                    const auto alpha = layout->hasAlpha ? band[3][offset] : 1.0F;
                    const auto luminance = layout->luminance ? band[0][offset] : 0.0F;
                    auto& target = row[static_cast<std::size_t>(x)];
                    target = layout->luminance
                                 ? std::array<float, 4>{luminance, luminance, luminance, alpha}
                                 : std::array<float, 4>{band[0][offset], band[1][offset],
                                                        band[2][offset], alpha};
                    // Non-finite policy, before any colour conversion: a NaN/Inf component
                    // becomes zero and alpha is clamped to [0, 1].
                    bool repaired = false;
                    for (auto& component : target)
                        if (!std::isfinite(component)) {
                            component = 0.0F;
                            repaired = true;
                        }
                    if (target[3] < 0.0F || target[3] > 1.0F) {
                        target[3] = std::clamp(target[3], 0.0F, 1.0F);
                        repaired = true;
                    }
                    // Premultiplied RGB at zero alpha (additive emission) cannot survive the v1
                    // process image; it is dropped and reported.
                    if (!stored && sourcePremultiplied && target[3] == 0.0F &&
                        (target[0] != 0.0F || target[1] != 0.0F || target[2] != 0.0F))
                        repaired = true;
                    repairedRow[static_cast<std::size_t>(x)] = repaired;
                    if (sourcePremultiplied && convert) {
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            target[channel] = target[3] > 0.0F ? target[channel] / target[3] : 0.0F;
                    }
                }
                if (convert && ((processor && !processor->apply(row)) ||
                                (compatibilityProcessor && !compatibilityProcessor->apply(row))))
                    return failure<std::shared_ptr<const render::Rgba32fImage>>(
                        ImageDiagnosticCode::ColorSpaceUnavailable,
                        "Input color conversion failed");
                auto outputRow = builder.value()->row(
                    static_cast<std::int64_t>(layout->dataWindow.min.y) +
                    static_cast<std::int64_t>(first) + static_cast<std::int64_t>(local));
                if (!outputRow)
                    return failure<std::shared_ptr<const render::Rgba32fImage>>(
                        ImageDiagnosticCode::InvalidDataWindow, "OpenEXR output row is invalid");
                for (std::uint64_t x = 0; x < width; ++x) {
                    auto sample = row[static_cast<std::size_t>(x)];
                    // The same policy after colour conversion, which can itself overflow. The
                    // frame keeps rendering; each repaired pixel is counted once.
                    bool repaired = repairedRow[static_cast<std::size_t>(x)];
                    for (auto& component : sample)
                        if (!std::isfinite(component)) {
                            component = 0.0F;
                            repaired = true;
                        }
                    if (sample[3] < 0.0F || sample[3] > 1.0F) {
                        sample[3] = std::clamp(sample[3], 0.0F, 1.0F);
                        repaired = true;
                    }
                    const bool asStored = stored || (sourcePremultiplied && !convert);
                    const auto red = asStored ? sample[0] : sample[0] * sample[3];
                    const auto green = asStored ? sample[1] : sample[1] * sample[3];
                    const auto blue = asStored ? sample[2] : sample[2] * sample[3];
                    if (stored)
                        sample[3] = storedSampleAlpha(sample[3]);
                    // The v1 process image cannot carry premultiplied RGB at zero alpha
                    // (additive emission); such a pixel becomes transparent and is reported.
                    if (sample[3] == 0.0F && (red != 0.0F || green != 0.0F || blue != 0.0F))
                        repaired = true;
                    auto pixel = render::Rgba32f::fromPremultiplied(red, green, blue, sample[3]);
                    if (!pixel) {
                        // A finite input can still overflow when premultiplied.
                        pixel = render::Rgba32f::fromPremultiplied(
                            std::isfinite(red) ? red : 0.0F, std::isfinite(green) ? green : 0.0F,
                            std::isfinite(blue) ? blue : 0.0F, sample[3]);
                        repaired = true;
                    }
                    if (!pixel)
                        return failure<std::shared_ptr<const render::Rgba32fImage>>(
                            ImageDiagnosticCode::DecodeFailed, "OpenEXR contains an invalid pixel");
                    if (repaired)
                        ++repairedPixels;
                    (*outputRow.value())[static_cast<std::size_t>(x)] = *pixel.value();
                }
                ++completed;
                if (progress)
                    progress(completed, height);
            }
        }
        if (!input.isComplete())
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::Truncated, "OpenEXR pixel data is truncated");
        auto image = std::move(*builder.value()).freeze();
        if (!image)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::DecodeFailed, "Process image validation failed");
        publishAlongside(alongside, extras, *layout);
        ImageResult<std::shared_ptr<const render::Rgba32fImage>> decoded{
            std::make_shared<const render::Rgba32fImage>(std::move(*image.value())), {}};
        decoded.repairedPixels = repairedPixels;
        return decoded;
    } catch (const std::bad_alloc&) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::AllocationFailure, "OpenEXR decode allocation failed");
    } catch (...) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::Truncated, "OpenEXR pixel data could not be decoded");
    }
}

// Resolves the interpretation's part and colour layer, then decodes that layer's colour image.
static ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExrParts(Imf::MultiPartInputFile& input, const ImageInterpretation& interpretation,
               std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
               const CancelImageWork& cancel, const ImageProgress& progress,
               const std::size_t pixelBudget, ImageChannelsAlongside* alongside = nullptr) {
    using Decoded = std::shared_ptr<const render::Rgba32fImage>;
    if (const auto error = validateVersion(static_cast<std::uint32_t>(input.version())))
        return failure<Decoded>(*error, "OpenEXR version or flags are unsupported");
    const auto index = findPartIndex(input, interpretation.part);
    if (!index)
        return failure<Decoded>(ImageDiagnosticCode::PartNotFound,
                                "The selected OpenEXR part is not present");
    ImageDiagnosticCode error = ImageDiagnosticCode::InvalidHeader;
    const auto layout = inspectHeader(input.header(*index), interpretation.layer, error);
    if (!layout)
        return failure<Decoded>(error, "OpenEXR header is unsupported or invalid");
    if (!interpretation.layer.empty() && !layout->layerFound)
        return failure<Decoded>(ImageDiagnosticCode::LayerNotFound,
                                "The selected OpenEXR layer is not present");
    if (!layout->colour)
        return failure<Decoded>(ImageDiagnosticCode::NoColourChannels,
                                "No color layer — use Channels");
    Imf::InputPart part(input, *index);
    return decodeExrInput(part, *layout, interpretation, std::move(processor), cancel, progress,
                          pixelBudget, alongside);
}

// #474: OpenEXR compresses each chunk (a run of scanlines or a tile) on its own, across every
// channel of the part, so reading only the selected channels cannot skip decompression; the chunks
// are independent, though, so bands of whole chunks decode concurrently, each through its own
// reader over the same immutable bytes. The extra threads in flight across the process are bounded
// by the hardware concurrency: when many frames decode at once each gets fewer, never fewer than
// its own thread.
constexpr std::uint64_t kMinimumChunksPerExrBand = 4;
constexpr unsigned kMaximumExrBands = 8;
std::atomic<unsigned> exrBandThreadsInFlight{0};

[[nodiscard]] unsigned reserveExrBandThreads(const unsigned wanted) noexcept {
    const unsigned limit = std::max(1U, std::thread::hardware_concurrency());
    auto current = exrBandThreadsInFlight.load(std::memory_order_relaxed);
    while (true) {
        const auto take = std::min(wanted, current < limit ? limit - current : 0U);
        if (take == 0)
            return 0;
        if (exrBandThreadsInFlight.compare_exchange_weak(current, current + take,
                                                         std::memory_order_relaxed))
            return take;
    }
}

void releaseExrBandThreads(const unsigned count) noexcept {
    exrBandThreadsInFlight.fetch_sub(count, std::memory_order_relaxed);
}

// #422: the selected colour layer's binary16 samples interleaved as R, G, B, A (Y in lane 0 for a
// luminance layer), read by OpenEXR without any conversion. The ingest repairs the float decode
// would make are counted from the bits; SampleConvertV1 applies them on the device.
static ImageResult<ExrHalfSamples>
decodeExrHalfParts(Imf::MultiPartInputFile& input, const std::span<const std::byte> bytes,
                   const ImageInterpretation& interpretation, const CancelImageWork& cancel,
                   const std::size_t pixelBudget, ImageChannelsAlongside* alongside) {
    using Decoded = ExrHalfSamples;
    if (const auto error = validateVersion(static_cast<std::uint32_t>(input.version())))
        return failure<Decoded>(*error, "OpenEXR version or flags are unsupported");
    const auto index = findPartIndex(input, interpretation.part);
    if (!index)
        return failure<Decoded>(ImageDiagnosticCode::PartNotFound,
                                "The selected OpenEXR part is not present");
    ImageDiagnosticCode error = ImageDiagnosticCode::InvalidHeader;
    const auto& header = input.header(*index);
    const auto layout = inspectHeader(header, interpretation.layer, error);
    if (!layout)
        return failure<Decoded>(error, "OpenEXR header is unsupported or invalid");
    if (!interpretation.layer.empty() && !layout->layerFound)
        return failure<Decoded>(ImageDiagnosticCode::LayerNotFound,
                                "The selected OpenEXR layer is not present");
    if (!layout->colour)
        return failure<Decoded>(ImageDiagnosticCode::NoColourChannels,
                                "No color layer — use Channels");
    // Only an all-HALF, unsubsampled colour layer crosses as native samples; FLOAT and UINT
    // channels keep the float decode.
    const std::size_t laneCount = layout->luminance ? 1U : 3U;
    for (std::size_t lane = 0; lane < 4; ++lane) {
        if (lane >= laneCount && lane != 3)
            continue;
        if (lane == 3 && !layout->hasAlpha)
            continue;
        const auto* channel = header.channels().findChannel(layout->colourNames[lane]);
        if (channel == nullptr || channel->type != Imf::HALF || channel->xSampling != 1 ||
            channel->ySampling != 1)
            return failure<Decoded>(ImageDiagnosticCode::NativeSamplesUnavailable,
                                    "OpenEXR colour channels are not all half");
    }
    const auto width = layout->width;
    const auto height = layout->height;
    std::uint64_t byteCount = 0;
    if (!detail::checkedSizeProduct(width, height, byteCount) ||
        !detail::checkedSizeProduct(byteCount, 8U, byteCount) || byteCount > pixelBudget)
        return failure<Decoded>(ImageDiagnosticCode::PixelBudgetExceeded,
                                "Decoded samples exceed the pixel storage budget");
    if (cancelled(cancel))
        return {{}, "Image decode cancelled", true, ImageDiagnosticCode::Cancelled};
    // #549: the channels read beside the layer, in the room the samples leave.
    auto extras = alongsidePlanes(alongside, header, *layout, pixelBudget - byteCount);
    Decoded decoded;
    decoded.layout.dataX = layout->dataWindow.min.x;
    decoded.layout.dataY = layout->dataWindow.min.y;
    decoded.layout.width = width;
    decoded.layout.height = height;
    decoded.layout.displayX = layout->displayWindow.min.x;
    decoded.layout.displayY = layout->displayWindow.min.y;
    decoded.layout.displayWidth = layout->displayWidth;
    decoded.layout.displayHeight = layout->displayHeight;
    decoded.layout.luminance = layout->luminance;
    decoded.layout.hasAlpha = layout->hasAlpha;
    decoded.layout.premultiplied =
        (interpretation.alphaAssociation == ImageAlphaAssociation::Auto
             ? layout->alphaAssociation
             : interpretation.alphaAssociation) == ImageAlphaAssociation::Premultiplied;
    auto storage = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(byteCount));
    const auto rowStride = static_cast<std::size_t>(width) * 8U;
    // Reads are whole chunks: a tile row, or the compression's scanlines per chunk.
    const auto chunkHeight = static_cast<std::uint64_t>(
        layout->tiled ? std::max<std::uint32_t>(1U, layout->tileHeight)
                      : static_cast<std::uint32_t>(
                            std::max(1, Imf::getCompressionNumScanlines(header.compression()))));
    constexpr std::uint64_t kBandTargetBytes = std::uint64_t{1024} * 1024U;
    const std::uint64_t chunkRows = std::min(
        height, std::max<std::uint64_t>(1U, kBandTargetBytes / rowStride / chunkHeight) *
                    chunkHeight);
    Imf::FrameBuffer frameBuffer;
    // Slices address absolute data-window coordinates; the buffer starts at the data window.
    auto* const base = reinterpret_cast<char*>(storage->data());
    const auto origin = Imath::V2i(layout->dataWindow.min.x, layout->dataWindow.min.y);
    const auto insert = [&](const std::string& name, const std::size_t lane) {
        frameBuffer.insert(name, Imf::Slice::Make(Imf::HALF, base + lane * 2U, origin,
                                                  static_cast<std::int64_t>(width),
                                                  static_cast<std::int64_t>(height), 8U,
                                                  rowStride));
    };
    insert(layout->colourNames[0], 0);
    if (!layout->luminance) {
        insert(layout->colourNames[1], 1);
        insert(layout->colourNames[2], 2);
    }
    if (layout->hasAlpha)
        insert(layout->colourNames[3], 3);
    if (extras)
        insertExrChannelSlices(frameBuffer, *extras, *layout);
    // Rows [first, first + count) of the data window through `part`, then their repair count.
    // `stop` is polled between reads; false means the band stopped before its end.
    const auto readBand = [&](Imf::InputPart& part, const std::uint64_t first,
                              const std::uint64_t count, const auto& stop,
                              std::uint64_t& repaired) {
        part.setFrameBuffer(frameBuffer);
        for (std::uint64_t done = 0; done < count;) {
            if (stop())
                return false;
            const auto rows = std::min(chunkRows, count - done);
            const auto firstY = static_cast<std::int64_t>(layout->dataWindow.min.y) +
                                static_cast<std::int64_t>(first + done);
            part.readPixels(static_cast<int>(firstY),
                            static_cast<int>(firstY + static_cast<std::int64_t>(rows) - 1));
            done += rows;
        }
        auto bandLayout = decoded.layout;
        bandLayout.height = count;
        repaired = countExrHalfRepairs(
            std::span<const std::byte>(*storage).subspan(static_cast<std::size_t>(first) * rowStride,
                                                         static_cast<std::size_t>(count) *
                                                             rowStride),
            bandLayout, interpretation.storedSamples);
        return true;
    };
    Imf::InputPart part(input, *index);
    const auto chunks = (height + chunkHeight - 1) / chunkHeight;
    const auto wanted = static_cast<unsigned>(std::min<std::uint64_t>(
        kMaximumExrBands, std::max<std::uint64_t>(1U, chunks / kMinimumChunksPerExrBand)));
    const auto extra = wanted > 1 ? reserveExrBandThreads(wanted - 1) : 0U;
    struct Reservation final {
        unsigned count;
        ~Reservation() { releaseExrBandThreads(count); }
    } reservation{extra};
    const auto bands = static_cast<std::uint64_t>(extra) + 1U;
    // Whole chunks per band, the remainder spread over the first bands.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (std::uint64_t band = 0, chunk = 0; band < bands; ++band) {
        const auto take = chunks / bands + (band < chunks % bands ? 1U : 0U);
        const auto first = chunk * chunkHeight;
        const auto last = std::min(height, (chunk + take) * chunkHeight);
        ranges.emplace_back(first, last - first);
        chunk += take;
    }
    std::atomic<bool> stopBands{false};
    std::mutex bandMutex;
    std::condition_variable bandDone;
    std::size_t bandsRunning = ranges.size() - 1U;
    std::vector<std::optional<ImageDiagnosticCode>> bandErrors(ranges.size());
    std::vector<std::uint64_t> bandRepairs(ranges.size(), 0U);
    std::vector<std::jthread> workers;
    workers.reserve(ranges.size() - 1U);
    const auto stopRequested = [&stopBands] { return stopBands.load(std::memory_order_relaxed); };
    for (std::size_t band = 1; band < ranges.size(); ++band)
        workers.emplace_back([&, band] {
            try {
                RevisionStream stream(bytes);
                Imf::MultiPartInputFile file(stream, 1);
                Imf::InputPart own(file, *index);
                if (!readBand(own, ranges[band].first, ranges[band].second, stopRequested,
                              bandRepairs[band]))
                    bandErrors[band] = ImageDiagnosticCode::Cancelled;
            } catch (const std::bad_alloc&) {
                bandErrors[band] = ImageDiagnosticCode::AllocationFailure;
            } catch (...) {
                bandErrors[band] = ImageDiagnosticCode::Truncated;
            }
            if (bandErrors[band])
                stopBands.store(true, std::memory_order_relaxed);
            const std::lock_guard lock(bandMutex);
            --bandsRunning;
            bandDone.notify_all();
        });
    // This thread reads the first band and alone consults the caller's cancellation callback.
    const auto stopFirst = [&] {
        if (cancelled(cancel))
            stopBands.store(true, std::memory_order_relaxed);
        return stopRequested();
    };
    std::optional<ImageDiagnosticCode> firstError;
    try {
        if (!readBand(part, ranges[0].first, ranges[0].second, stopFirst, bandRepairs[0]))
            firstError = ImageDiagnosticCode::Cancelled;
    } catch (...) {
        stopBands.store(true, std::memory_order_relaxed);
        std::unique_lock lock(bandMutex);
        bandDone.wait(lock, [&] { return bandsRunning == 0; });
        throw;
    }
    {
        std::unique_lock lock(bandMutex);
        while (!bandDone.wait_for(lock, std::chrono::milliseconds(5),
                                  [&] { return bandsRunning == 0; }))
            if (cancelled(cancel))
                stopBands.store(true, std::memory_order_relaxed);
    }
    workers.clear();
    if (cancelled(cancel))
        return {{}, "Image decode cancelled", true, ImageDiagnosticCode::Cancelled};
    for (const auto& bandError : bandErrors)
        if (bandError && *bandError != ImageDiagnosticCode::Cancelled)
            firstError = bandError;
    if (firstError == ImageDiagnosticCode::AllocationFailure)
        return failure<Decoded>(ImageDiagnosticCode::AllocationFailure,
                                "OpenEXR decode allocation failed");
    if (firstError == ImageDiagnosticCode::Truncated)
        return failure<Decoded>(ImageDiagnosticCode::Truncated,
                                "OpenEXR revision pixel data could not be decoded");
    if (firstError)
        return {{}, "Image decode cancelled", true, ImageDiagnosticCode::Cancelled};
    if (!part.isComplete())
        return failure<Decoded>(ImageDiagnosticCode::Truncated, "OpenEXR pixel data is truncated");
    for (const auto repaired : bandRepairs)
        decoded.repairedPixels += repaired;
    decoded.bytes = std::move(storage);
    publishAlongside(alongside, extras, *layout);
    return {std::move(decoded), {}};
}

ImageResult<ExrHalfSamples> decodeExrHalfSamples(const std::span<const std::byte> bytes,
                                                 const ImageInterpretation& interpretation,
                                                 const CancelImageWork& cancel,
                                                 const std::size_t budget,
                                                 ImageChannelsAlongside* alongside) {
    try {
        RevisionStream stream(bytes);
        Imf::MultiPartInputFile input(stream, 1);
        return decodeExrHalfParts(input, bytes, interpretation, cancel, budget, alongside);
    } catch (const std::bad_alloc&) {
        return failure<ExrHalfSamples>(ImageDiagnosticCode::AllocationFailure,
                                       "OpenEXR decode allocation failed");
    } catch (...) {
        return failure<ExrHalfSamples>(ImageDiagnosticCode::Truncated,
                                       "OpenEXR revision pixel data could not be decoded");
    }
}

std::uint64_t countExrHalfRepairs(const std::span<const std::byte> bytes,
                                  const ExrHalfLayout& layout, const bool storedSamples) noexcept {
    const auto half = [&](const std::size_t offset) {
        return static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset]) |
                                          (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U));
    };
    const auto nonFinite = [](const std::uint32_t bits) { return (bits & 0x7C00U) == 0x7C00U; };
    std::uint64_t repaired = 0;
    const auto pixels = static_cast<std::size_t>(layout.width) * static_cast<std::size_t>(layout.height);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const auto offset = pixel * 8U;
        std::array<std::uint32_t, 4> lanes{half(offset), half(offset + 2), half(offset + 4),
                                           layout.hasAlpha ? half(offset + 6) : 0x3C00U};
        if (layout.luminance)
            lanes[1] = lanes[2] = lanes[0];
        bool flagged = false;
        for (auto& lane : lanes)
            if (nonFinite(lane)) {
                lane = 0;
                flagged = true;
            }
        const auto alpha = lanes[3];
        const bool negative = (alpha & 0x8000U) != 0 && (alpha & 0x7FFFU) != 0;
        const bool aboveOne = (alpha & 0x8000U) == 0 && alpha > 0x3C00U;
        flagged = flagged || negative || aboveOne;
        const bool zeroAlpha = negative || (alpha & 0x7FFFU) == 0;
        // Premultiplied RGB at zero alpha (additive emission) is dropped and reported.
        if (!storedSamples && layout.premultiplied && zeroAlpha &&
            ((lanes[0] & 0x7FFFU) != 0 || (lanes[1] & 0x7FFFU) != 0 || (lanes[2] & 0x7FFFU) != 0))
            flagged = true;
        if (flagged)
            ++repaired;
    }
    return repaired;
}

ImageResult<ImageProbe> probeExr(const std::span<const std::byte> bytes,
                                 const core::Sha256Digest digest, const CancelImageWork& cancel) {
    if (cancelled(cancel))
        return {{}, "Image probe cancelled", true, ImageDiagnosticCode::Cancelled};
    try {
        if (bytes.size() < 8)
            return failure<ImageProbe>(ImageDiagnosticCode::Truncated,
                                       "OpenEXR revision version field is truncated");
        std::uint32_t encodedVersion = 0;
        for (std::size_t index = 0; index < 4; ++index)
            encodedVersion |= static_cast<std::uint32_t>(bytes[index + 4]) << (8U * index);
        if (const auto error = validateVersion(encodedVersion))
            return failure<ImageProbe>(*error, "OpenEXR version or flags are unsupported");
        RevisionStream stream(bytes);
        const Imf::MultiPartInputFile input(stream, 1);
        return probeParts(input, digest, cancel);
    } catch (const std::bad_alloc&) {
        return failure<ImageProbe>(ImageDiagnosticCode::AllocationFailure,
                                   "OpenEXR probe allocation failed");
    } catch (...) {
        return failure<ImageProbe>(ImageDiagnosticCode::InvalidHeader,
                                   "OpenEXR revision header could not be read");
    }
}

ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(const std::filesystem::path& path, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress, const std::size_t budget) {
    try {
        Imf::MultiPartInputFile input(utf8Path(path).c_str(), 1);
        return decodeExrParts(input, interpretation, std::move(processor), cancel, progress,
                              budget);
    } catch (...) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::Truncated, "OpenEXR pixel data could not be decoded");
    }
}

ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(const std::span<const std::byte> bytes, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress, const std::size_t budget,
          ImageChannelsAlongside* alongside) {
    try {
        RevisionStream stream(bytes);
        Imf::MultiPartInputFile input(stream, 1);
        return decodeExrParts(input, interpretation, std::move(processor), cancel, progress, budget,
                              alongside);
    } catch (...) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::Truncated, "OpenEXR revision pixel data could not be decoded");
    }
}

ImageResult<std::shared_ptr<const render::ChannelSetImage>>
decodeExrChannels(const std::span<const std::byte> bytes, const ImageChannelSelection& selection,
                  const CancelImageWork& cancel, const ImageProgress& progress,
                  const std::size_t budget) {
    using Decoded = std::shared_ptr<const render::ChannelSetImage>;
    if (cancelled(cancel))
        return {{}, "Image decode cancelled", true, ImageDiagnosticCode::Cancelled};
    try {
        RevisionStream stream(bytes);
        Imf::MultiPartInputFile input(stream, 1);
        if (const auto error = validateVersion(static_cast<std::uint32_t>(input.version())))
            return failure<Decoded>(*error, "OpenEXR version or flags are unsupported");
        const auto index = findPartIndex(input, selection.part);
        if (!index)
            return failure<Decoded>(ImageDiagnosticCode::PartNotFound,
                                    "The selected OpenEXR part is not present");
        const auto& header = input.header(*index);
        ImageDiagnosticCode error = ImageDiagnosticCode::InvalidHeader;
        const auto layout = inspectHeader(header, {}, error);
        if (!layout)
            return failure<Decoded>(error, "OpenEXR header is unsupported or invalid");
        auto selected = selectExrChannels(header, *layout, selection.channels);
        if (!selected.value)
            return {{}, selected.diagnostic, false, selected.code};
        // Admission: every selected plane (channels x pixels x 4) and the part's string metadata.
        // The planes are the read's own slices, so it stages no scratch rows.
        if (!decodeWorkingSetFits(selected.value->bytes, 0U, 0U, budget))
            return failure<Decoded>(ImageDiagnosticCode::PixelBudgetExceeded,
                                    "Decoded channel set exceeds the pixel working-set budget");
        auto& planes = *selected.value;
        allocateExrChannelPlanes(planes, *layout);
        Imf::FrameBuffer frameBuffer;
        insertExrChannelSlices(frameBuffer, planes, *layout);
        const auto width = layout->width;
        const auto height = layout->height;
        // Reads of about a megabyte of planes (a tile row when tiled), polling cancellation.
        constexpr std::uint64_t kBandTargetBytes = std::uint64_t{1024} * 1024U;
        const auto bytesPerRow = std::max<std::uint64_t>(
            1U, width * std::max<std::size_t>(1U, planes.selected.size()) * sizeof(float));
        const auto chunkRows = std::min(
            height, layout->tiled ? std::max<std::uint64_t>(1U, layout->tileHeight)
                                  : std::max<std::uint64_t>(1U, kBandTargetBytes / bytesPerRow));
        Imf::InputPart part(input, *index);
        part.setFrameBuffer(frameBuffer);
        std::uint64_t completed = 0;
        if (progress)
            progress(0, height);
        while (completed < height) {
            if (cancelled(cancel))
                return {{}, "Image decode cancelled", true, ImageDiagnosticCode::Cancelled};
            const auto first = layout->tiled ? (completed / chunkRows) * chunkRows : completed;
            const auto rows = std::min(chunkRows, height - first);
            const auto firstY = static_cast<std::int64_t>(layout->dataWindow.min.y) +
                                static_cast<std::int64_t>(first);
            part.readPixels(static_cast<int>(firstY),
                            static_cast<int>(firstY) + static_cast<int>(rows) - 1);
            completed = first + rows;
            if (progress)
                progress(completed, height);
        }
        if (!part.isComplete())
            return failure<Decoded>(ImageDiagnosticCode::Truncated,
                                    "OpenEXR pixel data is truncated");
        return finishExrChannelPlanes(std::move(planes), *layout, selection.inputColorSpaceId);
    } catch (const std::bad_alloc&) {
        return failure<Decoded>(ImageDiagnosticCode::AllocationFailure,
                                "OpenEXR channel decode allocation failed");
    } catch (...) {
        return failure<Decoded>(ImageDiagnosticCode::Truncated,
                                "OpenEXR channel data could not be decoded");
    }
}

} // namespace bloom::media::detail
