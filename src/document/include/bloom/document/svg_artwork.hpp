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

// ---- #642: the Vector Artwork layer's expansion ----
//
// A Vector Artwork node (vector_artwork_nodes.hpp) names a data block holding an SVG file's bytes.
// expandVectorArtwork() replaces every such node of a composition with the artwork the editable
// import would build from those bytes for this composition: buildSvgArtwork() over the parsed
// bytes, written with ids derived from the node's id (never ones the project allocator hands
// out), and every edge that read the node's image reading the artwork's root Merge instead. The
// layer that held the node is left exactly as it is, so its transform, opacity, blend mode, track
// matte, masks, parent, timing and every node between the source and its slot apply to the
// artwork's root as they apply to the root group of an editable import.

struct VectorArtworkExpansionContext final {
    // The installed fonts the artwork's text is matched against; null enumerates them when a file
    // has text (names only, no hashing).
    std::shared_ptr<const platform::FontCatalogue> fonts;
    // The parsed scene of a block, so a caller can keep scenes between compiles. Empty: the block's
    // bytes are parsed here. Returning null names the block unreadable.
    std::function<std::shared_ptr<const media::SvgScene>(const DataBlockRecord&)> scene;
    // Polled between nodes and while a block is parsed.
    std::function<bool()> cancelled;
    // Draw the holding layer's transform through the artwork's geometry (see
    // VectorArtworkComposedLayer), or the artwork at the layer's output scale
    // (VectorArtworkOversampledLayer), so the artwork stays sharp at any scale and rotation. Off,
    // the layer resamples the artwork's picture as it resamples an editable import's root group.
    bool composeLayerTransform = true;
    // A preview's constant in place of a parameter's stored source (a Viewer gesture's override),
    // or nothing. The oversampled layer's scale bound reads it, so a drag that grows the layer
    // draws the artwork at the scale it is dragged to.
    std::function<std::optional<ParameterValue>(ParameterId)> override;
};

// #642: how the holding layer's transform reaches the artwork's geometry. A Merge ends vector
// provenance (layer-graph-model.md), so a layer that transformed the artwork's root Merge would
// resample a picture drawn at the composition's scale: soft when scaled up or turned. Instead the
// expansion moves the layer's transform (its position, anchor, scale and rotation parameters, by
// their ids, and its parent) onto a hidden `carrier` Layer node that draws nothing, parents every
// drawn layer of the artwork (`leaves`: shapes, text, gradients and matte sources, and any group
// whose picture is filtered, such as a blurred group) to it, and leaves the holding layer itself at
// the identity. Each shape and text is then covered through the composed matrix at the output
// resolution, exactly as a parented shape layer is. The layer keeps its opacity, blend mode, track
// matte, range, enabled and solo state; layers parented to it are parented to the carrier, which
// moves exactly as the layer did. The layer's published bounds are its artwork's content bounds
// through the carrier's matrix, so the Viewer's box and gestures see the layer as before.
//
// Only a layer the composition cannot change is composed: the node feeds it directly and nothing
// else, it pivots on the frame, and it has no masks (drawn in its content space) and identity
// timing (its content would run on another clock than its transform). Any other layer is drawn as
// a scaled pre-composition instead (VectorArtworkOversampledLayer).
struct VectorArtworkComposedLayer final {
    LayerId carrier;
    std::vector<LayerId> leaves;
};

// #642: the most a layer's artwork is oversampled, and the longest edge (in composition pixels at
// full resolution) the oversampled composition frame may reach. A 3840 x 2160 composition is drawn
// at up to 2x, a 1920 x 1080 one at up to 4x, anything up to 1024 pixels on its longer side at up
// to 8x.
inline constexpr double kVectorArtworkMaxOversample = 8.0;
inline constexpr double kVectorArtworkMaxOversampledEdge = 8192.0;

// #642: a layer that pivots on its content, has masks or its own timing, or reads the artwork
// through effects (a blur, a glow, a drop shadow, colour nodes) is drawn as a scaled
// pre-composition would be. The artwork is drawn at `scale` (a power of two) about the composition
// origin, through the same covered shapes and text: a hidden `carrier` Layer node holding that
// constant scale parents every drawn layer of the artwork (`leaves`, as for a composed layer). Every
// length of the nodes between in composition pixels (a blur's radius, a glow's size, a shadow's
// distance and softness) and the layer's masks (paths, feather, expansion) are multiplied by
// `scale` too, so the nodes see the picture a pre-composition `scale` times larger would give them.
// The layer keeps its own transform, pivot, masks, timing, parent, children, opacity, blend mode and
// track matte; the compiler gives it a content scale (CompiledLayerOutput::contentScale) so its one
// resample maps the larger picture back through its matrix, and its published bounds are the
// picture's at the composition's scale. The result is the plain layer's picture, drawn from
// `scale` times the pixels.
//
// `scale` is the band of the layer's largest output scale over its whole animation: the smallest
// power of two at least the product, over the layer and its parents, of the largest |scale| any of
// their keys (or their handles and easing) reaches, and at least 2 when any of them turns. It is
// part of the plan, so an animated transform renders every frame from one plan and one oversampled
// picture of the artwork; only an edit that moves the band compiles a different picture. It is at
// most kVectorArtworkMaxOversample and kVectorArtworkMaxOversampledEdge over the composition's
// longer side, and is halved while a scaled length would leave its parameter's domain; `limited`
// says why when it is below the band.
struct VectorArtworkOversampledLayer final {
    LayerId carrier;
    std::vector<LayerId> leaves;
    double scale = 1.0;
    std::string limited;
};

struct VectorArtworkExpansion final {
    // The composition with every resolvable Vector Artwork node replaced. Empty when the
    // composition holds none, so the caller keeps reading the original.
    std::optional<Composition> composition;
    // Each expanded node's artwork root (a Merge), by the node's id.
    std::unordered_map<NodeId, NodeId> roots;
    // What the file holds that the artwork does not take, by the node's id (the import's warnings).
    std::unordered_map<NodeId, SvgArtworkNotes> notes;
    // Why a node stayed unexpanded: its block is missing or does not parse. It stays in the
    // composition, and the compiler refuses it by name.
    std::unordered_map<NodeId, std::string> unresolved;
    // The layers whose transform is drawn through their artwork's geometry, by the holding layer.
    std::unordered_map<LayerId, VectorArtworkComposedLayer> composed;
    // The layers whose artwork is drawn at their output scale, by the holding layer.
    std::unordered_map<LayerId, VectorArtworkOversampledLayer> oversampled;
    // Every parameter an oversampled layer's expansion multiplied, and by what, so a preview
    // override of one is multiplied the same way.
    std::unordered_map<ParameterId, double> scaledParameters;
    // Why an expanded node's layer resamples its artwork's picture at the composition's scale, by
    // the node's id. A layer that neither scales up nor turns needs neither and is in none of them.
    std::unordered_map<NodeId, std::string> resampled;
    bool cancelled = false;
};

[[nodiscard]] VectorArtworkExpansion expandVectorArtwork(const Project& project, const Composition& composition,
                                                         const VectorArtworkExpansionContext& context = {});

// The scene an SVG data block holds, parsed (bounded and cancellable like a file import). Null with
// `diagnostic` set when it is not an SVG block or does not parse.
[[nodiscard]] std::shared_ptr<const media::SvgScene>
parseVectorArtworkBlock(const DataBlockRecord& block, std::string& diagnostic,
                        const std::function<bool()>& cancelled = {});

} // namespace bloom::document
