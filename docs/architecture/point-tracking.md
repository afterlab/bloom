# Point tracking and Stabilize

Stabilize (`bloom.stabilize`, #513) holds a picture still against the move of a feature, or carries
an element along with it, from a track made by a cancellable job. This document is the specification of
the node, the job, the data it stores and how each meets the GPU coverage contract.

## Scope

Built: a **point tracker** (one feature, translation) with **Stabilize** and **Match Move** applications
of the track, end to end, in the document, the Properties and node card and the CPU and GPU render paths.

Not built, and stated plainly:

- **Planar tracking** (a surface followed through perspective) has no tracker. Corner Pin
  (`image-filters.md`) and the Homography instruction already accept four corners, so a planar track
  would apply through them without a new render operation, but nothing produces those corners.
- **Applying a track to Corner Pin** needs four tracks (or a planar one); the single-point tracker
  cannot drive it, so no such application exists.
- **Camera solve** is a later mode and is not part of this work.
- **Rotation and scale of Stabilize** need two tracked points or a planar track; Stabilize today moves
  the picture only.

## The node

Stabilize has one picture input and one picture output, and these values:

| Value | Meaning |
| --- | --- |
| Mode | Stabilize holds the picture still against the move; Match Move carries it along with the move. A constant choice. |
| Pattern, Search | The size of the pattern followed and of the search around it (Small, Medium, Large and Narrow, Normal, Wide: half sides of 8, 16, 32 and 16, 32, 64 pixels at full resolution). Constants. |
| Track Point | The feature as chosen, on the frame the track starts from, pixels from the frame's centre, y down. Animatable and drivable. |
| Tracked Position | Where the track found the feature, same units. Keyed by the job, animatable and drivable, so a track can be corrected by hand or driven from elsewhere. |
| Filter, Edges | As the other warps (`image-filters.md`, "Corner Pin, ST Map and Displace"). |

The move is `Track Point - Tracked Position` in Stabilize and the reverse in Match Move. It lowers to one field
raster whose program is a coordinate, the Homography instruction in its translation mode (the
similarity map with that offset, no turn, no scale and no window) and the Image read of the input
(`fields.md`, "Homography"). The canvas is the input, so the frame keeps its size. Muted, the node is
its input. Rendering is the ordinary field raster on the CPU reference and the GPU; Stabilize has its own
Required GPU coverage fixtures (`node.bloom.stabilize`, both modes, every filter and edge rule, full
frame and ROI) and native CPU/GPU parity cases, and no coverage exception.

## The job

The Track button asks the `TrackController` to start. On the UI thread it only reads the node's values
and submits one task; everything else runs in the task:

1. Compile the composition with a viewer-only inspection of the output that feeds the node's input
   (the mechanism an Image Preview uses to show any node's picture), so only what feeds the node is
   lowered. The compile honours cancellation.
2. For each frame from the current frame to the end of the composition, evaluate that picture on the CPU
   reference evaluator at the composition's resolution (or a 1920 pixel longest side proxy), convert it
   to Rec. 709 luminance and follow the feature (`point_tracker.hpp`).
3. Return the positions, scores and how the track ended.

The tracker takes a square pattern around the feature on the first frame and finds it on each later
frame by normalised cross-correlation over the search window, then refines the whole-pixel match to a
fraction of a pixel with a few Gauss-Newton steps of the translation that best lays the pattern over the
frame (means removed, so a change of brightness does not move the match). The pattern is the first frame
and is retaken from the current frame when the correlation falls under 0.9, so a feature that changes
slowly is followed without each frame's error adding to the last. A correlation under 0.6, an
unreadable frame, a pattern with no detail, or a pattern reaching the edge ends the track at that frame
with the reason; everything found before it is kept.

Cancellation is checked at each frame and inside the evaluator; a cancelled job stores nothing. Progress
(frames done of total) is reported to the task system and the card's bar, and the reason a job ended is
shown on the card in words. One job runs at a time.

### How pixels reach the tracker, and the GPU coverage rules

The job reads the evaluated picture with the CPU reference evaluator, one whole frame at a time. It is
analysis: it runs as a task, off the UI thread and off every render route; no frame being rendered waits
on it; it adds no mid-frame readback and no per-operation roundtrip to any route; its output is
document data. The pixels it reads are not produced by a GPU pass and relabelled; they are the CPU
reference result, which is the oracle the GPU is held to. The GPU coverage contract
(`src/runtime/include/bloom/runtime/gpu_coverage_contract.hpp`) covers pixel operations, image lowerings
and render routes; the job is none of those, and the node it feeds is a pixel lowering that meets the
contract on its own fixtures. Tracking a picture that only the GPU can produce (the CPU evaluator
reports it as unsupported) fails the job with that reason rather than substituting anything.

## Stored data

When a job ends with at least two frames tracked, one undoable edit stores:

- A **Table data block** (`bloom.point-track`, media type `application/vnd.bloom.point-track`), about the
  node (its subject), with the columns `frame` (integer), `score`, `x` and `y` (columns are sorted by
  name), one row per frame, positions as the Tracked Position parameter's units. Its provenance names the
  producer (`bloom.point-tracker`, version 1) and a SHA-256 digest of the settings (pattern, search,
  frame range and start point), so two tracks made differently can be told apart.
- **Keys** on the node's Tracked Position, one per tracked frame, linear, replacing any earlier keys.

The block is the durable record of the track; the keys are how it is applied, so applying a track is the
node's ordinary animation: editable, undoable and rendered the same everywhere.

## Cost

From the code: the job evaluates one frame per tracked frame on the CPU reference path (the cost of a
CPU preview frame of the node's input) and correlates, per frame, (2 s + 1)^2 positions each over a
(2 p + 1)^2 pattern: with the Normal search (s = 32) and Medium pattern (p = 16) at full resolution,
4,225 positions of 1,089 pixels, about 4.6 million multiply-adds, plus the same window to retake the
pattern. Rendering Stabilize is one field raster pass. No timing is recorded; see
[Benchmarks](../development/benchmarks.md#stabilize-513).

## Verification

`point_tracker_tests` follows an analytic texture moved by known fractions of a pixel (within 0.1 of a
pixel over ten frames; 0.02 measured), a still picture without drift, a feature leaving the picture, a flat picture, an
unreadable frame, a changed picture, an empty range and a cancel. `snapshot_compiler_warp_tests` lowers
Stabilize, reads both modes against an independent nearest reading, and runs the job over a real plan: a
checker moved by a keyed Transform 2D is tracked to two pixels of its own move, frame by frame.
`node_operations_tests` stores a track as one undo step. `stabilize_tests` runs the controller end to
end (refusal, store, undo, cancel, a picture with nothing to follow).
