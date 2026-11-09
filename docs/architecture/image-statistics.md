# Image Statistics

Image Statistics (`bloom.image-statistics`, schema 1, issue #349) measures one value of the pixels of
a picture and gives the result to the value graph as five Scalars, so that it can drive other nodes: a
Map Range or a Levels that normalizes a depth pass to its own extremes, for example. It is under
Tracking & Analysis. This document is the specification. It also records the owner's decision of
2026-11-09 that allows the one named readback the node needs, and is the owning document of the
coverage contract entry `prep.statistics.result_readback`.

## Node

One Image in, `input` (optional: with no picture wired every output is 0). Five Scalars out:

| Output | Meaning |
|---|---|
| `minimum`, `maximum` | The smallest and largest counted value |
| `mean` | The arithmetic mean of the counted values |
| `deviation` | The population standard deviation of the counted values |
| `count` | The number of pixels counted |

| Role | Default | Domain |
|---|---|---|
| `channel` | Luminance | Luminance (Rec. 709 weights on the color), Red, Green, Blue, Alpha |
| `space` | Straight color | Straight color (the color is divided by alpha; a pixel with no alpha has no color value) or Stored values |
| `pixels` | All pixels | All pixels, or only the pixels with alpha above zero |

All three are constants: they have no socket and are not animated. The node has no effect mask.

What is counted:

- The whole data window of the picture. The measure does not depend on a region of interest that a
  frame asks for, on the display window or on the resolution's pixel aspect; a picture with a data
  window origin that is not zero is measured the same.
- A value that is not a finite number, or whose magnitude is above 1e12, is not counted. A data pass
  that marks "no data" with an infinity or a huge sentinel therefore measures only its data. A
  straight value whose division by a tiny alpha overflows is not counted either. Images hold finite
  numbers by construction, so these are the ways a non-finite value can arise.
- HDR and negative values are measured as they are; nothing is clamped.
- A picture with nothing to count (a fully transparent picture in straight color, for instance) gives
  0 for every output, including `count`.
- `count` is exact up to 16,777,216 pixels; a larger count is a binary32 sum.

## Which frame's statistics a node reads

A node that reads an Image Statistics output reads the statistics of the same frame: the picture at
the time being evaluated, at the resolution being evaluated, in the color of that evaluation. No frame
uses another frame's statistics.

The statistics are prepared before the frame is evaluated, on a worker, never on the interface thread
(`ImageStatisticsPreparation`, `image_statistics.hpp`), and kept in a bounded cache under an exact
identity: the route, the resolution, quality and color, the node, the plan and the exact time. A hit
needs the same plan, compared in full, not only the same revision, so an edit made while a value is
being dragged never reads the statistics of the picture before the edit.

An evaluation outside a worker's preparation (a thread with none installed) only reads the cache, and
the cache holds nothing for a frame it was not prepared for. The statistics are then reported as not
ready: every output reads 0 and the evaluation carries the diagnostic "Image Statistics are not
ready". The 0 is not a measured value, and a frame that depends on it fails with that diagnostic
instead of rendering with it. Nothing is cached for a measure that failed or was cancelled.

## The statistics plan

The compiler gives the node a plan of its own, `CompiledValueImageStatistics::plan`: the picture the
node's `input` ends in, followed by the reduction below, shown through the composition output. It is
compiled from the same snapshot (and the same live overrides) as the plan that contains the node,
and only when the node's value is reached. The picture is not part of the containing plan. A node
whose picture depends on its own result is refused at compile time.

The reduction is two passes of one grade kind (`StatisticsKernel`, `ColorGradeKind::Statistics`) and
one GPU program (`bloom.render.statistics.v1`), as Histogram's and Auto Exposure's reductions are:

1. Reduce cuts the picture into at most 32 by 32 tiles and reduces each tile, its pixels taken in
   row-major order, to its count, minimum, maximum, mean and sum of squared deviations from that mean.
2. Sum combines the tiles in row-major order (the parallel variance combination), and publishes two
   pixels: `(count, minimum, maximum)` and `(mean, variance, 0)`.

The CPU reference (`applyStatistics`) and the GPU program run the same binary32 operations in the same
order over the same table, so they agree exactly. The deviation is the square root of the binary32
variance, taken on the host in double precision, the same for every route. The result sits at the
origin of the composition, whatever the picture's own origin, so a route can ask for, and read back,
exactly those two pixels.

## Routes

The statistics plan is run for the frame by the route that is evaluating the frame, with the frame's
own time, resolution, quality and color, a request region of interest of just the two result pixels,
and the picture measured whole.

- CPU reference: the CPU evaluator runs the plan on the worker that is evaluating the frame.
- Device route (a final render through the GPU process frame evaluator: still, sequence, video and
  headless or scripted export): the same evaluator runs the plan on the device. The picture, both
  passes and the two moves are device work in one resident scene. The only readback is the result,
  two pixels, read back once at the end of that plan. The picture itself is never read back, and no
  per-node or per-operation full-frame roundtrip exists. The result is labelled with the route that
  measured it (`ImageStatisticsRoute::Device`), and the cache keeps CPU and device results apart, so
  a CPU result is never reported as a device result.
- A route that cannot run the plan reports it. A scene built where no preparation is installed (the
  interactive viewer's device preparation, which does not run the plan) is declined as unsupported
  with the reason "Image Statistics are measured by the CPU reference route here", and the frame
  takes the CPU reference path, which measures the statistics itself. That frame is labelled as a CPU
  reference frame. An unavailable device is reported by the evaluator as unavailable, as for any
  other frame.

The nested run is the one reentrant call the GPU process frame evaluator permits: the statistics plan
of the frame being prepared, run by that frame's own preparation on the calling worker before the
frame's scene is queued. It is admitted like any request and cannot wait on the frame it serves.

### The statistics readback (owner decision, 2026-11-09)

A statistic has to reach the value graph from a frame's pixels, and a frame's value graph is resolved
before its scene is built. The owner approved one small named readback for this: a bounded readback
of the statistics result only, on any device route, and never a full-frame readback or a per-node
full-frame roundtrip. It is the coverage contract entry `prep.statistics.result_readback`
(`gpuApprovedCpuPreparation()` in `gpu_coverage_contract.hpp`), disposition
`ApprovedCpuHostPreparation`, owning document this one. It is narrow: it does not widen any pixel
opt-out, tolerance or gate. The whole picture is still GPU work, the reduction is still a Required
pixel operation (`effect.StatisticsKernel`, and the `node.bloom.image-statistics` lowering), and the
final composited image still reads back once at the codec boundary under `prep.export.final_readback`.

## Cache identity, cancellation, budget and retirement

- Identity: see above. The plan's tables are part of the cache identity of each pass, so a changed
  channel, space or pixel choice re-measures.
- Cancellation: the pass checks cancellation while it reduces and publishes no image when cancelled;
  a cancelled measure is not cached and is reported as cancelled.
- Budget: the two passes are charged against the frame's pixel budget like any pass (a tile row is
  two pixels, so the working set is tiny); a budget too small for the result is refused. The cache
  holds at most 4,096 results (a few dozen bytes each) and retires the least recently used.
- Retirement: purging decoded media clears the cache (`clearImageStatistics`).
- Cold and warm: a cold frame measures once per node and time; a warm frame, or any other output of
  the same node in the same frame, reads the cache and runs nothing.

## Evidence

`bloom.runtime.image-statistics` (CPU reference, known answers, tile independence, refusals,
cancellation, the cache and the value graph), `bloom.runtime.gpu_scene_effect_native` (CPU and
device parity of the passes at the strict gate with exact alpha, windows, resident dispatches, warm
zero-dispatch reuse, and the device route end to end through the GPU process frame evaluator), and
the coverage fixtures `effect.StatisticsKernel` and `node.bloom.image-statistics`. The benchmark case
`image-statistics-2k` is written and has not been run.
