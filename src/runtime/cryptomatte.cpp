#include "cryptomatte.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace bloom::runtime::detail {
namespace {

// A strict reader for the manifest: one JSON object of string keys and string values.
class ManifestReader final {
  public:
    explicit ManifestReader(std::string_view text) : text_(text) {}

    bool read(std::vector<std::pair<std::string, std::string>>& out) {
        skip();
        if (!take('{'))
            return false;
        skip();
        if (take('}'))
            return end();
        for (;;) {
            std::string key;
            std::string value;
            skip();
            if (!string(key))
                return false;
            skip();
            if (!take(':'))
                return false;
            skip();
            if (!string(value))
                return false;
            out.emplace_back(std::move(key), std::move(value));
            skip();
            if (take(','))
                continue;
            if (take('}'))
                return end();
            return false;
        }
    }

  private:
    bool end() {
        skip();
        return position_ == text_.size();
    }
    void skip() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\n' ||
                                            text_[position_] == '\r' || text_[position_] == '\t'))
            ++position_;
    }
    bool take(const char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }
    static void appendUtf8(std::string& out, const std::uint32_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    bool hex4(std::uint32_t& value) {
        if (text_.size() - position_ < 4)
            return false;
        const auto* first = text_.data() + position_;
        const auto parsed = std::from_chars(first, first + 4, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != first + 4)
            return false;
        position_ += 4;
        return true;
    }
    bool string(std::string& out) {
        if (!take('"'))
            return false;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size())
                return false;
            const char escape = text_[position_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out.push_back(escape);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::uint32_t code = 0;
                if (!hex4(code))
                    return false;
                if (code >= 0xD800 && code < 0xDC00) {
                    std::uint32_t low = 0;
                    if (!take('\\') || !take('u') || !hex4(low) || low < 0xDC00 || low >= 0xE000)
                        return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

[[nodiscard]] std::optional<int> rankSlot(std::string_view suffix) noexcept {
    std::string lower(suffix);
    std::ranges::transform(lower, lower.begin(), [](const unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    if (lower == "r" || lower == "red")
        return 0;
    if (lower == "g" || lower == "green")
        return 1;
    if (lower == "b" || lower == "blue")
        return 2;
    if (lower == "a" || lower == "alpha")
        return 3;
    return std::nullopt;
}

// `<name>NN` -> (name, NN).
[[nodiscard]] std::optional<std::pair<std::string_view, int>>
rankLayer(const std::string_view layer) noexcept {
    if (layer.size() < 3)
        return std::nullopt;
    const auto digits = layer.substr(layer.size() - 2);
    if (digits[0] < '0' || digits[0] > '9' || digits[1] < '0' || digits[1] > '9')
        return std::nullopt;
    return std::pair{layer.substr(0, layer.size() - 2), (digits[0] - '0') * 10 + (digits[1] - '0')};
}

} // namespace

CryptomatteManifest cryptomatteManifest(const render::ChannelMetadata& metadata,
                                        const std::string_view layer) {
    CryptomatteManifest result;
    std::string key;
    for (const auto& [name, value] : metadata)
        if (name.starts_with("cryptomatte/") && name.ends_with("/name") && value == layer) {
            key = name.substr(0, name.size() - std::string_view("name").size());
            break;
        }
    if (key.empty())
        return result;
    const auto manifest = std::ranges::find_if(
        metadata, [&](const auto& entry) { return entry.first == key + "manifest"; });
    if (manifest == metadata.end()) {
        if (std::ranges::any_of(
                metadata, [&](const auto& entry) { return entry.first == key + "manif_file"; }))
            result.diagnostic = "The Cryptomatte manifest is a sidecar file; names resolve by hash";
        return result;
    }
    result.text = manifest->second;
    std::vector<std::pair<std::string, std::string>> raw;
    if (!ManifestReader(manifest->second).read(raw)) {
        result.diagnostic = "The Cryptomatte manifest is not a valid JSON object";
        return result;
    }
    result.present = true;
    for (auto& [name, hex] : raw) {
        std::uint32_t id = 0;
        const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != hex.data() + hex.size() || hex.empty()) {
            result.diagnostic = "A Cryptomatte manifest ID is not hexadecimal";
            continue;
        }
        result.entries.emplace_back(std::move(name), id);
    }
    return result;
}

std::optional<std::string> cryptomatteManifestName(const CryptomatteManifest& manifest,
                                                   const std::uint32_t id) {
    for (const auto& [name, entry] : manifest.entries)
        if (entry == id)
            return name;
    return std::nullopt;
}

std::vector<std::uint32_t> resolveCryptomatteIds(const std::span<const std::string> mattes,
                                                 const CryptomatteManifest& manifest) {
    std::vector<std::uint32_t> ids;
    for (const auto& matte : mattes) {
        std::uint32_t id = 0;
        if (const auto raw = parseCryptomatteRawId(matte)) {
            id = *raw;
        } else if (const auto found = std::ranges::find_if(
                       manifest.entries, [&](const auto& entry) { return entry.first == matte; });
                   found != manifest.entries.end()) {
            id = found->second;
        } else {
            id = cryptomatteNameId(matte);
        }
        if (std::ranges::find(ids, id) == ids.end())
            ids.push_back(id);
    }
    return ids;
}

std::vector<CryptomatteRank> cryptomatteRanks(const std::span<const ResolvedChannel> channels,
                                              const std::string_view layer) {
    // ranks[NN][slot] -> channel name
    std::vector<std::array<std::string, 4>> ranks;
    for (const auto& channel : channels) {
        const auto parsed = rankLayer(channel.descriptor.layer);
        if (!parsed || parsed->first != layer)
            continue;
        const auto suffix =
            std::string_view(channel.name).substr(channel.descriptor.layer.size() + 1);
        const auto slot = rankSlot(suffix);
        if (!slot)
            continue;
        if (ranks.size() <= static_cast<std::size_t>(parsed->second))
            ranks.resize(static_cast<std::size_t>(parsed->second) + 1);
        ranks[static_cast<std::size_t>(parsed->second)][static_cast<std::size_t>(*slot)] =
            channel.name;
    }
    std::vector<CryptomatteRank> pairs;
    for (const auto& rank : ranks) {
        if (!rank[0].empty() && !rank[1].empty())
            pairs.push_back({rank[0], rank[1]});
        if (!rank[2].empty() && !rank[3].empty())
            pairs.push_back({rank[2], rank[3]});
    }
    return pairs;
}

std::vector<std::string> cryptomatteLayers(const std::span<const ResolvedChannel> channels) {
    std::vector<std::string> layers;
    for (const auto& channel : channels) {
        const auto parsed = rankLayer(channel.descriptor.layer);
        if (!parsed)
            continue;
        std::string name(parsed->first);
        if (std::ranges::find(layers, name) == layers.end() &&
            !cryptomatteRanks(channels, name).empty())
            layers.push_back(std::move(name));
    }
    return layers;
}

std::string resolveCryptomatteLayer(const std::span<const ResolvedChannel> channels,
                                    const std::string_view layer) {
    if (!layer.empty())
        return std::string(layer);
    auto layers = cryptomatteLayers(channels);
    return layers.empty() ? std::string{} : std::move(layers.front());
}

float accumulateCryptomatteCoverage(const float previous, const float idBits, const float coverage,
                                    const std::span<const std::uint32_t> ids) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(idBits);
    const bool match = std::ranges::find(ids, bits) != ids.end();
    const float sum = previous + (match ? coverage : 0.0F);
    return std::clamp(sum, 0.0F, 1.0F);
}

} // namespace bloom::runtime::detail

#include "operation_key.hpp"

namespace bloom::runtime::detail {

CryptomatteSelection
selectCryptomatte(const CompiledCryptomatte& cryptomatte, const core::RationalTime time,
                  const document::FrameRate rate, const std::filesystem::path& base,
                  const CancellationToken& cancel, const EvaluationColorIntent& colorIntent,
                  OperationCache* revisionCache, const MediaSourceRevisionPolicy revisionPolicy,
                  const std::size_t hostByteBudget, const bool allowRevisionInsertion,
                  LinearPassRetention* const linearPass, const ChannelSourceClock& clock) {
    CryptomatteSelection result;
    result.layer = cryptomatte.layer;
    result.mattes = cryptomatte.mattes;
    result.preview = cryptomatte.preview;
    result.channels = selectChannelSetSources(cryptomatte.channels, time, rate, base, cancel,
                                              colorIntent, revisionCache, revisionPolicy,
                                              hostByteBudget, allowRevisionInsertion, linearPass,
                                              clock);
    result.cancelled = result.channels.cancelled;
    result.budgetExceeded = result.channels.budgetExceeded;
    OperationKey key;
    key.add(std::string{"cryptomatte-v1"});
    for (const auto& source : result.channels.sources)
        key.add(source.cacheKey);
    key.add(cryptomatte.layer);
    if (cryptomatte.preview)
        key.add(std::string{"preview-v1"});
    key.add(cryptomatte.mattes.size());
    for (const auto& matte : cryptomatte.mattes)
        key.add(matte);
    result.cacheKey = key.bytes();
    if (!result.channels.available()) {
        result.warning = result.channels.warning;
        return result;
    }
    result.layer = resolveCryptomatteLayer(result.channels.resolution.channels, cryptomatte.layer);
    result.ranks = cryptomatteRanks(result.channels.resolution.channels, result.layer);
    if (result.ranks.empty()) {
        result.warning =
            cryptomatte.layer.empty()
                ? std::string("The channel set has no Cryptomatte layer")
                : "The channel set has no Cryptomatte layer named " + cryptomatte.layer;
        return result;
    }
    const auto& channels = result.channels.resolution.channels;
    const auto find = [&](const std::string& name) {
        return std::ranges::find_if(
            channels, [&](const ResolvedChannel& channel) { return channel.name == name; });
    };
    OperationKey gatherKey;
    gatherKey.add(std::string{"cryptomatte-rank-layers-v2"});
    for (const auto& source : result.channels.sources)
        gatherKey.add(source.decodeKey);
    for (const auto& rank : result.ranks) {
        for (const auto* name : {&rank.id, &rank.coverage}) {
            const auto channel = find(*name);
            // #480: ID hashes are float bit patterns that only a file keeps exact.
            if (channel->source && *channel->source < cryptomatte.channels.sources.size() &&
                cryptomatte.channels.sources[*channel->source].imageBacked) {
                result.warning = "Cryptomatte reads its ID ranks from file channels, not from the "
                                 "Image to Channel Set channel " +
                                 channel->name;
                return result;
            }
            result.rankChannels.push_back(*channel);
            gatherKey.add(channel->source.has_value());
            gatherKey.add(channel->source.value_or(0U));
            gatherKey.add(channel->sourceChannel);
            gatherKey.add(static_cast<double>(channel->constant));
        }
    }
    for (const auto& channel : result.rankChannels)
        if (channel.source) {
            result.anchor = *channel.source;
            break;
        }
    result.gatherKey = gatherKey.digest();
    result.available = true;
    return result;
}

CryptomatteGather gatherCryptomatte(const CryptomatteSelection& selection, const std::size_t budget,
                                    const CancellationToken& cancel, const ChannelSetReader& read) {
    CryptomatteGather result;
    if (!selection.available) {
        result.diagnostic = selection.warning;
        return result;
    }
    const auto cancelled = [&cancel] { return cancel.isCancellationRequested(); };
    const auto& sources = selection.channels.sources;
    std::vector<std::vector<std::string>> names(sources.size());
    for (const auto& channel : selection.rankChannels)
        if (channel.source && std::ranges::find(names[*channel.source], channel.sourceChannel) ==
                                  names[*channel.source].end())
            names[*channel.source].push_back(channel.sourceChannel);
    std::vector<std::shared_ptr<const render::ChannelSetImage>> sets(sources.size());
    std::size_t remaining = budget;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (names[index].empty())
            continue;
        auto decoded = read ? read(sources[index], names[index], remaining, cancel)
                            : readSourceChannels(sources[index], names[index], remaining, cancel);
        if (!decoded.value.has_value()) {
            result.cancelled = decoded.cancelled;
            result.diagnostic = decoded.diagnostic;
            result.code = decoded.code;
            return result;
        }
        const auto cost = (*decoded.value)->byteCost();
        remaining = cost < remaining ? remaining - cost : 0;
        sets[index] = std::move(*decoded.value);
    }
    // The manifest travels in the metadata of the part holding the rank channels.
    if (sets[selection.anchor])
        result.manifest = cryptomatteManifest(sets[selection.anchor]->metadata(), selection.layer);
    result.ids = resolveCryptomatteIds(selection.mattes, result.manifest);
    const auto* anchorPart = selection.channels.parts[selection.anchor];
    const auto display =
        render::ImageWindow::create(anchorPart->displayX, anchorPart->displayY,
                                    anchorPart->displayWidth, anchorPart->displayHeight);
    std::optional<render::ImageWindow> union_;
    for (const auto& set : sets) {
        if (!set)
            continue;
        const auto window = set->dataWindow();
        if (!union_) {
            union_ = window;
            continue;
        }
        const auto left = std::min(union_->originX(), window.originX());
        const auto top = std::min(union_->originY(), window.originY());
        const auto right = std::max(union_->maxXExclusive(), window.maxXExclusive());
        const auto bottom = std::max(union_->maxYExclusive(), window.maxYExclusive());
        const auto merged =
            render::ImageWindow::create(left, top, static_cast<std::uint64_t>(right - left),
                                        static_cast<std::uint64_t>(bottom - top));
        if (!merged) {
            result.diagnostic = "The Cryptomatte windows cannot be combined";
            return result;
        }
        union_ = *merged.value();
    }
    if (!display || !union_) {
        result.diagnostic = "The Cryptomatte frame is invalid";
        return result;
    }
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *union_, *display.value(), core::PixelAspectRatio::square());
    if (!descriptor) {
        result.diagnostic = "The Cryptomatte frame is invalid";
        return result;
    }
    const auto window = *union_;
    const auto width = static_cast<std::size_t>(window.extent().width());
    const auto height = static_cast<std::size_t>(window.extent().height());
    const auto pixels = width * height;
    // One plane on the union window: the channel's samples where its own window covers the pixel,
    // zero elsewhere, or the channel's constant.
    const auto fill = [&](const ResolvedChannel& channel, std::vector<float>& plane) {
        plane.assign(pixels, 0.0F);
        if (!channel.source) {
            std::ranges::fill(plane, channel.constant);
            return true;
        }
        const auto& set = sets[*channel.source];
        const auto index = set ? set->find(channel.sourceChannel) : std::nullopt;
        if (!index)
            return true;
        const auto source = set->plane(*index);
        const auto channelWindow = set->dataWindow();
        const auto left = std::max(window.originX(), channelWindow.originX());
        const auto right = std::min(window.maxXExclusive(), channelWindow.maxXExclusive());
        const auto top = std::max(window.originY(), channelWindow.originY());
        const auto bottom = std::min(window.maxYExclusive(), channelWindow.maxYExclusive());
        const auto channelWidth = static_cast<std::size_t>(channelWindow.extent().width());
        for (auto y = top; y < bottom && left < right; ++y) {
            if (cancelled())
                return false;
            const auto from = static_cast<std::size_t>(y - channelWindow.originY()) * channelWidth +
                              static_cast<std::size_t>(left - channelWindow.originX());
            const auto to = static_cast<std::size_t>(y - window.originY()) * width +
                            static_cast<std::size_t>(left - window.originX());
            std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(from),
                        static_cast<std::size_t>(right - left),
                        plane.begin() + static_cast<std::ptrdiff_t>(to));
        }
        return true;
    };
    const auto rankCount = selection.rankChannels.size() / 2;
    for (std::size_t first = 0; first < rankCount; first += 2) {
        const auto ranks = std::min<std::size_t>(2, rankCount - first);
        std::vector<render::ChannelDescriptor> descriptors;
        std::vector<std::vector<float>> planes(ranks * 2);
        for (std::size_t rank = 0; rank < ranks; ++rank) {
            const auto& idChannel = selection.rankChannels[(first + rank) * 2];
            const auto& coverageChannel = selection.rankChannels[(first + rank) * 2 + 1];
            auto& id = planes[rank * 2];
            auto& coverage = planes[rank * 2 + 1];
            if (!fill(idChannel, id) || !fill(coverageChannel, coverage)) {
                result.cancelled = true;
                return result;
            }
            for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                if (!std::isfinite(id[pixel]) || !std::isfinite(coverage[pixel])) {
                    id[pixel] = std::isfinite(id[pixel]) ? id[pixel] : 0.0F;
                    coverage[pixel] = std::isfinite(coverage[pixel]) ? coverage[pixel] : 0.0F;
                    ++result.repairedPixels;
                }
            }
            descriptors.push_back({idChannel.name, selection.layer, render::ChannelRole::Id,
                                   render::ChannelSampleKind::Float,
                                   render::ChannelValueEncoding::Data});
            descriptors.push_back({coverageChannel.name, selection.layer,
                                   render::ChannelRole::Coverage, render::ChannelSampleKind::Float,
                                   render::ChannelValueEncoding::Data});
        }
        auto layer = render::ChannelSetImage::create(window, *display.value(),
                                                     core::PixelAspectRatio::square(),
                                                     std::move(descriptors), std::move(planes), {},
                                                     remaining);
        if (!layer) {
            result.diagnostic = "The Cryptomatte ranks exceed the pixel storage budget";
            result.code = media::ImageDiagnosticCode::PixelBudgetExceeded;
            return result;
        }
        const auto cost = layer.value()->byteCost();
        remaining = cost < remaining ? remaining - cost : 0;
        result.layers.push_back(
            std::make_shared<const render::ChannelSetImage>(std::move(*layer.value())));
    }
    result.rankCount = static_cast<std::uint32_t>(rankCount);
    return result;
}

std::pair<float, float> CryptomatteGather::rankPair(const std::uint32_t rank,
                                                    const std::size_t pixel) const noexcept {
    const auto& layer = *layers[rank / 2];
    const auto lane = static_cast<std::size_t>(rank % 2) * 2;
    return {layer.plane(lane)[pixel], layer.plane(lane + 1)[pixel]};
}

media::ImageResult<render::Rgba32fImage> cryptomatteCoverageImage(const CryptomatteGather& gather,
                                                                  const std::size_t budget,
                                                                  const CancellationToken& cancel) {
    if (gather.layers.empty())
        return {{}, gather.diagnostic.empty() ? "The Cryptomatte has no ranks" : gather.diagnostic};
    const auto& first = *gather.layers.front();
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        first.dataWindow(), first.displayWindow(), first.pixelAspect());
    if (!descriptor)
        return {{}, "Cryptomatte matte validation failed"};
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return {{},
                "The Cryptomatte matte exceeds the pixel storage budget",
                false,
                media::ImageDiagnosticCode::PixelBudgetExceeded};
    const auto window = first.dataWindow();
    const auto width = static_cast<std::size_t>(window.extent().width());
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        if (cancel.isCancellationRequested())
            return {{}, {}, true, media::ImageDiagnosticCode::Cancelled};
        auto row = builder.value()->row(y);
        const auto offset = static_cast<std::size_t>(y - window.originY()) * width;
        for (std::size_t column = 0; column < width; ++column) {
            float coverage = 0.0F;
            for (std::uint32_t rank = 0; rank < gather.rankCount; ++rank) {
                const auto [id, value] = gather.rankPair(rank, offset + column);
                coverage = accumulateCryptomatteCoverage(coverage, id, value, gather.ids);
            }
            const auto pixel =
                render::Rgba32f::fromPremultiplied(coverage, coverage, coverage, coverage);
            (*row.value())[column] = *pixel.value();
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return {{}, "Cryptomatte matte validation failed"};
    return {std::move(*frozen.value()), {}};
}

media::ImageResult<render::Rgba32fImage>
cryptomattePreviewImage(const CryptomatteGather& gather, const std::size_t budget,
                        const CancellationToken& cancel) {
    if (gather.layers.empty())
        return {{}, gather.diagnostic.empty() ? "The Cryptomatte has no ranks" : gather.diagnostic};
    const auto& first = *gather.layers.front();
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        first.dataWindow(), first.displayWindow(), first.pixelAspect());
    if (!descriptor)
        return {{}, "Cryptomatte preview validation failed"};
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return {{}, "The Cryptomatte preview exceeds the pixel storage budget", false,
                media::ImageDiagnosticCode::PixelBudgetExceeded};
    const auto window = first.dataWindow();
    const auto width = static_cast<std::size_t>(window.extent().width());
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        if (cancel.isCancellationRequested())
            return {{}, {}, true, media::ImageDiagnosticCode::Cancelled};
        auto row = builder.value()->row(y);
        const auto offset = static_cast<std::size_t>(y - window.originY()) * width;
        for (std::size_t column = 0; column < width; ++column) {
            std::array<float, 3> colour{};
            float alpha = 0.0F;
            for (std::uint32_t rank = 0; rank < gather.rankCount; ++rank) {
                const auto [id, coverage] = gather.rankPair(rank, offset + column);
                const auto bits = std::bit_cast<std::uint32_t>(id);
                auto tint = cryptomatteIdColour(bits);
                if (std::ranges::find(gather.ids, bits) != gather.ids.end())
                    for (auto& channel : tint)
                        channel = channel * 0.5F + 0.5F;
                for (std::size_t k = 0; k < 3; ++k) {
                    const float term = tint[k] * coverage;
                    colour[k] = colour[k] + term;
                }
                const float sum = alpha + coverage;
                alpha = std::clamp(sum, 0.0F, 1.0F);
            }
            const auto pixel = render::Rgba32f::fromPremultiplied(
                std::min(colour[0], alpha), std::min(colour[1], alpha), std::min(colour[2], alpha),
                alpha);
            (*row.value())[column] = *pixel.value();
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return {{}, "Cryptomatte preview validation failed"};
    return {std::move(*frozen.value()), {}};
}

media::ImageResult<render::Rgba32fImage>
evaluateCryptomatte(const CryptomatteSelection& selection,
                    const render::Rgba32fImageDescriptor composition, const double horizontalScale,
                    const double verticalScale, const std::size_t budget,
                    const CancellationToken& cancel) {
    const auto gather = gatherCryptomatte(selection, budget, cancel);
    if (gather.cancelled)
        return {{}, {}, true, media::ImageDiagnosticCode::Cancelled};
    if (gather.layers.empty())
        return {{}, gather.diagnostic, false, gather.code};
    auto coverage = selection.preview ? cryptomattePreviewImage(gather, budget, cancel)
                                      : cryptomatteCoverageImage(gather, budget, cancel);
    if (!coverage.value.has_value())
        return coverage;
    auto resampled = resampleDecodedImage(*coverage.value, composition, horizontalScale,
                                          verticalScale, budget, cancel);
    resampled.repairedPixels = gather.repairedPixels;
    // A successful matte carries the manifest's diagnostic (sidecar or malformed manifest).
    if (resampled.value.has_value())
        resampled.diagnostic = gather.manifest.diagnostic;
    return resampled;
}

std::optional<ContentBounds>
cryptomatteFrameBounds(const CryptomatteSelection& selection) noexcept {
    if (!selection.available || selection.anchor >= selection.channels.sources.size())
        return std::nullopt;
    return imageSourceFrameBounds(selection.channels.sources[selection.anchor]);
}

} // namespace bloom::runtime::detail
