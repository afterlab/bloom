#pragma once

// #343, #642: the one builder that turns a parsed SVG scene into Bloom document records. It runs in
// two steps so every route builds exactly the same artwork:
//
//   1. buildSvgArtwork() reads the scene and describes the nodes, Merge slots, entry layers, layer
//      values, track mattes and node frames the artwork is made of, in the order they are made.
//      It touches no document and allocates no id: the description is plain data.
//   2. SvgArtworkWriter writes a description into a composition. Where the ids come from and how
//      an entry layer is made is the SvgArtworkTarget's business: the editable import
//      (AddSvgArtwork, "as editable shapes") allocates them from the project in a Draft, and the
//      Vector Artwork layer's expansion (expandVectorArtwork, below) derives them for the
//      compiler, so the one-layer artwork is the editable import by construction.
//
// Qt-free; links bloom_media_vector (the scene) and bloom_platform (the installed fonts).
// See docs/architecture/vector-artwork.md.

#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::media {
struct SvgScene;
} // namespace bloom::media
namespace bloom::platform {
struct FontCatalogue;
} // namespace bloom::platform

namespace bloom::document {

// A name the document accepts: control characters dropped, cut at 200 bytes on a UTF-8 boundary,
// `fallback` when nothing valid is left.
[[nodiscard]] std::string svgArtworkName(std::string name, const std::string& fallback);

// The warnings of one import: every note in order, the first kMaximumListedSvgNotes by name and the
// rest counted by reason (the text after the element's name).
inline constexpr std::size_t kMaximumListedSvgNotes = 380;
struct SvgArtworkNotes final {
    std::vector<std::string> listed;
    std::map<std::string, std::size_t> omitted;

    void add(std::string text);
    // Appends `later` as if each of its notes had been added after these ones.
    void append(const SvgArtworkNotes& later);

    friend bool operator==(const SvgArtworkNotes&, const SvgArtworkNotes&) = default;
};

// A parameter of a described node driven by another described node's output.
struct SvgArtworkDriver final {
    std::size_t node = 0;
    std::string port;

    friend bool operator==(const SvgArtworkDriver&, const SvgArtworkDriver&) = default;
};
using SvgArtworkValue = std::variant<ParameterValue, SvgArtworkDriver>;

struct SvgArtworkNode final {
    std::string type;
    // By parameter role; every other parameter takes its definition's default.
    std::map<std::string, SvgArtworkValue> values;
    Vec2d position;
    bool collapsed = true;

    friend bool operator==(const SvgArtworkNode&, const SvgArtworkNode&) = default;
};

// The steps, in the order they are made. Nodes, slots and layers are named by their index: a node
// by its place in `nodes`, a slot by the order of AddSlot steps, a layer by the order of AddEntry
// steps.
struct SvgArtworkAddNode final {
    std::size_t node = 0;
    friend bool operator==(const SvgArtworkAddNode&, const SvgArtworkAddNode&) = default;
};
// A new slot on top of `merge`'s stack, fed by `source`'s image output.
struct SvgArtworkAddSlot final {
    std::size_t merge = 0;
    std::size_t source = 0;
    friend bool operator==(const SvgArtworkAddSlot&, const SvgArtworkAddSlot&) = default;
};
// `source`'s image output into `destination`'s `input`.
struct SvgArtworkConnect final {
    std::size_t source = 0;
    std::size_t destination = 0;
    std::string input;
    friend bool operator==(const SvgArtworkConnect&, const SvgArtworkConnect&) = default;
};
// The entry layer of a slot of `merge`, named.
struct SvgArtworkAddEntry final {
    std::size_t merge = 0;
    std::size_t slot = 0;
    std::string name;
    friend bool operator==(const SvgArtworkAddEntry&, const SvgArtworkAddEntry&) = default;
};
struct SvgArtworkSetLayerValue final {
    std::size_t layer = 0;
    std::string role;
    ParameterValue value;
    friend bool operator==(const SvgArtworkSetLayerValue&, const SvgArtworkSetLayerValue&) = default;
};
struct SvgArtworkSetTrackMatte final {
    std::size_t layer = 0;
    std::size_t matte = 0;
    TrackMatteChannel channel = TrackMatteChannel::Alpha;
    friend bool operator==(const SvgArtworkSetTrackMatte&, const SvgArtworkSetTrackMatte&) = default;
};
using SvgArtworkStep = std::variant<SvgArtworkAddNode, SvgArtworkAddSlot, SvgArtworkConnect,
                                    SvgArtworkAddEntry, SvgArtworkSetLayerValue,
                                    SvgArtworkSetTrackMatte>;

// A node frame of the node editor: one per SVG group, over the nodes its direct children made.
struct SvgArtworkFrame final {
    std::string name;
    std::vector<std::size_t> members; // ascending node indices
    friend bool operator==(const SvgArtworkFrame&, const SvgArtworkFrame&) = default;
};

struct SvgArtworkDescription final {
    // nodes[0] is the root Merge: the artwork's whole picture is its output. It is made by
    // SvgArtworkWriter::addRoot(), never by a step.
    std::vector<SvgArtworkNode> nodes;
    std::vector<SvgArtworkStep> steps;
    std::vector<SvgArtworkFrame> frames;
    std::size_t slotCount = 0;
    std::size_t layerCount = 0;
    // What the file holds that the artwork does not take or approximates, by name.
    SvgArtworkNotes notes;

    friend bool operator==(const SvgArtworkDescription&, const SvgArtworkDescription&) = default;
};

struct SvgArtworkOptions final {
    // The composition the artwork is fitted into and drawn in.
    double width = 0;
    double height = 0;
    // Contain-and-centre into the composition; false keeps the file's own pixels, centred.
    bool fit = true;
    // The name of the root group (the file's stem); "SVG" when it is not a valid name.
    std::string name;
    // Where the node cards start on the canvas (svgArtworkLayoutOrigin()).
    double layoutOriginX = 0;
    // The installed fonts a file's text is matched against. Without them a file with text
    // enumerates the system's fonts when it is built (names only, no hashing).
    std::shared_ptr<const platform::FontCatalogue> fonts;
};

// The x where a new artwork's cards go: right of every card already in the composition.
[[nodiscard]] double svgArtworkLayoutOrigin(const Composition& composition);

[[nodiscard]] SvgArtworkDescription buildSvgArtwork(const media::SvgScene& scene,
                                                    const SvgArtworkOptions& options);

// Where a writer's ids come from and how it makes an entry layer.
class SvgArtworkTarget {
  public:
    SvgArtworkTarget() = default;
    SvgArtworkTarget(const SvgArtworkTarget&) = delete;
    SvgArtworkTarget& operator=(const SvgArtworkTarget&) = delete;
    virtual ~SvgArtworkTarget() = default;

    [[nodiscard]] virtual std::optional<NodeId> allocateNode() = 0;
    [[nodiscard]] virtual std::optional<ParameterId> allocateParameter() = 0;
    [[nodiscard]] virtual std::optional<LayerSlotId> allocateLayerSlot() = 0;
    [[nodiscard]] virtual std::optional<EdgeId> allocateEdge() = 0;
    [[nodiscard]] virtual std::optional<NodeGroupId> allocateNodeGroup() = 0;
    struct Entry final {
        std::optional<LayerId> layer;
        std::string failure;
    };
    // The entry layer of a slot that has none yet, at the defaults a new entry gets.
    [[nodiscard]] virtual Entry createEntryLayer(NodeId merge, LayerSlotId slot) = 0;
};

// A description that cannot be written: an id space is exhausted, a value is refused by its
// schema, a node type is not registered. Nothing the writer has done is undone; the caller
// discards the composition (a Draft is not committed, an expansion is not used).
class SvgArtworkWriteError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class SvgArtworkWriter final {
  public:
    // `writeLayout`: give every node a card position (the editable import); an expansion the
    // compiler reads needs none.
    SvgArtworkWriter(Composition& composition, SvgArtworkTarget& target,
                     const SvgArtworkDescription& description, bool writeLayout = true);

    // The root Merge (nodes[0]).
    NodeId addRoot();
    // A new slot on top of `merge`, fed by `source`'s image output, and its entry layer.
    LayerSlotId addSlot(NodeId merge, NodeId source);
    LayerId addEntry(NodeId merge, LayerSlotId slot, const std::string& name);
    // Every step after the root, in order.
    void writeSteps();
    // The node frames (layout only).
    void writeFrames();

    [[nodiscard]] const std::vector<NodeId>& nodeIds() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<LayerId>& layerIds() const noexcept { return layers_; }

  private:
    NodeId writeNode(std::size_t index);

    Composition& composition_;
    SvgArtworkTarget& target_;
    const SvgArtworkDescription& description_;
    bool writeLayout_;
    std::vector<NodeId> nodes_;
    std::vector<LayerSlotId> slots_;
    std::vector<LayerId> layers_;
};

} // namespace bloom::document
