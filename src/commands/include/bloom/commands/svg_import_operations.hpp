#pragma once

// #343: imports an SVG scene as native Bloom layers. Each drawn element becomes a Shape layer driven
// by its own Path node (so its points can be keyed and Trim Path, Offset Path, Boolean Paths, Merge
// Paths, Round Corners and Path Stroke can be wired in), each group becomes a nested Merge that
// keeps its name and order, and what a Shape cannot hold itself (a gradient, a clip, a mask, a dash
// pattern) is built from the nodes Bloom already has. Everything is one undo step.
// See docs/architecture/vector-artwork.md.

#include <bloom/commands/operation.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/document/ids.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace bloom::media {
struct SvgScene;
}

namespace bloom::commands {

// Output names: the root group's layer ("layer"), its stack slot ("slot") and Merge node ("merge").
class AddSvgArtwork final : public Operation {
  public:
    // `scene` is the parsed file (the desktop parses it on a worker). When it is null the file at
    // `path` is read and parsed when the operation applies (scripts and MCP).
    AddSvgArtwork(document::CompositionId composition, std::shared_ptr<const media::SvgScene> scene,
                  std::filesystem::path path, std::string name, bool fit = true)
        : composition_(composition), scene_(std::move(scene)), path_(std::move(path)),
          name_(std::move(name)), fit_(fit) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.layer.add-svg"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    std::shared_ptr<const media::SvgScene> scene_;
    std::filesystem::path path_;
    std::string name_;
    bool fit_;
};

} // namespace bloom::commands
