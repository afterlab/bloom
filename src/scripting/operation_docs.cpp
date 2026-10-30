// The reference text of the built-in operations: what each one does and the unit, range and closed
// values of its arguments. It is applied to the registry once (OperationRegistry::builtIn()), so
// `query operations`, Host.schemas(), bloom-cli help, bloom.help() and the MCP `help` tool all read
// the same schema. An operation registered with its own description keeps it.

#include "operation_docs.hpp"
#include "operation_values.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace bloom::scripting::detail {
namespace {

using namespace std::string_view_literals;

constexpr std::string_view kTimeUnit =
    "frames at the composition's frame rate (an integer), or exact seconds as "
    "[numerator, denominator]";
constexpr std::string_view kColorUnit =
    "straight (unpremultiplied) RGBA in the composition's working colour space; red, green and "
    "blue are finite and may exceed 1, alpha is 0..1";
constexpr std::string_view kPixelUnit =
    "composition pixels: x to the right, y down, origin at the frame's top-left corner";

// clang-format off
constexpr std::array kNotes{
    // --- animation ----------------------------------------------------------------------------
    OperationNotes{"bloom.animation.convert-to-constant",
        "Stops animating a parameter: removes its curve and keeps one constant value, as turning "
        "off the key diamond in Properties does.",
        "", "bloom.animation.create-for-parameter bloom.parameter.set-source query:keyframes",
        "The parameter is not animated => read its source with query {kind:\"parameter\"}; only a "
        "curve-driven parameter converts."},
    OperationNotes{"bloom.animation.create-for-parameter",
        "Makes a constant parameter animated: creates its curve with one key holding the current "
        "value at `time`, as the first click on a key diamond does. Pass `component` to animate "
        "one axis or channel of a vector or colour parameter.",
        "animationCurve, keyframe",
        "bloom.animation.set-keyframe-at-time-for-parameter bloom.animation.convert-to-constant "
        "query:keyframes",
        "The parameter is already animated => key it with "
        "bloom.animation.set-keyframe-at-time-for-parameter instead.\nThe parameter cannot be "
        "animated => query {kind:\"parameter\"} reports animatable:false for it."},
    OperationNotes{"bloom.animation.delete-keyframe",
        "Deletes one key from a curve (one component of a vector or colour curve when `component` "
        "is given).",
        "", "bloom.animation.delete-keyframes query:keyframes", ""},
    OperationNotes{"bloom.animation.delete-keyframes",
        "Deletes several keys in one step. Each address is [curve, keyframe] or [curve, keyframe, "
        "component].",
        "", "bloom.animation.delete-keyframe query:keyframes", ""},
    OperationNotes{"bloom.animation.insert-scalar-keyframe",
        "Adds a key to a scalar curve at `time`.", "keyframe",
        "bloom.animation.set-keyframe-at-time query:keyframes",
        "A key already exists at that time => use bloom.animation.update-scalar-keyframe or "
        "bloom.animation.set-keyframe-at-time, which updates it."},
    OperationNotes{"bloom.animation.insert-vec2-keyframe",
        "Adds a key to both components of a vec2 curve (for example Position) at `time`.",
        "keyframe", "bloom.animation.set-keyframe-at-time query:keyframes", ""},
    OperationNotes{"bloom.animation.insert-vec3-keyframe",
        "Adds a key to all three components of a vec3 curve at `time`.", "keyframe",
        "bloom.animation.set-keyframe-at-time query:keyframes", ""},
    OperationNotes{"bloom.animation.insert-color4-keyframe",
        "Adds a key to all four channels of a colour curve at `time`.", "keyframe",
        "bloom.animation.set-keyframe-at-time query:keyframes", ""},
    OperationNotes{"bloom.animation.move-keyframes",
        "Moves keys to new times in one step. Each move is [curve, keyframe, time] or [curve, "
        "keyframe, time, component]; `time` is the key's new absolute time.",
        "", "query:keyframes", "Two keys would land on one time => move them in separate "
        "transactions or choose distinct times."},
    OperationNotes{"bloom.animation.paste-keyframes",
        "Writes keys onto parameters, creating their curves when needed, as pasting keys at the "
        "playhead does. Each entry is [parameter, time, value] plus an optional component and "
        "interpolation.",
        "keyframe (one per pasted key)", "bloom.animation.set-keyframe-at-time-for-parameter", ""},
    OperationNotes{"bloom.animation.set-keyframe-at-time",
        "Sets a curve's value at `time`: updates the key there, or inserts one. The value has the "
        "curve's own width (a number, [x, y], [x, y, z] or [r, g, b, a]).",
        "keyframe", "bloom.animation.set-keyframe-at-time-for-parameter query:keyframes", ""},
    OperationNotes{"bloom.animation.set-keyframe-at-time-for-parameter",
        "Sets an animated parameter's value at `time`: updates the key there or inserts one. The "
        "parameter must already be animated; make a constant one animated first with "
        "bloom.animation.create-for-parameter (in the same transaction is fine). The value has "
        "the parameter's own width (query {kind:\"parameter\"} reports valueKind).",
        "keyframe",
        "bloom.animation.create-for-parameter "
        "bloom.animation.set-keyframe-at-time-for-parameter-component query:keyframes "
        "bloom.parameter.set-source",
        "The parameter is not animated (\"Parameter does not have an animation source\") => run "
        "bloom.animation.create-for-parameter first.\nThe value has the wrong width => match the "
        "parameter's valueKind: scalar is a number, vector2 is [x, y], vector3 [x, y, z], color "
        "[r, g, b, a]."},
    OperationNotes{"bloom.animation.set-keyframe-at-time-for-parameter-component",
        "Keys one axis or channel of an animated vector or colour parameter at `time`, leaving "
        "the other components as they are. Make a constant parameter animated first with "
        "bloom.animation.create-for-parameter.",
        "keyframe",
        "bloom.animation.create-for-parameter "
        "bloom.animation.set-keyframe-at-time-for-parameter query:keyframes",
        "The parameter is not animated => run bloom.animation.create-for-parameter first."},
    OperationNotes{"bloom.animation.set-keyframe-handles",
        "Sets ease handles. Each handle is [curve, keyframe, time, value] with an optional "
        "component: `time` is a fraction 0..1 of the segment's duration measured from the key, "
        "`value` an offset from the key's value. Handles shape ease-in-out segments only.",
        "", "bloom.animation.set-keyframe-interpolation query:keyframes", ""},
    OperationNotes{"bloom.animation.set-keyframe-interpolation",
        "Sets the outgoing interpolation of one key: the segment from this key to the next.",
        "", "bloom.animation.set-keyframes-interpolation query:keyframes", ""},
    OperationNotes{"bloom.animation.set-keyframe-values",
        "Changes the values of several keys in one step without moving them. Each edit is "
        "[curve, keyframe, value] or [curve, keyframe, value, component].",
        "", "query:keyframes", ""},
    OperationNotes{"bloom.animation.set-keyframes-interpolation",
        "Sets the outgoing interpolation of several keys in one step.", "",
        "bloom.animation.set-keyframe-interpolation query:keyframes", ""},
    OperationNotes{"bloom.animation.update-scalar-keyframe",
        "Re-places one scalar key: its time, value and outgoing interpolation.", "keyframe",
        "query:keyframes", ""},
    OperationNotes{"bloom.animation.update-vec2-keyframe",
        "Re-places one vec2 key: its time, value and outgoing interpolation.", "keyframe",
        "query:keyframes", ""},
    OperationNotes{"bloom.animation.update-vec3-keyframe",
        "Re-places one vec3 key: its time, value and outgoing interpolation.", "keyframe",
        "query:keyframes", ""},
    OperationNotes{"bloom.animation.update-color4-keyframe",
        "Re-places one colour key: its time, value and outgoing interpolation.", "keyframe",
        "query:keyframes", ""},

    // --- assets -------------------------------------------------------------------------------
    OperationNotes{"bloom.asset.create-folder",
        "Creates an Assets folder.", "folder", "bloom.asset.move bloom.asset.move-folders query:assets",
        ""},
    OperationNotes{"bloom.asset.ensure-font",
        "Adds a font face as an asset, or returns the existing asset with the same digest, so a "
        "Text layer can use it.",
        "asset", "bloom.asset.relink-font query:assets", ""},
    OperationNotes{"bloom.asset.relink-font",
        "Points a font asset at another file with the given digest.", "asset",
        "bloom.asset.ensure-font", ""},
    OperationNotes{"bloom.asset.import",
        "Imports media files and folders into Assets in one step: hashes and probes each file "
        "(image sequences are detected from numbered names) and adds one asset per item. Files "
        "the project already holds are reused, not duplicated. It does not add layers; follow "
        "with bloom.layer.add-image or bloom.layer.add-audio.",
        "asset (one per imported or reused item)",
        "bloom.layer.add-image bloom.layer.add-audio query:assets",
        "[operation-warning] A path cannot be read or is not a supported format => it is reported as a "
        "bloom.scripting.operation-warning diagnostic and skipped; fix the path and import it "
        "again."},
    OperationNotes{"bloom.asset.relink",
        "Points an asset at a different media file (Replace Footage), keeping every layer that "
        "uses it.",
        "", "query:assets", ""},
    OperationNotes{"bloom.asset.move",
        "Moves assets into a folder at a position.", "", "bloom.asset.reorder query:assets", ""},
    OperationNotes{"bloom.asset.move-folders",
        "Moves folders under another folder.", "", "bloom.asset.create-folder", ""},
    OperationNotes{"bloom.asset.move-compositions",
        "Files compositions into an Assets folder.", "", "query:compositions", ""},
    OperationNotes{"bloom.asset.remove",
        "Removes an asset from the project. Layers that use it lose their media.", "",
        "query:assets", ""},
    OperationNotes{"bloom.asset.remove-folder",
        "Removes an Assets folder.", "", "bloom.asset.create-folder", ""},
    OperationNotes{"bloom.asset.rename",
        "Renames an asset.", "", "query:assets", ""},
    OperationNotes{"bloom.asset.rename-folder",
        "Renames an Assets folder.", "", "bloom.asset.create-folder", ""},
    OperationNotes{"bloom.asset.reorder",
        "Sets the complete order of the assets in one folder.", "", "bloom.asset.move", ""},
    OperationNotes{"bloom.asset.set-tags",
        "Replaces the tag list of each asset.", "", "query:assets", ""},
    OperationNotes{"bloom.asset.set-input-color",
        "Sets how an image or video asset's pixels are interpreted (Input Colour): a legacy "
        "choice, or an exact colour space of the project's OCIO config.",
        "", "query:assets", "[invalid-argument] Both a legacy choice other than auto and inputColorSpaceId are given "
        "=> pass one of them."},

    // --- compositions -------------------------------------------------------------------------
    OperationNotes{"bloom.composition.add",
        "Creates a composition with its own Merge and Output nodes and no layers.", "composition",
        "bloom.composition.set-settings query:compositions bloom.layer.add-solid", ""},
    OperationNotes{"bloom.composition.set-settings",
        "Changes the named Composition Settings in one step; omitted settings stay. A size change "
        "keeps top-level layers pinned to `anchor`.",
        "", "query:compositions bloom.composition.set-duration bloom.composition.set-format",
        "[invalid-argument] A pair is incomplete (frameRateNumerator without frameRateDenominator, or the pixel "
        "aspect parts) => pass both parts of the pair."},
    OperationNotes{"bloom.composition.clear-work-area",
        "Clears the work area so playback, RAM preview and work-area exports cover the whole "
        "composition.",
        "", "bloom.composition.set-work-area query:compositions", ""},
    OperationNotes{"bloom.composition.delete",
        "Deletes a composition. Deleting the last composition leaves an empty project.", "",
        "query:compositions", ""},
    OperationNotes{"bloom.composition.duplicate",
        "Copies a composition with its layers, nodes, parameters and keys.", "composition",
        "query:compositions", ""},
    OperationNotes{"bloom.composition.set-background-color",
        "Sets the composition's background colour. The background is display-only: renders, "
        "exports and captures keep alpha and do not include it.",
        "", "bloom.composition.set-settings", ""},
    OperationNotes{"bloom.composition.set-duration",
        "Sets the composition's duration.", "", "bloom.composition.set-settings query:compositions",
        ""},
    OperationNotes{"bloom.composition.set-format",
        "Sets the composition's pixel size and frame rate.", "",
        "bloom.composition.set-settings query:compositions", ""},
    OperationNotes{"bloom.composition.set-name",
        "Renames a composition.", "", "query:compositions", ""},
    OperationNotes{"bloom.composition.set-safe-areas",
        "Sets the action-safe and title-safe guide fractions the Viewer draws. Guides are "
        "display-only and never rendered.",
        "", "bloom.composition.set-settings", ""},
    OperationNotes{"bloom.composition.set-work-area",
        "Sets the work area: the span playback, RAM preview and work-area export jobs cover. "
        "`end` is exclusive and at most the composition duration.",
        "", "bloom.composition.clear-work-area query:compositions",
        "start is not before end, or end is past the duration => pass 0 <= start < end <= "
        "duration (query:compositions reports the duration)."},

    // --- data blocks --------------------------------------------------------------------------
    OperationNotes{"bloom.data-block.create",
        "Creates a data block: a curve, ramp or opaque payload nodes can read.", "dataBlock",
        "bloom.data-block.replace-payload", ""},
    OperationNotes{"bloom.data-block.replace-payload",
        "Replaces a data block's payload.", "", "bloom.data-block.create", ""},
    OperationNotes{"bloom.data-block.remove", "Removes a data block.", "", "", ""},
    OperationNotes{"bloom.data-block.relink", "Points a data block at another source file.", "",
        "", ""},
    OperationNotes{"bloom.data-block.set-tags", "Replaces a data block's tag list.", "", "", ""},

    // --- export queue (ADR 0033) --------------------------------------------------------------
    OperationNotes{"bloom.export.add-job",
        "Adds an export job for a composition to the project's export queue (the Export panel's "
        "Add). Output keeps alpha; the composition background colour is not rendered. Start it "
        "with the export.start tool.",
        "exportJob, exportOutput (one per output)",
        "export.start export.status export.presets bloom.export.add-output", ""},
    OperationNotes{"bloom.export.remove-jobs", "Removes export jobs from the queue.", "",
        "export.status", ""},
    OperationNotes{"bloom.export.remove-finished-jobs", "Removes finished (Done) export jobs.", "",
        "export.status", ""},
    OperationNotes{"bloom.export.duplicate-job", "Copies an export job with its outputs.",
        "exportJob, exportOutput", "export.status", ""},
    OperationNotes{"bloom.export.move-jobs", "Moves export jobs in the queue's run order.", "",
        "export.status", ""},
    OperationNotes{"bloom.export.set-job-name", "Renames an export job.", "", "export.status", ""},
    OperationNotes{"bloom.export.set-job-range",
        "Sets which frames an export job renders. Custom ranges are inclusive composition frame "
        "indices.",
        "", "export.status", ""},
    OperationNotes{"bloom.export.set-job-enabled",
        "Includes or excludes an export job from export.start-all.", "", "export.start-all", ""},
    OperationNotes{"bloom.export.set-job-label-color", "Sets or clears a job's row label colour.",
        "", "", ""},
    OperationNotes{"bloom.export.add-output",
        "Adds an output (a preset, settings and destination) to an export job.", "exportOutput",
        "export.presets bloom.export.set-output-destination", ""},
    OperationNotes{"bloom.export.remove-output", "Removes an output from its job.", "", "", ""},
    OperationNotes{"bloom.export.set-output-settings",
        "Replaces an output's settings with a document 1.29 settings object.", "",
        "bloom.export.apply-preset export.presets", ""},
    OperationNotes{"bloom.export.apply-preset",
        "Replaces an output's settings with a built-in preset's.", "", "export.presets", ""},
    OperationNotes{"bloom.export.set-output-destination",
        "Sets where an output is written: a project-relative path, or a file URI with "
        "portability external. It may use {name} (composition name), {job} (job name), {format} "
        "and {date}; an image sequence's pattern also takes <base>, <ext> and exactly one #### "
        "or {frame}.",
        "", "export.confirm-replace", ""},
    OperationNotes{"bloom.export.set-output-enabled", "Includes or excludes one output of a job.",
        "", "", ""},
    OperationNotes{"bloom.export.set-output-import",
        "Chooses whether the verified result is imported into Assets.", "", "", ""},

    // --- layers -------------------------------------------------------------------------------
    OperationNotes{"bloom.layer.add-audio",
        "Adds an audio layer for an imported audio asset on top of the layer stack.",
        "layer, slot, audioNode, layerOutputNode", "bloom.asset.import query:layers", ""},
    OperationNotes{"bloom.layer.add-image",
                   "Adds a layer showing an imported image, image sequence or video asset on top "
                   "of the layer stack, centred in the composition. A still image spans the "
                   "composition; a sequence or video starts at frame 0 and lasts its natural "
                   "length (capped by the composition), showing nothing after its last frame.",
                   "layer, slot, imageNode, layerOutputNode, positionParameter, anchorParameter, "
                   "scaleParameter, rotationParameter, opacityParameter, blendModeParameter, "
                   "imageToLayerEdge, layerToStackEdge",
                   "bloom.asset.import query:layers", ""},
    OperationNotes{"bloom.layer.add-shape",
        "Adds a shape layer (rectangle, ellipse, triangle, polygon, star, line or a path) on top "
        "of the layer stack. Fill, stroke and other shape parameters are edited afterwards with "
        "bloom.parameter.set-source (query:layers lists them).",
        "layer, slot, layerOutputNode and the shape's nodes and parameters (listed in the result)",
        "query:layers bloom.parameter.set-source", ""},
    OperationNotes{"bloom.layer.add-solid",
        "Adds a composition-sized solid colour layer on top of the layer stack.",
        "layer, slot, solidNode, layerOutputNode, colorParameter, widthParameter, "
        "heightParameter, positionParameter, "
        "anchorParameter, scaleParameter, rotationParameter, opacityParameter, blendModeParameter",
        "query:layers bloom.parameter.set-source bloom.animation.set-keyframe-at-time-for-parameter",
        ""},
    OperationNotes{"bloom.layer.add-text",
        "Adds a text layer on top of the layer stack. Font, alignment, box and the other text "
        "parameters are edited afterwards with bloom.parameter.set-source; their ids are in the "
        "outputs and in query:layers.",
        "layer, slot, textNode, layerOutputNode, textParameter, sizeParameter, colorParameter, "
        "fontParameter, boxParameter, wrapParameter, anchorModeParameter, overflowParameter, "
        "positionParameter, anchorParameter, scaleParameter, rotationParameter, opacityParameter, "
        "blendModeParameter",
        "query:layers bloom.parameter.set-source", ""},
    OperationNotes{"bloom.layer.move-before",
        "Reorders the layer stack: moves a slot to sit above another slot, or to the bottom. "
        "query:layers reports each layer's slot; index 0 is the topmost layer.",
        "", "query:layers bloom.merge.reorder-input", ""},
    OperationNotes{"bloom.layer.rename", "Renames a layer.", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-enabled",
        "Shows or hides a layer (the Timeline eye).", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-solo", "Solos or unsolos a layer.", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-locked",
        "Locks or unlocks a layer. A locked layer and the nodes it depends on refuse edits.", "",
        "query:layers", ""},
    OperationNotes{"bloom.layer.set-label-color",
        "Sets or clears the layer's Timeline label colour.", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-parent",
        "Parents a layer to another layer's transform, or clears the parent.", "", "query:layers",
        "The parent would form a cycle => choose a layer that is not a child of this one."},
    OperationNotes{"bloom.layer.set-range",
        "Trims a layer: sets the composition span it is visible in, without moving its content. "
        "`end` is exclusive: the layer shows frames start..end-1.",
        "", "bloom.layer.set-timing query:layers",
        "start is not before end, or end is past the composition duration => pass 0 <= start < "
        "end <= duration."},
    OperationNotes{"bloom.layer.set-timing",
        "Sets layer-owned time in one step (ADR 0024): where the layer's content starts "
        "(`startTime`, which moves content and keys), its visible span start..end (end "
        "exclusive), and an exact speed.",
        "", "bloom.layer.set-range bloom.layer.set-time-modes query:layers",
        "[invalid-argument] The speed is outside 1/1000..1000 => pick an exact ratio inside it."},
    OperationNotes{"bloom.layer.set-time-modes",
        "Sets what a layer shows before and after its content: nothing, the first or last frame "
        "held, a loop or a ping-pong.",
        "", "bloom.layer.set-timing query:layers", ""},
    OperationNotes{"bloom.layer.enable-time-remap",
        "Turns on Time Remap for a layer: an animatable parameter mapping layer time to content "
        "time.",
        "", "bloom.layer.disable-time-remap query:layers", ""},
    OperationNotes{"bloom.layer.disable-time-remap", "Turns off Time Remap for a layer.", "",
        "bloom.layer.enable-time-remap", ""},
    OperationNotes{"bloom.layer.add-mask",
        "Adds a mask to a layer. The path is in the layer's content space (pixels of the layer's "
        "own content, before its transform).",
        "mask, path, feather, expansion, opacity", "bloom.layer.set-mask-path query:layers", ""},
    OperationNotes{"bloom.layer.set-mask-path",
        "Sets a mask's outline. On an animated mask path this keys the outline at `time`.", "",
        "bloom.layer.animate-mask-path", ""},
    OperationNotes{"bloom.layer.animate-mask-path",
        "Makes a mask path animated with a key at `time`.", "curve, keyframe",
        "bloom.layer.set-mask-path bloom.layer.stop-animating-mask-path", ""},
    OperationNotes{"bloom.layer.stop-animating-mask-path",
        "Removes a mask path's keys and keeps one constant outline.", "",
        "bloom.layer.animate-mask-path", ""},
    OperationNotes{"bloom.layer.remove-mask", "Removes a mask from a layer.", "", "query:layers",
        ""},
    OperationNotes{"bloom.layer.move-mask", "Moves a mask in its layer's mask order.", "",
        "query:layers", ""},
    OperationNotes{"bloom.layer.set-mask-mode",
        "Sets how a mask combines with the masks above it.", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-mask-inverted", "Inverts or restores a mask's coverage.", "",
        "query:layers", ""},
    OperationNotes{"bloom.layer.rename-mask", "Renames a mask.", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-track-matte",
        "Uses another layer of the same Merge as this layer's track matte.", "",
        "bloom.layer.clear-track-matte query:layers", ""},
    OperationNotes{"bloom.layer.clear-track-matte", "Removes a layer's track matte.", "",
        "bloom.layer.set-track-matte", ""},
    OperationNotes{"bloom.layer.split-at-time",
        "Splits a layer at `time` into two layers (Ctrl+K): the original ends at `time` and a copy "
        "starts there.",
        "layer (the new layer)", "query:layers", ""},
    OperationNotes{"bloom.layer.align-layers",
        "Lines up layers' evaluated bounds at `time` (Composition > Align). The bounds are "
        "measured on the server before the transaction runs; skipped layers are warnings.",
        "movedLayer, skippedLayer", "bloom.layer.distribute-layers query:layers", ""},
    OperationNotes{"bloom.layer.distribute-layers",
        "Spaces three or more layers' evaluated centres evenly (Composition > Distribute).",
        "movedLayer, skippedLayer", "bloom.layer.align-layers query:layers", ""},

    // --- graph --------------------------------------------------------------------------------
    OperationNotes{"bloom.merge.reorder-input",
        "Moves one slot of a Merge node's layer stack to an index (0 is the topmost).", "",
        "bloom.layer.move-before query:nodes", ""},
    OperationNotes{"bloom.merge.set-enabled", "Enables or bypasses a Merge node.", "",
        "query:nodes", ""},
    OperationNotes{"bloom.node-group.create",
        "Frames nodes in a named group. Groups are layout only and never change evaluation.",
        "nodeGroup", "query:nodes", ""},
    OperationNotes{"bloom.node-group.remove", "Removes a group frame; its nodes stay.", "",
        "query:nodes", ""},
    OperationNotes{"bloom.node-group.rename", "Renames a group frame.", "", "query:nodes", ""},
    OperationNotes{"bloom.node-group.set-members", "Sets the nodes a group frames.", "",
        "query:nodes", ""},
    OperationNotes{"bloom.node.add",
        "Adds a node of a registered type with its default parameters. query:node-types lists "
        "the types, their ports and parameters.",
        "node", "query:node-types bloom.node.connect query:nodes",
        "Unknown nodeType => pick an id from query {kind:\"node-types\"}.\nThe type allows one "
        "per composition (Merge, Output) and one exists => reuse the existing node."},
    OperationNotes{"bloom.node.connect",
        "Connects an output port to an input port, or writes a Merge layer-stack slot when "
        "`role` is given. Kinds must match or promote (integer to scalar, boolean to integer or "
        "scalar, scalar to vector).",
        "edge; layerSlot when a new stack slot is created",
        "bloom.node.disconnect-input query:nodes query:node-types",
        "The port kinds do not match => query:node-types shows each port's kind; insert a "
        "conversion node.\nThe link would form a cycle => connect a node that is not downstream."},
    OperationNotes{"bloom.node.disconnect-input",
        "Clears one input port, or one Merge stack slot when `role` is given.", "",
        "bloom.node.connect query:nodes", ""},
    OperationNotes{"bloom.node.dissolve",
        "Removes a node and reconnects its upstream to its downstream where the kinds match.", "",
        "bloom.node.remove", ""},
    OperationNotes{"bloom.node.duplicate",
        "Copies nodes with their parameters and the links between them.", "node (one per copy)",
        "query:nodes", ""},
    OperationNotes{"bloom.node.move",
        "Moves node cards in the node editor and optionally changes their group membership.", "",
        "query:nodes", ""},
    OperationNotes{"bloom.node.remove",
        "Removes nodes. Removing a layer's Layer Output node deletes the layer (query:layers "
        "reports it as layerOutputNode).",
        "", "bloom.node.dissolve query:layers", ""},
    OperationNotes{"bloom.node.set-collapsed", "Collapses or expands a node card.", "", "", ""},
    OperationNotes{"bloom.node.set-muted",
        "Mutes or unmutes a node: a muted node passes its main input through.", "", "", ""},
    OperationNotes{"bloom.node.set-width", "Sets a node card's width.", "", "", ""},

    // --- parameters and project ---------------------------------------------------------------
    OperationNotes{"bloom.parameter.set-source",
        "Sets what drives a parameter: exactly one of a constant `value`, an existing `curve`, or "
        "a value-graph `driverNode` with `driverPort`. Enumerations (blend mode, loop mode, text "
        "alignment and so on) take either the stored value or its label, matched "
        "case-insensitively (3 or \"screen\" for the Screen blend mode); query "
        "{kind:\"parameter\"} lists the choices.",
        "", "query:parameter bloom.animation.set-keyframe-at-time-for-parameter",
        "[invalid-argument] None of value, curve or driverNode is given => pass exactly one.\nAn unknown label or an out-of-range value of "
        "an enumeration => the message names the valid choices.\nThe value does not fit "
        "the parameter's kind or range => query {kind:\"parameter\"} reports valueKind, range and "
        "choices."},
    OperationNotes{"bloom.project.set-name", "Renames the project.", "", "query:project", ""},
};
// clang-format on

void choose(ArgumentSchema& argument,
            std::initializer_list<std::pair<std::string_view, std::string_view>> values) {
    argument.choices.clear();
    for (const auto& [value, label] : values)
        argument.choices.push_back(
            {.value = Value(std::string(value)), .label = std::string(label)});
}

void chooseIntegers(ArgumentSchema& argument, std::initializer_list<std::string_view> labels) {
    argument.choices.clear();
    std::int64_t stored = 0;
    for (const auto label : labels)
        argument.choices.push_back({.value = Value(stored++), .label = std::string(label)});
    argument.minimum = 0.0;
    argument.maximum = static_cast<double>(stored - 1);
}

[[nodiscard]] std::string_view exampleText(const ArgumentSchema& argument) {
    const auto* text =
        argument.example ? std::get_if<std::string>(&argument.example->storage) : nullptr;
    return text ? std::string_view(*text) : std::string_view{};
}

void range(ArgumentSchema& argument, const double minimum, const double maximum) {
    argument.minimum = minimum;
    argument.maximum = maximum;
}

[[nodiscard]] std::string idUnit(const std::string_view name) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 24> units{{
        {"composition", "composition id (query:compositions)"},
        {"layer", "layer id (query:layers .id)"},
        {"parent", "layer id (query:layers .id)"},
        {"source", "layer id (query:layers .id)"},
        {"key", "layer id (query:layers .id)"},
        {"parameter", "parameter id (query:layers .parameters, query:nodes .parameters)"},
        {"curve", "animation curve id (query:keyframes, or the parameter's curveId)"},
        {"keyframe", "keyframe id (query:keyframes)"},
        {"mask", "mask id (query:layers .masks)"},
        {"slot", "layer-stack slot id (query:layers .slot)"},
        {"before", "layer-stack slot id (query:layers .slot)"},
        {"insertBefore", "layer-stack slot id (query:layers .slot)"},
        {"node", "node id (query:nodes)"},
        {"sourceNode", "node id (query:nodes)"},
        {"targetNode", "node id (query:nodes)"},
        {"driverNode", "node id (query:nodes)"},
        {"merge", "Merge node id (query:nodes)"},
        {"group", "node group id (query:nodes .group)"},
        {"asset", "asset id (query:assets)"},
        {"folder", "Assets folder id (query:assets .folder)"},
        {"dataBlock", "data block id"},
        {"job", "export job id (export.status)"},
        {"output", "export output id (export.status)"},
        {"project", "project id (query:project)"},
    }};
    for (const auto& [key, unit] : units)
        if (key == name)
            return std::string(unit);
    return "stable id (a positive integer)";
}

void refine(const std::string& op, ArgumentSchema& argument) {
    const auto& name = argument.name;
    const bool layerOp = op.starts_with("bloom.layer.") || op.starts_with("bloom.composition.");
    switch (argument.kind) {
    case ValueKind::Time:
        argument.unit = kTimeUnit;
        break;
    case ValueKind::Color4:
        argument.unit = kColorUnit;
        break;
    case ValueKind::Id:
        argument.unit = name == "before" && op != "bloom.layer.move-before"
                            ? std::string("layer id (query:layers .id)")
                            : idUnit(name);
        argument.minimum = 1.0;
        break;
    case ValueKind::Path:
        argument.unit = "{closed, anchors:[{point:[x, y], inHandle:[x, y]|null, outHandle:[x, "
                        "y]|null, mode?}]} in the layer's content-space pixels; handles are "
                        "relative to their anchor point";
        break;
    case ValueKind::Vec2:
    case ValueKind::Vec3:
    case ValueKind::Double:
        if (name == "value")
            argument.unit = "the animated parameter's own stored units (query:parameter reports "
                            "them)";
        break;
    case ValueKind::Value:
        if (argument.unit.empty())
            argument.unit = "the parameter's own width: a number, [x, y], [x, y, z] or "
                            "[r, g, b, a]";
        break;
    default:
        break;
    }
    if (argument.context == ContextSource::Selection)
        argument.unit = "array of node ids (query:nodes); defaults to the selected nodes";
    if (argument.context == ContextSource::Layers)
        argument.unit = "array of layer ids (query:layers .id); defaults to the selected layers";

    if (name == "interpolation") {
        choose(argument, {{"hold", "Hold"}, {"linear", "Linear"}, {"ease-in-out", "Ease In-Out"}});
        if (!argument.required)
            argument.defaultText = "linear";
    } else if (name == "component") {
        chooseIntegers(argument, {"X", "Y", "Z", "Red", "Green", "Blue", "Alpha"});
        if (!argument.required)
            argument.defaultText = "the whole value (every component)";
    } else if (name == "opacity" && argument.kind == ValueKind::Double) {
        range(argument, 0.0, 1.0);
        argument.unit = "fraction 0..1";
        argument.defaultText = "1";
    } else if ((name == "position" || name == "lineStart" || name == "lineEnd") && layerOp) {
        argument.unit = kPixelUnit;
        if (name == "position")
            argument.defaultText = "the composition centre";
    } else if (name == "position" || name == "offset") {
        argument.unit = "node-editor layout units";
    } else if (name == "size" && op == "bloom.layer.add-shape") {
        argument.unit = "pixels as [width, height]";
        argument.defaultText = "[400, 300]";
    } else if (name == "size" && op == "bloom.layer.add-text") {
        argument.unit = "pixels (em size)";
        argument.minimum = 0.0;
        argument.defaultText = "72";
    } else if ((name == "width" || name == "height") && argument.kind == ValueKind::Integer) {
        argument.unit = "pixels";
        range(argument, 1.0, 1048576.0);
    } else if (name == "width" && op == "bloom.node.set-width") {
        argument.unit = "node-editor layout units";
    } else if (name == "frameRateNumerator" || name == "frameRateDenominator") {
        argument.unit = "frames per second as numerator / denominator (24000/1001 is 23.976)";
        range(argument, 1.0, 4294967295.0);
    } else if (name == "pixelAspectNumerator" || name == "pixelAspectDenominator") {
        argument.unit = "pixel aspect as numerator / denominator (1/1 is square)";
        argument.minimum = 1.0;
    } else if (name == "actionSafe" || name == "titleSafe" || name == "action" || name == "title") {
        argument.unit = "fraction of the frame, 0..1";
        range(argument, 0.0, 1.0);
    } else if (name == "anchor" && op == "bloom.composition.set-settings") {
        chooseIntegers(argument, {"Top left", "Top centre", "Top right", "Middle left", "Centre",
                                  "Middle right", "Bottom left", "Bottom centre", "Bottom right"});
        argument.defaultText = "4 (centre)";
    } else if ((name == "before" || name == "after") && op == "bloom.layer.set-time-modes") {
        chooseIntegers(argument, {"None", "Hold", "Loop", "Ping-pong"});
    } else if (name == "mode" && op.starts_with("bloom.layer.")) {
        chooseIntegers(argument, {"None", "Add", "Subtract", "Intersect", "Difference"});
        if (!argument.required)
            argument.defaultText = "1 (Add)";
    } else if (name == "channel" && op == "bloom.layer.set-track-matte") {
        chooseIntegers(argument, {"Alpha", "Luma"});
    } else if (name == "kind" && op == "bloom.layer.add-shape") {
        choose(argument, {{"rectangle", "Rectangle"},
                          {"ellipse", "Ellipse"},
                          {"triangle", "Triangle"},
                          {"polygon", "Polygon"},
                          {"star", "Star"},
                          {"line", "Line"},
                          {"path", "Path"}});
    } else if (name == "legacy") {
        choose(argument,
               {{"auto", "Auto"}, {"srgb", "sRGB"}, {"linear", "Linear"}, {"raw", "Raw"}});
        argument.defaultText = "auto";
    } else if (name == "portability" && op.starts_with("bloom.asset.")) {
        choose(argument, {{"system", "System font"},
                          {"builtin", "Built-in font"},
                          {"project-relative", "Project-relative file"}});
        argument.defaultText = "system";
    } else if (name == "portability" && op.starts_with("bloom.data-block.")) {
        choose(argument,
               {{"project-relative", "Project-relative file"}, {"external", "External file"}});
        argument.defaultText = "project-relative";
    } else if (name == "portability") {
        choose(argument, {{"project-relative", "Inside the project folder"},
                          {"external", "A file URI anywhere"}});
        argument.defaultText = "project-relative";
    } else if (name == "overwrite") {
        choose(argument, {{"ask", "Ask before replacing (the run stops at the existing file "
                                  "until export.confirm-replace)"},
                          {"replace", "Replace the existing file"},
                          {"refuse", "Fail when the file exists"}});
        argument.defaultText = "ask";
    } else if (name == "range" && op.starts_with("bloom.export.")) {
        choose(argument, {{"work-area", "Work area"},
                          {"composition", "Whole composition"},
                          {"custom", "Custom range first..last (inclusive)"},
                          {"frame", "One frame (first)"}});
        if (!argument.required)
            argument.defaultText = "work-area";
    } else if ((name == "first" || name == "last") && op.starts_with("bloom.export.")) {
        argument.unit = "composition frame index (inclusive)";
        argument.minimum = 0.0;
    } else if (name == "edge") {
        choose(argument, {{"left", "Left"},
                          {"horizontal-centre", "Horizontal centre"},
                          {"right", "Right"},
                          {"top", "Top"},
                          {"vertical-middle", "Vertical middle"},
                          {"bottom", "Bottom"}});
    } else if (name == "axis") {
        choose(argument, {{"horizontal", "Horizontal"}, {"vertical", "Vertical"}});
    } else if (name == "target") {
        choose(argument, {{"selection", "The union of the layers' bounds"},
                          {"composition", "The composition frame"},
                          {"key-object", "The key layer's bounds"}});
        argument.defaultText = "selection";
    } else if (name == "placement") {
        choose(argument, {{"top", "Above every layer"},
                          {"bottom", "Below every layer"},
                          {"before", "Directly above the layer `before`"}});
        argument.defaultText = "top";
    } else if (name == "role" &&
               (op == "bloom.node.connect" || op == "bloom.node.disconnect-input" ||
                exampleText(argument) == "content")) {
        choose(argument, {{"content", "The slot's image (layer) input"},
                          {"audio", "The slot's audio input"}});
    } else if ((name == "payloadKind" || name == "kind") && op.starts_with("bloom.data-block.")) {
        choose(argument, {{"curve", "Curve"}, {"ramp", "Ramp"}, {"opaque", "Opaque"}});
        argument.defaultText = "opaque";
    } else if (name == "color" && op == "bloom.layer.set-label-color") {
        argument.unit = "label colour as [r, g, b], each an integer 0..255";
        range(argument, 0.0, 255.0);
        argument.defaultText = "no label";
    } else if (name == "labelColor") {
        argument.unit = "label colour as [r, g, b], each an integer 0..255";
        range(argument, 0.0, 255.0);
    } else if (name == "speedNumerator" || name == "speedDenominator") {
        argument.unit = "exact speed ratio numerator / denominator, between 1/1000 and 1000";
        range(argument, 1.0, 2147483647.0);
    } else if (name == "index") {
        argument.unit = "zero-based position";
        argument.minimum = 0.0;
    } else if (name == "background" && argument.kind == ValueKind::Color4) {
        argument.unit = std::string(kColorUnit) +
                        "; display-only: renders, exports and captures keep alpha and do not "
                        "include the background";
    } else if (name == "color" && op == "bloom.composition.set-background-color") {
        argument.unit = std::string(kColorUnit) +
                        "; display-only: renders, exports and captures keep alpha and do not "
                        "include the background";
    } else if (name == "workingColorSpace") {
        argument.unit = "a scene-linear colour space name of the project's OCIO config; empty "
                        "follows the project";
    } else if (name == "projectDirectory") {
        argument.unit = "directory path; relative locators are made relative to it";
    } else if (name == "digest") {
        argument.unit = "64 lowercase hexadecimal characters (SHA-256)";
    } else if (name == "nodeType") {
        argument.unit = "node type id (query:node-types)";
    } else if (name == "sourcePort" || name == "port" || name == "driverPort") {
        argument.unit = "port name (query:node-types lists each type's ports)";
    }
}

} // namespace

const OperationNotes* operationNotes(const std::string_view typeId) noexcept {
    const auto found = std::ranges::find(kNotes, typeId, &OperationNotes::id);
    return found == kNotes.end() ? nullptr : &*found;
}

std::span<const OperationNotes> allOperationNotes() noexcept { return kNotes; }

void describeOperations(std::vector<OperationDescriptor>& descriptors) {
    for (auto& descriptor : descriptors) {
        auto& schema = descriptor.schema;
        if (schema.description.empty()) {
            if (const auto* notes = operationNotes(schema.typeId))
                schema.description = std::string(notes->description);
        }
        for (auto& argument : schema.arguments) {
            // An operation that describes its own argument keeps it.
            if (!argument.unit.empty() || !argument.choices.empty())
                continue;
            refine(schema.typeId, argument);
        }
    }
}

} // namespace bloom::scripting::detail
