#include "channel_static.hpp"

#include "channel_resolution.hpp"

#include <bloom/runtime/channel_inspection_listing.hpp>

#include <algorithm>

namespace bloom::runtime::detail {
namespace {

// Whether a file channel named `name` could match `pattern`: its role depends only on its name
// and sample kind, so every kind a probe may report is tried.
[[nodiscard]] bool fileMayMatch(const std::string_view pattern, const std::string& name) {
    for (const auto kind : {render::ChannelSampleKind::Float, render::ChannelSampleKind::Uint})
        if (matchesChannelPattern(pattern, render::describeChannel(name, kind)))
            return true;
    return false;
}

[[nodiscard]] StaticChannel constantChannel(const std::string_view slot) {
    StaticChannel constant;
    constant.name = std::string(slot);
    constant.descriptor = render::describeChannel(slot, render::ChannelSampleKind::Float);
    constant.kind = StaticChannelKind::Constant;
    constant.constant = slot == "1" ? 1.0F : 0.0F;
    return constant;
}

[[nodiscard]] StaticChannel absentChannel(const std::string_view slot) {
    StaticChannel absent;
    absent.name = std::string(slot);
    absent.kind = StaticChannelKind::Absent;
    return absent;
}

} // namespace

std::vector<render::ChannelDescriptor>
imageChannelSetDescriptors(const std::string_view layer, const std::span<const std::string> names,
                           const bool colourEncoding) {
    constexpr std::array kColourRoles{render::ChannelRole::ColorR, render::ChannelRole::ColorG,
                                      render::ChannelRole::ColorB, render::ChannelRole::Alpha};
    std::vector<render::ChannelDescriptor> descriptors;
    for (std::size_t lane = 0; lane < names.size() && lane < 4; ++lane) {
        const auto full = layer.empty() ? names[lane] : std::string(layer) + "." + names[lane];
        auto descriptor = render::describeChannel(full, render::ChannelSampleKind::Float);
        if (colourEncoding) {
            descriptor.role = kColourRoles[lane];
            descriptor.encoding = lane < 3 ? render::ChannelValueEncoding::Colour
                                           : render::ChannelValueEncoding::Data;
        } else {
            if (render::isColourRole(descriptor.role))
                descriptor.role = render::ChannelRole::Data;
            descriptor.encoding = render::ChannelValueEncoding::Data;
        }
        descriptors.push_back(std::move(descriptor));
    }
    return descriptors;
}

StaticChannel staticChannelSlot(const StaticChannelSet& set, const std::string_view slot,
                                const std::size_t slotIndex) {
    if (slot == "0" || slot == "1")
        return constantChannel(slot);
    std::string name(slot);
    if (slot.empty()) {
        if (set.files) {
            // Only a probe knows the default colour layer. A file-only set gathers it; a set that
            // also names image channels refuses at evaluation if the default lands on one.
            StaticChannel unknown;
            unknown.kind = StaticChannelKind::File;
            return unknown;
        }
        std::vector<std::string> names;
        for (const auto& channel : set.known)
            names.push_back(channel.name);
        const auto channel = defaultShuffleSlotChannel(names, slotIndex);
        if (!channel)
            return absentChannel(slot);
        name = *channel;
    }
    const auto found = std::ranges::find_if(
        set.known, [&](const StaticChannel& channel) { return channel.name == name; });
    if (found != set.known.end())
        return *found;
    if (set.files) {
        StaticChannel file;
        file.name = name;
        file.descriptor = render::describeChannel(name, render::ChannelSampleKind::Float);
        file.kind = StaticChannelKind::File;
        return file;
    }
    return absentChannel(name);
}

StaticChannelSet staticChannelSet(const CompiledChannelExpression& expression) {
    std::vector<StaticChannelSet> values;
    values.reserve(expression.steps.size());
    for (std::size_t index = 0; index < expression.steps.size(); ++index) {
        const auto& step = expression.steps[index];
        const auto earlier = [&](const std::uint32_t at) -> const StaticChannelSet* {
            return at < values.size() ? &values[at] : nullptr;
        };
        StaticChannelSet value;
        switch (step.kind) {
        case CompiledChannelStepKind::Source: {
            if (step.source >= expression.sources.size()) {
                value.files = true;
                break;
            }
            const auto& source = expression.sources[step.source];
            if (!source.imageBacked) {
                value.files = true;
                break;
            }
            for (std::size_t lane = 0; lane < source.imageChannels.size(); ++lane) {
                StaticChannel channel;
                channel.name = source.imageChannels[lane].name;
                channel.descriptor = source.imageChannels[lane];
                channel.kind = StaticChannelKind::Image;
                channel.source = step.source;
                channel.lane = static_cast<std::uint8_t>(lane);
                value.known.push_back(std::move(channel));
            }
            break;
        }
        case CompiledChannelStepKind::Remove: {
            const auto* input = earlier(step.input);
            if (input == nullptr) {
                value.files = true;
                break;
            }
            value.files = input->files;
            for (const auto& channel : input->known)
                if (matchesChannelPattern(step.select, channel.descriptor) == step.keep)
                    value.known.push_back(channel);
            break;
        }
        case CompiledChannelStepKind::Copy: {
            const auto* input = earlier(step.input);
            const auto* from = earlier(step.from);
            if (input == nullptr || from == nullptr) {
                value.files = true;
                break;
            }
            value = *input;
            value.files = input->files || from->files;
            // A file channel of `from` the compiler cannot name may replace a same-named channel.
            if (from->files)
                for (auto& channel : value.known)
                    if (channel.kind == StaticChannelKind::Image &&
                        fileMayMatch(step.select, channel.name))
                        channel.kind = StaticChannelKind::Ambiguous;
            for (const auto& channel : from->known) {
                if (!matchesChannelPattern(step.select, channel.descriptor))
                    continue;
                const auto existing = std::ranges::find_if(
                    value.known, [&](const StaticChannel& item) { return item.name == channel.name; });
                if (existing != value.known.end())
                    *existing = channel;
                else
                    value.known.push_back(channel);
            }
            break;
        }
        case CompiledChannelStepKind::Rename: {
            const auto* input = earlier(step.input);
            if (input == nullptr) {
                value.files = true;
                break;
            }
            constexpr std::array<std::string_view, 4> kSuffixes{"R", "G", "B", "A"};
            for (std::size_t slot = 0; slot < 4; ++slot) {
                auto mapped = staticChannelSlot(*input, step.mapping[slot], slot);
                mapped.name = step.layer.empty() ? std::string(kSuffixes[slot])
                                                 : step.layer + "." + std::string(kSuffixes[slot]);
                mapped.descriptor =
                    render::describeChannel(mapped.name, mapped.descriptor.sampleKind);
                if (mapped.kind == StaticChannelKind::Absent)
                    mapped.kind = StaticChannelKind::File; // evaluation reports the missing name
                value.known.push_back(std::move(mapped));
            }
            break;
        }
        }
        values.push_back(std::move(value));
    }
    if (values.empty())
        return {};
    return std::move(values.back());
}

std::optional<std::array<std::string, 4>> passMapping(const std::span<const PassCandidate> channels,
                                                      const std::string_view pass) {
    if (channels.empty())
        return std::nullopt;
    const auto grey = [](const std::string& name) {
        return std::array<std::string, 4>{name, name, name, "1"};
    };
    std::string layer(pass);
    if (pass.empty()) {
        // The first data layer, else the first layer.
        const auto data = std::ranges::find_if(channels, [](const PassCandidate& channel) {
            return channel.descriptor.encoding == render::ChannelValueEncoding::Data &&
                   channel.descriptor.role != render::ChannelRole::Alpha;
        });
        layer = (data != channels.end() ? *data : channels.front()).descriptor.layer;
    } else if (const auto exact = std::ranges::find_if(
                   channels, [&](const PassCandidate& channel) { return channel.name == pass; });
               exact != channels.end()) {
        return grey(exact->name);
    }
    std::vector<const PassCandidate*> members;
    for (const auto& channel : channels)
        if (channel.descriptor.layer == layer && channel.descriptor.role != render::ChannelRole::Alpha)
            members.push_back(&channel);
    if (members.empty())
        return std::nullopt;
    if (members.size() == 1)
        return grey(members.front()->name);
    const auto prefix = layer.empty() ? std::string{} : layer + ".";
    const auto has = [&](const std::string& name) {
        return std::ranges::any_of(members,
                                   [&](const PassCandidate* member) { return member->name == name; });
    };
    // OpenEXR lists channels alphabetically, so a layer's vector is ordered by its suffixes.
    constexpr std::array<std::array<std::string_view, 3>, 6> kVectors{{{"R", "G", "B"},
                                                                       {"X", "Y", "Z"},
                                                                       {"U", "V", "W"},
                                                                       {"r", "g", "b"},
                                                                       {"x", "y", "z"},
                                                                       {"u", "v", "w"}}};
    for (const auto& vector : kVectors) {
        std::array<std::string, 4> mapping{"0", "0", "0", "1"};
        std::size_t present = 0;
        for (std::size_t slot = 0; slot < 3; ++slot) {
            const auto name = prefix + std::string(vector[slot]);
            if (has(name)) {
                mapping[slot] = name;
                ++present;
            } else if (slot < 2) {
                present = 0;
                break;
            }
        }
        if (present >= 2)
            return mapping;
    }
    std::array<std::string, 4> mapping{"0", "0", "0", "1"};
    for (std::size_t slot = 0; slot < 3 && slot < members.size(); ++slot)
        mapping[slot] = members[slot]->name;
    return mapping;
}

} // namespace bloom::runtime::detail
