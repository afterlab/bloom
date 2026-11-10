#pragma once

// #509 Glowing paths: ready-made Node Groups the Add menu offers (Saber, Beam, Vegas, Write-on and
// Stroke). Each is an ordinary group definition (document/node_group_nodes.hpp) holding a Path
// Stroke of its own path, a Shape Source that fills the stroke, and a Glow, with the values an
// artist reaches for wired to the definition's Group Input, and a Node Group instance that reads
// it. Nothing about a preset is stored beside those nodes: once made it is a group like any other,
// edited, saved and reopened as one. A group slot has no Path kind and cannot be keyed, so the path
// (its Path Stroke's own path) and the motion of Write-on and Vegas are held inside the group,
// edited by opening it.

#include <bloom/commands/operation.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace bloom::commands {

enum class PathGlowPreset : std::uint8_t { Saber, Beam, Vegas, WriteOn, Stroke };
inline constexpr std::array kPathGlowPresets{PathGlowPreset::Saber, PathGlowPreset::Beam,
                                             PathGlowPreset::Vegas, PathGlowPreset::WriteOn,
                                             PathGlowPreset::Stroke};
[[nodiscard]] std::string_view pathGlowPresetName(PathGlowPreset preset) noexcept;

inline constexpr std::string_view kPathGlowPresetNodeOutput = "node";
inline constexpr std::string_view kPathGlowPresetGroupOutput = "group";

// Adds the preset's group definition to the project and its Node Group instance at `position` in
// `host`, in one step. The instance's slots hold the preset's values (Width, the glow's Size and
// Intensity, and the colour). Write-on's End and Vegas's Dash Offset run with time inside the group
// (a Value Time times a rate), so both move as soon as they are added.
class CreatePathGlowPreset final : public Operation {
  public:
    CreatePathGlowPreset(document::CompositionId host, PathGlowPreset preset,
                         document::Vec2d position)
        : host_(host), preset_(preset), position_(position) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return host_;
    }

  private:
    document::CompositionId host_;
    PathGlowPreset preset_;
    document::Vec2d position_;
};

} // namespace bloom::commands
