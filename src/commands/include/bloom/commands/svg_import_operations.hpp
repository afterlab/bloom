#pragma once

// #343, #642: imports an SVG scene, in one of two modes and always as one undo step.
//
// As one layer (the default): a Vector Artwork layer (bloom/document/vector_artwork_nodes.hpp). The
// file's bytes are held in a data block of the project, so the layer renders with the file gone,
// and the compiler expands the layer into exactly what the editable import builds. The graph holds
// one layer and one node.
//
// As editable shapes: each drawn element becomes a Shape layer driven by its own Path node (so its
// points can be keyed and Trim Path, Offset Path, Boolean Paths, Merge Paths, Round Corners and Path
// Stroke can be wired in), each group becomes a nested Merge that keeps its name and order, and what
// a Shape cannot hold itself (a gradient, a clip, a mask, a dash pattern) is built from the nodes
// Bloom already has.
//
// Both modes build from one description (bloom/document/svg_artwork.hpp) and report the same named
// warnings. See docs/architecture/vector-artwork.md.

#include <bloom/commands/operation.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::media {
struct SvgScene;
enum class SvgError : std::uint8_t;
} // namespace bloom::media
namespace bloom::platform {
struct FontCatalogue;
}

namespace bloom::commands {

enum class SvgImportMode : std::uint8_t {
    // A Vector Artwork layer: one layer, one node, the file's vector geometry drawn at the output
    // resolution.
    Layer,
    // The editable breakdown: a Merge of Shape, Gradient and Text layers with a Path node per shape.
    Shapes,
};
inline constexpr SvgImportMode kDefaultSvgImportMode = SvgImportMode::Layer;
// The scripting spelling: "layer" and "shapes".
[[nodiscard]] std::string_view svgImportModeName(SvgImportMode mode) noexcept;
[[nodiscard]] std::optional<SvgImportMode> svgImportModeFromName(std::string_view name) noexcept;

// What the file holds that the artwork does not take ("bloom.svg.not-imported") or approximates
// ("bloom.svg.approximated"), by the element's id, label or position.
struct SvgSourceWarning final {
    std::string code;
    std::string summary;
    std::string detail;
};

// An SVG file read for import: everything the import needs, gathered where file I/O and font
// enumeration may happen (a worker, an operation factory), so applying the import reads nothing.
struct PreparedSvgSource final {
    std::filesystem::path path;
    // The parsed file; its sourcePath and sourceDigest name `path` and `digest`.
    std::shared_ptr<const media::SvgScene> scene;
    // The file's bytes exactly as read, which a one-layer import keeps in the project.
    std::shared_ptr<const std::vector<std::byte>> bytes;
    core::Sha256Digest digest;
    // The installed fonts, read only when the file has text.
    std::shared_ptr<const platform::FontCatalogue> fonts;
    std::vector<SvgSourceWarning> warnings;
};

struct SvgSourceRead final {
    std::shared_ptr<const PreparedSvgSource> source;
    media::SvgError error{};
    std::string diagnostic;
    [[nodiscard]] bool cancelled() const noexcept;
};

enum class SvgPreparePhase : std::uint8_t { Reading, FindingFonts };

// Reads (at most 32 MiB), hashes and parses one file, and enumerates the installed fonts when it
// has text. `cancel` is polled throughout; `phase` is told what it is doing. Never on the UI thread.
[[nodiscard]] SvgSourceRead prepareSvgSource(const std::filesystem::path& path,
                                             const std::function<bool()>& cancel = {},
                                             const std::function<void(SvgPreparePhase)>& phase = {});
// The same from bytes already in hand (a copy the project holds); `path` names their file.
[[nodiscard]] SvgSourceRead prepareSvgSource(std::shared_ptr<const std::vector<std::byte>> bytes,
                                             const std::filesystem::path& path,
                                             const std::function<bool()>& cancel = {},
                                             const std::function<void(SvgPreparePhase)>& phase = {});

// Output names: the layer ("layer") and its stack slot ("slot") in both modes; "merge", the root
// group's Merge node, as editable shapes; "node", the Vector Artwork node, and "dataBlock", the
// block holding the file, as one layer; and "asset", the Assets record of the source file when the
// scene was read from one.
class AddSvgArtwork final : public Operation {
  public:
    // `scene` is the parsed file. When it is null the file at `path` is read and parsed when the
    // operation applies (as it is for one layer, which needs the file's bytes, when only a scene is
    // given).
    AddSvgArtwork(document::CompositionId composition, std::shared_ptr<const media::SvgScene> scene,
                  std::filesystem::path path, std::string name, bool fit = true,
                  SvgImportMode mode = kDefaultSvgImportMode);
    // A file prepared by prepareSvgSource(): applying reads nothing. The caller states the mode.
    AddSvgArtwork(document::CompositionId composition, std::shared_ptr<const PreparedSvgSource> source,
                  std::string name, SvgImportMode mode, bool fit = true);
    AddSvgArtwork& withMode(const SvgImportMode mode) {
        mode_ = mode;
        return *this;
    }
    // The project folder the source file's locator is made relative to (empty: the file is external).
    AddSvgArtwork& withProjectDirectory(std::filesystem::path directory) {
        projectDirectory_ = std::move(directory);
        return *this;
    }
    // The installed fonts the file's text is matched against. Without them a file with text
    // enumerates the system's fonts when it applies (names only, no hashing), so the desktop passes
    // the catalogue it read on its worker.
    AddSvgArtwork& withFonts(std::shared_ptr<const platform::FontCatalogue> fonts) {
        fonts_ = std::move(fonts);
        return *this;
    }
    [[nodiscard]] SvgImportMode mode() const noexcept { return mode_; }
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.layer.add-svg"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    std::shared_ptr<const media::SvgScene> scene_;
    std::shared_ptr<const PreparedSvgSource> source_;
    std::filesystem::path path_;
    std::string name_;
    bool fit_;
    SvgImportMode mode_ = kDefaultSvgImportMode;
    std::filesystem::path projectDirectory_;
    std::shared_ptr<const platform::FontCatalogue> fonts_;
};

} // namespace bloom::commands
