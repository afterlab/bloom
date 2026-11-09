// The reference text of the built-in operations: what each one does and the unit, range and closed
// values of its arguments. It is applied to the registry once (OperationRegistry::builtIn()), so
// `query operations`, Host.schemas(), bloom-cli help, bloom.help() and the MCP `help` tool all read
// the same schema. An operation registered with its own description keeps it.

#include "operation_docs.hpp"
#include "operation_values.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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
// Keyframe operations: the unit depends on who owns the parameter or curve being keyed.
constexpr std::string_view kKeyTimeUnit =
    "frames at the composition's frame rate (an integer), or exact seconds as [numerator, "
    "denominator], in the unit of the key's owner: LAYER time for a layer's own parameters "
    "(query:layers .parameters: position, opacity, blend mode, Time Remap), which is the "
    "composition time minus the layer's start, times its speed (a layer starting at frame 48 "
    "shows its frame 0 at composition frame 48); COMPOSITION time for every other parameter (a "
    "node's, a source's, an effect's). A layer key outside the layer's range never plays; the "
    "edit succeeds and returns a warning naming the range";
constexpr std::string_view kColorUnit =
    "straight (unpremultiplied) RGBA in the composition's working colour space; red, green and "
    "blue are finite and may exceed 1, alpha is 0..1";
constexpr std::string_view kPixelUnit =
    "composition pixels: x to the right, y down, origin at the frame's top-left corner";

// clang-format off
constexpr std::array kNotes{
    // --- animation ----------------------------------------------------------------------------
    OperationNotes{"bloom.animation.easy-ease",
        "Applies Easy Ease to keys: the eased side gets a Bezier handle of 33.33% influence and "
        "zero speed, as F9 does in the Graph Editor.",
        "", "bloom.animation.set-keyframes-easing query:keyframes",
        "A key address names no key => read the curve and key ids with query "
        "{kind:\"keyframes\", parameter}."},
    OperationNotes{"bloom.animation.set-keyframes-easing",
        "Sets the easing equation parameters of keys (which end eases, Back's overshoot, Elastic's "
        "amplitude and period) and optionally switches their outgoing interpolation. Keys keep "
        "their times and values.",
        "", "bloom.animation.easy-ease bloom.animation.set-keyframes-handle-mode query:keyframes",
        "A key address names no key => read the curve and key ids with query "
        "{kind:\"keyframes\", parameter}."},
    OperationNotes{"bloom.animation.set-keyframes-handle-mode",
        "Sets how the two handles of keys relate: free, aligned or auto. Auto recomputes the "
        "handles in the same undo step.",
        "", "bloom.animation.set-keyframes-easing query:keyframes",
        "The mode is not free, aligned or auto => pass one of the listed values."},
    OperationNotes{"bloom.animation.convert-to-constant",
        "Stops animating a parameter: removes its curve and keeps one constant value, as turning "
        "off the key diamond in Properties does.",
        "", "bloom.animation.create-for-parameter bloom.parameter.set-source query:keyframes",
        "The parameter is not animated => read its source with query {kind:\"parameter\"}; only a "
        "curve-driven parameter converts."},
    OperationNotes{"bloom.animation.create-for-parameter",
        "Makes a constant parameter animated: creates its curve with one key holding the current "
        "value at `time`, as the first click on a key diamond does. Pass `component` to animate "
        "one axis or channel of a vector or colour parameter. `time` is in layer time for a "
        "layer's own parameters (the composition frame minus the layer's start) and in "
        "composition time for every other parameter; a key outside the layer's range succeeds "
        "with a warning naming the range.",
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
    OperationNotes{"bloom.animation.set-keys",
        "Sets a parameter's whole list of keys in one undo step: a key at an existing time takes "
        "the new value, any other time gets a new key, and a constant parameter becomes "
        "animated. With `replace` the existing keys go first and the parameter returns to the "
        "constant it was animated from, so an empty list with `replace` leaves it constant. "
        "Each key is [time, value] or [time, value, interpolation]; the last key's "
        "interpolation is always linear. Key times are in layer time for a layer's own "
        "parameters (the composition frame minus the layer's start) and in composition time for "
        "every other parameter; keys outside the layer's range succeed with a warning naming "
        "the range.",
        "animationCurve (when created), keyframe (one per key, in the order given)",
        "bloom.animation.set-key-columns bloom.animation.set-keyframe-at-time-for-parameter "
        "bloom.animation.convert-to-constant query:keyframes",
        "[invalid-argument] A key is not [time, value, interpolation] => the message names the "
        "key index and the part to fix.\nTwo keys share a time => give each key its own time.\n"
        "The parameter is driven by a value node => give it a constant with "
        "bloom.parameter.set-source first.\nThe value does not fit the parameter => the message "
        "names the key index; query {kind:\"parameter\"} reports valueKind and range."},
    OperationNotes{"bloom.animation.set-key-columns",
        "Sets a parameter's keys from columns, in one undo step: `times`, `values` and "
        "optionally `interpolations`, one entry per key, instead of set-keys' rows. It is the "
        "same edit as set-keys with [time, value, interpolation] rows, for hundreds of keys. "
        "Over MCP any column can be {\"fromFile\": \"<absolute .json path>\"}, so the numbers "
        "never pass through the conversation, and transact's `response: \"summary\"` answers "
        "with a count and a digest instead of every created id.",
        "animationCurve (when created), keyframe (one per key, in the order given)",
        "bloom.animation.set-keys bloom.animation.convert-to-constant query:keyframes",
        "[invalid-argument] The columns differ in length => values has one entry per time, and "
        "interpolations at most one.\nTwo keys share a time => give each key its own time.\n"
        "The parameter is driven by a value node => give it a constant with "
        "bloom.parameter.set-source first."},
    OperationNotes{"bloom.animation.set-keyframe-at-time-for-parameter",
        "Sets an animated parameter's value at `time`: updates the key there or inserts one. The "
        "parameter must already be animated; make a constant one animated first with "
        "bloom.animation.create-for-parameter (in the same transaction is fine). The value has "
        "the parameter's own width (query {kind:\"parameter\"} reports valueKind). `time` is in "
        "layer time for a layer's own parameters (the composition frame minus the layer's start) "
        "and in composition time for every other parameter; a key outside the layer's range "
        "succeeds with a warning naming the range.",
        "keyframe",
        "bloom.animation.create-for-parameter "
        "bloom.animation.set-keyframe-at-time-for-parameter-component query:keyframes "
        "bloom.parameter.set-source",
        "The parameter is not animated (\"Parameter N is not animated\") => run "
        "bloom.animation.create-for-parameter first.\nThe value has the wrong width => match the "
        "parameter's valueKind: scalar is a number, vector2 is [x, y], vector3 [x, y, z], color "
        "[r, g, b, a]."},
    OperationNotes{"bloom.animation.set-keyframe-at-time-for-parameter-component",
        "Keys one axis or channel of an animated vector or colour parameter at `time`, leaving "
        "the other components as they are. Make a constant parameter animated first with "
        "bloom.animation.create-for-parameter. `time` is in layer time for a layer's own "
        "parameters (the composition frame minus the layer's start) and in composition time for "
        "every other parameter; a key outside the layer's range succeeds with a warning naming "
        "the range.",
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
    OperationNotes{"bloom.asset.import",
        "Imports media files and folders into Assets in one step: hashes and probes each file "
        "(image sequences are detected from numbered names) and adds one asset per item. Files "
        "the project already holds are reused, not duplicated. It does not add layers; follow "
        "with bloom.layer.add-image or bloom.layer.add-audio.",
        "asset (one per imported or reused item)",
        "bloom.layer.add-image bloom.layer.add-audio query:assets",
        "[operation-warning] A path cannot be read or is not a supported format => it is reported as a "
        "bloom.scripting.operation-warning diagnostic and skipped; fix the path and import it "
        "again.\n"
        "[invalid-argument] A relative path in a project that has never been saved, with no "
        "projectDirectory => refused before anything is imported, because there is no project "
        "folder to resolve it against; pass an absolute path, or projectDirectory, or save the "
        "project first."},
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
        "Adds a composition-sized solid colour layer on top of the layer stack. `name` and "
        "`color` are required; `position` and `opacity` are optional.",
        "layer, slot, solidNode, layerOutputNode, colorParameter, widthParameter, "
        "heightParameter, positionParameter, "
        "anchorParameter, scaleParameter, rotationParameter, opacityParameter, blendModeParameter",
        "query:layers bloom.parameter.set-source bloom.animation.set-keyframe-at-time-for-parameter",
        ""},
    OperationNotes{"bloom.layer.add-text",
        "Adds a text layer on top of the layer stack. Font, alignment, box and the other text "
        "parameters are edited afterwards with bloom.parameter.set-source; their ids are in the "
        "outputs and in query:layers. The font is a font ID string (family, style, weight and "
        "source), never an asset.",
        "layer, slot, textNode, layerOutputNode, textParameter, sizeParameter, colorParameter, "
        "fontParameter, boxParameter, wrapParameter, anchorModeParameter, overflowParameter, "
        "positionParameter, anchorParameter, scaleParameter, rotationParameter, opacityParameter, "
        "blendModeParameter",
        "query:layers bloom.parameter.set-source", ""},
    OperationNotes{"bloom.layer.add-svg",
        "Imports an SVG file as native layers: every drawn element becomes a Shape layer driven by "
        "its own Path node (keyable points, Trim Path, Offset Path, Boolean Paths and the other "
        "path nodes can be wired to it), every group a nested Merge that keeps its name and order, "
        "and text a Text layer. Gradients, clips, masks and dash patterns are built from Gradient "
        "nodes, track mattes and Path Stroke. Transforms and the view box are baked into the points. "
        "The file is read here (nothing is fetched, no script runs); what could not be imported is "
        "listed by name in the operation's warnings. One undo step. `fit` (default true) scales the "
        "artwork to fit the composition and centres it. `projectDirectory` (optional) is the project "
        "folder the source file's recorded locator is made relative to; the file's path and "
        "SHA-256 are kept in the project's assets.",
        "layer, slot, merge", "query:layers bloom.node.set-parameter-value", ""},
    OperationNotes{"bloom.layer.move-before",
        "Reorders the layer stack: moves a slot to sit above another slot, or to the bottom. "
        "query:layers reports each layer's slot; index 0 is the topmost layer.",
        "", "query:layers bloom.merge.reorder-input", ""},
    OperationNotes{"bloom.layer.rename", "Renames a layer.", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-enabled",
        "Shows or hides a layer (the Timeline eye).", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-solo", "Solos or unsolos a layer.", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-shy",
        "Marks a layer shy: hidden from the layer table while its shy filter is on. It never "
        "changes the render.",
        "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-name-follows-source",
        "Makes a layer show its source's name and follow it when the source is renamed, or stops "
        "following. Renaming the layer stops it following.",
        "", "bloom.layer.rename query:layers",
        "The layer has no source => only a layer with an asset or nested composition source can "
        "follow a name."},
    OperationNotes{"bloom.node-group.set-color",
        "Sets a node group's frame colour, or clears it to the neutral frame when `color` is "
        "omitted.",
        "", "query:nodes", ""},
    OperationNotes{"bloom.layer.set-locked",
        "Locks or unlocks a layer. A locked layer and the nodes it depends on refuse edits.", "",
        "query:layers", ""},
    OperationNotes{"bloom.layer.set-label-color",
        "Sets or clears the layer's Timeline label colour.", "", "query:layers", ""},
    OperationNotes{"bloom.layer.set-parent",
        "Parents a layer to another layer's transform, or clears the parent. The child's position, "
        "anchor and rotation are then measured in the parent's space, whose origin is the "
        "top-left corner of the parent's content (a nested composition's frame), and move, scale "
        "and rotate with the parent.",
        "", "query:layers",
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
        "Uses another layer of the same Merge as this layer's track matte. The matte layer stops "
        "displaying in the composite and only shapes this layer; that is intended. The matte "
        "layer may be a child of this layer.",
        "",
        "bloom.layer.clear-track-matte query:layers", ""},
    OperationNotes{"bloom.layer.clear-track-matte", "Removes a layer's track matte.", "",
        "bloom.layer.set-track-matte bloom.layer.set-node-track-matte", ""},
    OperationNotes{"bloom.layer.set-node-track-matte",
        "Uses a node's Mask output (a Cryptomatte, a Mask Combine) as this layer's track matte.",
        "", "bloom.layer.clear-track-matte query:nodes query:layers", ""},
    OperationNotes{"bloom.layer.add-matte-mask",
        "Adds a mask whose shape is a node's Mask output (a Cryptomatte selection, a Mask "
        "Combine) instead of a path.",
        "mask, path, feather, expansion, opacity",
        "bloom.layer.set-mask-matte bloom.layer.clear-mask-matte query:nodes", ""},
    OperationNotes{"bloom.layer.set-mask-matte",
        "Shapes an existing mask by a node's Mask output instead of its path.", "",
        "bloom.layer.clear-mask-matte bloom.layer.add-matte-mask query:layers", ""},
    OperationNotes{"bloom.layer.clear-mask-matte",
        "Returns a mask shaped by a node to its own path.", "",
        "bloom.layer.set-mask-matte query:layers", ""},
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
    // #502 Every Merge entry is a layer; these carry their own descriptions.
    OperationNotes{"bloom.merge.ensure-entry-layer", "",
        "layer; parameter.position, parameter.anchor, parameter.scale, parameter.rotation, "
        "parameter.opacity, parameter.blendMode when the layer is created",
        "query:layers bloom.merge.set-entry-parameter bloom.merge.insert-layer-node",
        "The Merge entry does not exist in the composition => read merge and slot from "
        "query {kind:\"layers\"} entries."},
    OperationNotes{"bloom.merge.insert-layer-node", "",
        "node, layer, edge, parameter.<role>",
        "query:layers bloom.merge.ensure-entry-layer",
        "The Merge entry does not exist in the composition => read merge and slot from "
        "query {kind:\"layers\"} entries.\nAn entry without an image has no Layer node to "
        "insert => pick an entry with an image source.\nLayer is locked => unlock it with "
        "bloom.layer.set-locked (merge and slot address it)."},
    OperationNotes{"bloom.merge.add-adjustment-layer", "",
        "node (the Adjustment node), layerSlot, layer",
        "query:layers bloom.node.add-fed-by bloom.merge.set-entry-parameter",
        "The Merge does not exist in the composition => read Merge ids from query "
        "{kind:\"nodes\"}.\nThe entry to add the Adjustment Layer above does not exist in the "
        "Merge => pass a slot from query {kind:\"layers\"} entries, or omit above."},
    OperationNotes{"bloom.merge.set-entry-parameter", "",
        "layer; parameter.<role> when the layer is created; keyframe when keyed",
        "query:layers bloom.parameter.set-source "
        "bloom.animation.set-keyframe-at-time-for-parameter bloom.merge.insert-layer-node",
        "[invalid-argument] The role is not a layer control => pass position, anchor, scale, "
        "rotation, opacity, blendMode or timeRemap.\nTime Remap is off on this layer => run "
        "bloom.layer.enable-time-remap with the same merge and slot first.\nThe value does not "
        "fit the parameter => query {kind:\"parameter\"} with the entry's parameter id reports "
        "its kind, range and choices."},
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
        "{kind:\"parameter\"} lists the choices. A text font takes a font ID as a string, for "
        "example \"source=google;family=Roboto;style=Bold;weight=700;italic=false\" (source is "
        "system, google or builtin; family is required; style, weight, italic, axes, ps and "
        "version are optional); query {kind:\"parameter\"} reports it with its fontStatus.",
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
    // A key time is in the unit of its owner (layer time for a layer's own parameters).
    const bool keyed = op.starts_with("bloom.animation.") && name == "time";
    switch (argument.kind) {
    case ValueKind::Time:
        argument.unit = keyed ? kKeyTimeUnit : kTimeUnit;
        break;
    case ValueKind::Color4:
        argument.unit = kColorUnit;
        break;
    case ValueKind::Id:
        argument.unit = name == "before" && op != "bloom.layer.move-before"
                            ? std::string("layer id (query:layers .id)")
                        : name == "source" && op == "bloom.node.add-composition-source"
                            ? std::string("composition id (query:compositions)")
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

    if (name == "ease" && op == "bloom.animation.set-keyframes-easing") {
        choose(argument, {{"auto", "The usual default: out for back, bounce and elastic"},
                          {"in", "Ease in"},
                          {"out", "Ease out"},
                          {"in-out", "Ease in and out"}});
        argument.defaultText = "auto";
    } else if (name == "mode" && op == "bloom.animation.set-keyframes-handle-mode") {
        choose(argument, {{"free", "Independent handles"},
                          {"aligned", "Editing one handle keeps the other collinear"},
                          {"auto", "Auto Clamped handles, recomputed from the neighbouring keys"}});
    } else if (name == "side" && op == "bloom.animation.easy-ease") {
        choose(argument, {{"both", "Easy Ease (F9)"},
                          {"in", "Easy Ease In (Shift+F9)"},
                          {"out", "Easy Ease Out (Ctrl+Shift+F9)"}});
        argument.defaultText = "both";
    } else if (name == "anchor" && op == "bloom.layer.set-anchor-preset") {
        choose(argument, {{"top-left", "Top left"},
                          {"top", "Top centre"},
                          {"top-right", "Top right"},
                          {"left", "Middle left"},
                          {"center", "Centre"},
                          {"right", "Middle right"},
                          {"bottom-left", "Bottom left"},
                          {"bottom", "Bottom centre"},
                          {"bottom-right", "Bottom right"}});
    } else if (name == "point" && op == "bloom.cryptomatte.pick") {
        argument.unit = kPixelUnit;
    } else if (name == "interpolation") {
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
    } else if (name == "channel" && (op == "bloom.layer.set-track-matte" ||
                                      op == "bloom.layer.set-node-track-matte")) {
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
    } else if (name == "kind" && op == "bloom.data-block.create") {
        choose(argument, {{"curve", "Sampled curve (give payloadKind \"curve\")"},
                          {"ramp", "Color ramp (give payloadKind \"ramp\")"},
                          {"opaque", "Opaque bytes (payloadKind \"opaque\"); the default"},
                          {"analysis", "Analysis result held as opaque bytes (payloadKind "
                                       "\"opaque\")"}});
        argument.defaultText = "opaque";
        argument.unit = "block kind; the kind and payloadKind must agree. table, point-set, path "
                        "and mask blocks need payloads this operation cannot build and are "
                        "refused";
    } else if (name == "typeId" && op == "bloom.data-block.create") {
        argument.unit = "namespaced identifier: lowercase letters, digits, '.', '_' and '-', "
                        "starting with a letter or digit, at most 128 bytes, such as "
                        "\"bloom.curve\". Bloom names its own asset-backed blocks "
                        "bloom.media.asset and bloom.color.lut-asset";
    } else if (name == "mediaType" && op == "bloom.data-block.create") {
        argument.unit = "media type text of 1 to 256 UTF-8 bytes, such as "
                        "\"application/vnd.bloom.curve\", \"application/vnd.bloom.ramp\" or "
                        "\"application/octet-stream\". Bloom checks only its length and "
                        "encoding; it writes application/vnd.bloom.asset for a block backed by an "
                        "asset";
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
    } else if (name == "times" && op == "bloom.animation.set-key-columns") {
        argument.unit = "list of times, one per value; in " + std::string(kKeyTimeUnit);
    } else if (name == "keys" && op == "bloom.animation.set-keys") {
        argument.unit = "list of [time, value, interpolation?]; times in " +
                        std::string(kKeyTimeUnit);
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

// What an operation changes beyond the object it names. Each text is checked against the command
// it describes; an operation without one has no such effect. Separate sentences are separate lines.
struct SideEffectNote final {
    std::string_view id;
    std::string_view text;
};

// clang-format off
constexpr std::array kSideEffects{
    SideEffectNote{"bloom.node.connect",
        "An input holds one link: connecting to an input that is already fed replaces that link "
        "(the edge keeps its id and the old source is disconnected).\n"
        "Linking into a parameter's operand socket makes the parameter driven: its constant value "
        "is replaced by the link, and disconnecting returns it to the registered default."},
    SideEffectNote{"bloom.node.add-feeding",
        "When the input was already fed, the new node is inserted into the link: what fed the "
        "input now feeds the new node's first input of the same kind.\n"
        "A new node with no input of that kind (a value node, for example) replaces the existing "
        "link instead; nothing is kept of it."},
    SideEffectNote{"bloom.node.disconnect-input",
        "Disconnecting the only remaining link of a Merge layer-stack slot removes the slot, and "
        "the layer loses the track mattes it shared in that Merge.\n"
        "Disconnecting a parameter's operand socket returns the parameter to its registered "
        "default constant."},
    SideEffectNote{"bloom.node.duplicate",
        "Repeated ids in `nodes` collapse: a node named twice is copied once.\n"
        "Links among the copied nodes are copied, and a copied Layer Output keeps reading the same "
        "source; no other link from outside the set is copied."},
    SideEffectNote{"bloom.node.remove",
        "Removing a layer's Layer Output node deletes that layer.\n"
        "Links to and from a removed node are removed with it, and a Merge slot it fed goes too.\n"
        "A layer whose parent or track-matte source was a removed Layer Output loses that link.\n"
        "A removed node's parameters, and a removed layer's masks, are deleted with it."},
    SideEffectNote{"bloom.layer.duplicate",
        "A layer named N times is copied N times (unlike bloom.node.duplicate, which collapses "
        "repeated ids).\n"
        "The copies take new ids and a new stack slot directly below the original."},
    SideEffectNote{"bloom.layer.delete",
        "The layer's source nodes stay in the graph; only the layer is removed."},
    SideEffectNote{"bloom.parameter.set-source",
        "Setting a constant, a driver or another curve on an animated parameter retires its "
        "animation curve and every key on it; undo restores them. (bloom.animation."
        "convert-to-constant does the same and keeps one value.)\n"
        "Pointing a source node's asset or composition parameter at another one renames the "
        "layers whose name follows their source.\n"
        "Refused when the parameter belongs to a locked layer."},
    SideEffectNote{"bloom.animation.convert-to-constant",
        "The parameter's animation curve and all its keys are removed; undo restores them."},
    SideEffectNote{"bloom.composition.duplicate",
        "The copy is named after the source with \" copy\" appended and takes new ids for its "
        "layers, nodes and parameters."},
    SideEffectNote{"bloom.asset.import",
        "Files the project already holds are reused: no second asset is made and the existing "
        "asset's id is reported.\n"
        "A project that has never been saved is supported: it has no project folder, so an "
        "absolute path imports and the assets keep external locators (the file's absolute "
        "location, portability `external`) instead of project-relative ones. The file is "
        "not copied, so the asset stays valid after Save As, and it stops resolving if the file "
        "moves; relink it then. A relative path in that project is refused unless "
        "projectDirectory is given, and nothing is imported."},
    SideEffectNote{"bloom.asset.relink",
        "The asset keeps its id, name, folder, tags and place in the list; only its file, size, "
        "duration and digest change, so every layer that uses it shows the new file."},
};
// clang-format on

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '.'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool outputNameCharacter(const char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '<' || c == '>' ||
           c == '_';
}

// What kind of id an output carries, from its name.
[[nodiscard]] std::string outputKind(std::string_view name) {
    const auto dot = name.find('.');
    const auto base = name.substr(0, dot);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 24> kinds{{
        {"layer", "layer"},
        {"movedLayer", "layer"},
        {"skippedLayer", "layer"},
        {"slot", "layer-slot"},
        {"layerSlot", "layer-slot"},
        {"node", "node"},
        {"editorNode", "node"},
        {"audioNode", "node"},
        {"imageNode", "node"},
        {"solidNode", "node"},
        {"textNode", "node"},
        {"layerOutputNode", "node"},
        {"edge", "edge"},
        {"composition", "composition"},
        {"asset", "asset"},
        {"folder", "asset-folder"},
        {"mask", "mask"},
        {"keyframe", "keyframe"},
        {"animationCurve", "animation-curve"},
        {"curve", "animation-curve"},
        {"exportJob", "export-job"},
        {"exportOutput", "export-output"},
        {"dataBlock", "data-block"},
        {"nodeGroup", "node-group"},
    }};
    for (const auto& [key, kind] : kinds)
        if (base == key)
            return std::string(kind);
    if (base == "parameter" || base.ends_with("Parameter") || base == "path" || base == "feather" ||
        base == "expansion" || base == "opacity")
        return "parameter";
    return std::string(base);
}

[[nodiscard]] ReferenceOutput makeOutput(std::string name, std::string note, const bool repeated) {
    ReferenceOutput output;
    output.kind = outputKind(name);
    output.name = std::move(name);
    output.note = std::move(note);
    output.repeated = repeated;
    return output;
}

// "animationCurve (when created), keyframe (one per key); edge" -> the outputs it names.
[[nodiscard]] std::vector<ReferenceOutput> declaredOutputs(const std::string_view declared) {
    std::vector<ReferenceOutput> result;
    std::size_t depth = 0;
    std::size_t start = 0;
    const auto flush = [&](const std::size_t end) {
        auto token = trimmed(declared.substr(start, end - start));
        std::size_t length = 0;
        while (length < token.size() && outputNameCharacter(token[length]))
            ++length;
        if (length == 0 || token.substr(0, length) == "and" || token.substr(0, length) == "none")
            return;
        auto note = std::string(trimmed(token.substr(length)));
        if (note.size() >= 2 && note.front() == '(' && note.back() == ')')
            note = note.substr(1, note.size() - 2);
        const bool repeated = note.find("one per") != std::string::npos ||
                              note.find("each") != std::string::npos;
        result.push_back(makeOutput(std::string(token.substr(0, length)), std::move(note), repeated));
    };
    for (std::size_t index = 0; index < declared.size(); ++index) {
        const char c = declared[index];
        if (c == '(')
            ++depth;
        else if (c == ')' && depth > 0)
            --depth;
        else if ((c == ',' || c == ';') && depth == 0) {
            flush(index);
            start = index + 1;
        }
    }
    flush(declared.size());
    return result;
}

// The "Outputs:" sentence of an operation that carries its own text: every quoted name is an output.
[[nodiscard]] std::vector<ReferenceOutput> describedOutputs(const std::string_view description) {
    std::vector<ReferenceOutput> result;
    const auto at = description.find("Outputs:");
    if (at == std::string_view::npos)
        return result;
    auto sentence = description.substr(at + 8);
    for (const std::string_view stop : {"Errors:", "Undo:", "MCP example:"}) {
        if (const auto end = sentence.find(stop); end != std::string_view::npos)
            sentence = sentence.substr(0, end);
    }
    std::size_t cursor = 0;
    std::size_t previousEnd = 0;
    while (true) {
        const auto open = sentence.find('"', cursor);
        if (open == std::string_view::npos)
            break;
        const auto close = sentence.find('"', open + 1);
        if (close == std::string_view::npos)
            break;
        const auto name = std::string(sentence.substr(open + 1, close - open - 1));
        const auto before = sentence.substr(previousEnd, open - previousEnd);
        const auto nextOpen = sentence.find('"', close + 1);
        const auto after = sentence.substr(close + 1, (nextOpen == std::string_view::npos
                                                           ? sentence.size()
                                                           : nextOpen) - close - 1);
        const bool repeated = (before.find("one ") != std::string_view::npos &&
                               after.find(" per ") != std::string_view::npos) ||
                              after.find("for each") != std::string_view::npos ||
                              after.find("per ") == 1;
        if (!name.empty() && outputNameCharacter(name.front()))
            result.push_back(makeOutput(name, {}, repeated));
        previousEnd = close + 1;
        cursor = close + 1;
    }
    return result;
}

} // namespace

std::vector<ReferenceOutput> operationOutputs(const std::string_view typeId,
                                              const std::string_view description) {
    if (const auto* notes = operationNotes(typeId); notes != nullptr && !notes->outputs.empty())
        return declaredOutputs(notes->outputs);
    return describedOutputs(description);
}

std::vector<std::string> operationSideEffects(const std::string_view typeId) {
    std::vector<std::string> result;
    const auto found = std::ranges::find(kSideEffects, typeId, &SideEffectNote::id);
    if (found == kSideEffects.end())
        return result;
    auto rest = found->text;
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        result.emplace_back(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    }
    return result;
}

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
