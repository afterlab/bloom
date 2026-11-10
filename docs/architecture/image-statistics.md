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
- `count` is exact for every picture the engine can hold, on every route: the Sum pass carries it
  as two whole binary32 numbers (below), so a count above 16,777,216 (2^24, where binary32 stops
  holding every whole number) is not rounded.

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
   pixels: `(count, minimum, maximum)` and `(mean, variance, residual)`.

The count of a tile is a whole number, and the Reduce pass counts it exactly up to 2^24. A tile is a
32nd of the picture each way, so a tile holds more than 2^24 pixels only in a picture of more than
2^34 pixels (256 GiB of RGBA binary32 pixels), which no pixel budget admits. The
Sum pass carries the running count exactly as `high * 2^23 + low`, two whole binary32 numbers, adding
each tile's count in two parts with a carry; every step is exact. Beside it, the count rounded to
binary32 (`high * 2^23 + low`, correctly rounded once) is what weighs the mean and variance
combination and divides the variance, as the running binary32 count did before: up to 2^24 it is the
exact count and the passes' bits are unchanged, and above it the mean and variance depend on the count
only through that weight, the correctly rounded one, so they keep binary32 accuracy. Pixel 0 holds
that rounded count; the residual in pixel 1 is the whole number it leaves out (exact in binary32), and
the host adds the two in double precision for the exact count.

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
- Interactive viewer and RAM preview (decision, 2026-11-10): the preview's GPU scene stage
  (`makeCompositionPreviewGpuSceneStage`) is handed a device measure
  (`ImageStatisticsDeviceMeasure`). While the scene builder resolves the frame's value graph, before
  the frame holds a processing phase lease, the stage installs a preparation for the frame under its
  own Device identity, and each statistics plan is measured by the application's device route: the
  application's GPU final-render evaluator (`GpuExportProvider`), through
  `makeImageStatisticsDeviceMeasure` and `measureImageStatisticsOnDevice`. That is the same device
  route as a final render, with the same resident scene, the same two-pixel region and the same one
  two-pixel readback under `prep.statistics.result_readback`. The preview's preparing worker is not
  that evaluator's owner, so the measure cannot wait on the frame it serves. The measure carries the
  frame task's cancellation, so a viewer request superseded by a newer one cancels it, and a cancelled
  measure is not cached. The frame is then
  prepared on the GPU and shown as a GPU-resident frame; RAM preview frames take the same stage, so
  each frame of a range is measured once and a warm range measures nothing.
- A route that cannot run the plan reports it. When the device measure is unavailable (no evaluator
  yet, an unavailable device, a refused request, or values not measured on a device), the viewer
  stage never prepares the frame with unmeasured values: it returns the frame as outside the GPU
  subset with the diagnostic `bloom.preview.gpu-scene.statistics-unavailable` and the reason, and the
  frame takes the CPU reference path, which measures the statistics itself and is labelled as a CPU
  frame. A scene built where no preparation is installed at all (a preview stage without a device
  measure) is declined as unsupported with the reason "Image Statistics are measured by the CPU
  reference route here", with the same CPU path. An unavailable device is reported by the evaluator
  as unavailable, as for any other frame.

Denoise & Smooth's Auto strength (#514, `image-filters.md`) is a second user of this route: its
lowering builds a statistics plan of the node's own input cropped to its sample region and reads the
deviation as a compiled value, with the same preparation, cache, cancellation, budget, readback and
viewer route and fallback as the node.

The nested run is the one reentrant call the GPU process frame evaluator permits: the statistics plan
of the frame being prepared, run by that frame's own preparation on the calling worker before the
frame's scene is queued. It is admitted like any request and cannot wait on the frame it serves. A
preview frame's measure is not nested: it is an ordinary request from the preview's worker.

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
cancellation, the cache and the value graph, and the Sum pass alone over tables of tiles counting
16,777,219, 25,165,833 and 50,331,650 pixels: the count exact, the extremes exact, pixel 0 the
binary32 count, mean and deviation to binary32 accuracy against a double precision measure),
`bloom.runtime.gpu_scene_effect_native` (CPU and device parity of the passes at the strict gate with
exact alpha, windows, resident dispatches, warm zero-dispatch reuse, the same three tables on the
device with the CPU reference's bits exactly and the exact count read back, and the device route end
to end through the GPU process frame evaluator), `bloom.ui.composition_preview_gpu_scene_stage`
(the viewer stage with a device measure: the frame prepared on the GPU and measured once for its own
time, a cached frame not measured again, an unavailable or CPU-labelled measure taking the CPU path
with `statistics-unavailable` and caching nothing, a cancelled measure caching nothing, and no
measure at all taking the CPU path), `bloom.ui.gpu_ram_preview_image_statistics` (Image Statistics
driving Levels and Denoise with Auto strength in the interactive viewer and RAM preview on the real
service and the application's device route: GPU-resident frames against the CPU oracle, every 4th
pixel within one code, each frame's statistics measured once on the device and equal to the CPU
reference's exactly, a warm replay measuring nothing, an edit measuring every frame again, and a
refused device route giving a CPU frame with the CPU reference's pixels), and
the coverage fixtures `effect.StatisticsKernel` and `node.bloom.image-statistics`. The benchmark case
`image-statistics-2k` is written and has not been run.
