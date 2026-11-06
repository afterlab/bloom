# Motion blur

Status: implemented (#484). This is the specification of the temporal-sampling engine and of every
motion-blur node. How to use them is in the
[Motion blur user guide](../user-guide/motion-blur.md); what they cost is in
[Benchmarks, "Motion blur"](../development/benchmarks.md#motion-blur-484).

## One engine

Every node here shares one shutter model, one set of sample tiers, one seeded jitter and one kernel:

| Node | Input contract | How it blurs |
|---|---|---|
| Motion Blur (sub-frame) | any upstream graph | renders the animated upstream at each sub-frame time and accumulates |
| Transform 2D, Motion blur on | its input | one resample of its input per sample, along the path of its own matrix |
| Directional, Spin, Zoom Blur | any image | the same path sampler with a line, an arc or a ray |
| Vector Motion Blur | beauty, vectors, optional depth and matte | a tile gather or a soft z-ordered splat along the vectors |
| Optical Flow | footage, optional guide and matte | classical flow: forward, backward, confidence, occlusion |
| Optical Flow Motion Blur | footage, optional guide and matte | four solves, then the splat, per layer with a matte |
| Echo (Frame Blend, Trails, Posterize Time) | any upstream graph | the same accumulator over a temporal window |

The GLSL in `src/runtime/motion_kernel.glsl` is compiled twice: as C++ for the CPU reference and as a
compute shader for the GPU. A backend-neutral pass plan (`motion_blur_planner.cpp`) lays out each
pass's atlas and parameter table; the CPU interpreter (`applyMotion`) and the GPU emission
(`gpu_scene_motion_emission.ipp`) run the same plan. There is therefore one implementation of every
pass.

The GPU compiles one program per pass (`motionProgramText`), each a constant pass id over the same
text, so the compiler prunes every other pass. The whole kernel as one program took the driver 13 s on
its first use; a pass takes 0.18 s (the path sampler) to 2.6 s (DIS), and only the passes a node uses
are compiled.

Corner Pin does not exist in Bloom, so the "Transform/Corner Pin" row of #484 is Transform 2D alone.
When a Corner Pin node is added it takes the same path sampler.

## The shutter

Identical everywhere (`motion_shutter.hpp`, `motion_blur_parameters.hpp`):

- **Shutter angle** in degrees; 360 degrees is one frame, 180 degrees is 0.5 frames (RSMB's 0.5 and
  a layer-based compositor's 180 degrees).
- **Shutter opens** Centred (the default), Start on frame, End on frame or Custom with a phase in
  degrees. A layer-based compositor's phase is `-angle/2` for Centred.
- **Shutter curve** Box (the default), Triangle, Gaussian or Custom (a curve on the card). Each
  stratum's weight is the curve's mean over the stratum, normalised to sum 1.
- **Quality** tiers: Draft 4 samples, Standard 16, High 64, Final adaptive: the smallest power of two
  at least the widest displacement in pixels, between Minimum samples and Maximum samples (default 8
  and 64) each rounded up to a power of two. That is at most one pixel per step until the maximum.
- **Jitter** is off by default and seeded by a hash of the node id, the frame and the pixel only, so a
  jittered frame is reproducible on every device.
- **Maximum blur**, in pixels, default 512 (hard cap 4096).
- Accumulation is in linear premultiplied float, so alpha and HDR values add correctly (Porter and
  Duff 1984). Colour is not clamped; alpha is clamped once, at the end.

Sample times are rationals derived from the composition's frame rate, in a fixed order. Nothing in the
sum depends on scheduling.

## Sub-frame accumulation

`appendTimeSamples` in `motion_blur_recipes.cpp` lowers Motion Blur to one upstream evaluation per
sample at a time offset. The existing layer time scopes (`scopeLayerTime`) hoist everything that does
not depend on time, so a static branch of the upstream is evaluated once however many samples there
are, and only the animated parts run again. Each evaluation goes through the production operations,
so it needs no separate GPU path, and the accumulator is a Required kernel with CPU/GPU parity.

Footage between frames has four modes, applied to each footage source upstream (Bloom knows which
parts are footage, which a plug-in that sees flattened pixels cannot):

- **Automatic** blurs a source by its vector channels when it has a layer with the name in **Motion
  vector layer**, and otherwise holds it.
- **Hold** keeps footage at its frame while the animated parts of the upstream move: exact for motion
  graphics and CG layered over a plate.
- **Optical flow** replaces each footage source with the Optical Flow Motion Blur recipe on it,
  through the same shutter.
- **Motion vectors** replaces each with the Vector Motion Blur recipe, its vectors read from the named
  layer; a source without one fails with a diagnostic.

Footage inside a retimed layer is not held: the retime's own sub-frame times apply.

## The path sampler

Directional, Spin, Zoom and Transform 2D's shutter are one pass: a source image, per sample one matrix
from output to input, the samples' weights, bilinear filtering. Curved paths are exact because each
sample evaluates the matrix at its own time rather than interpolating two end matrices; Transform 2D
evaluates its rows with `sampleParametersAt`. Directional, Spin and Zoom's operands are read at the
frame time; only their result moves. A Directional Blur's length is its full reach.

## Vector Motion Blur

### Conventions

Decoding a vector image needs its channels, scale, offset, the sign of Y, its layout (one vector,
forward and backward, or previous and next in one image), its units (pixels or normalised) and whether
the third channel marks valid vectors. Each convention is a preset of these, and **Custom** exposes
them:

| Convention | Layout | Units | Y | Notes | Source |
|---|---|---|---|---|---|
| Bloom | one vector | pixels | down | Optical Flow's output | this document |
| Forward and backward | forward and backward | pixels | up | `forward.u/v`, `backward.u/v` | [S10] |
| RSMB | one vector | pixels | up | float value times MaxDisplace (default 2048); the third channel full on marks a valid vector | [S5] |
| Previous and next | previous and next | pixels | up | `Vector` pass; RG from the previous frame, BA to the next | [S16] |
| Arnold | one vector | pixels | up | `motionvector` AOV, raw | [S19, S20] |
| V-Ray | one vector | pixels | up | `VRayVelocity`, pixels over the frame | [S21] |
| Redshift | one vector | pixels | up | `MotionVectors` AOV, raw not normalised | [S23, S24] |
| Karma | one vector | normalised | up | `motionvector` AOV | [S25] |

The renderer presets are starting points: a scene's AOV settings can change scale, and S24 and S25 are
anecdotal. The two presets without a renderer's name are named for their layout, under the repository's rule on
naming commercial products in public copy. An integer-encoded RSMB file (values 0 to 1 around 0.5) uses Custom with a scale of twice
MaxDisplace and an offset of minus MaxDisplace. RE:Vision's format has alpha undefined under a valid
vector, so an RSMB alpha is not treated as coverage.

### Gather

The default at Draft and Standard, and always when chosen explicitly: McGuire, Hennessy, Bukowski and
Osman's reconstruction filter (I3D 2012) with TileMax and NeighbourMax, Guertin, McGuire and
Nowrouzezahrai's stabilisations (HPG 2014) and Jimenez's practical notes (SIGGRAPH 2014): sample along
the neighbourhood's dominant vector, weight by the depth-compared cones and the cylinder term. With no
depth input, depth comparison is neutral. Where no vector reaches a tile, the pass returns the input
pixel unchanged: it is exact there.

Two engineering choices serve a large **Maximum blur** (default 512 pixels):

- Tiles stay 32 pixels at most, whatever the limit, instead of McGuire's `K = maximum blur`. A 256-pixel
  tile would let one fast pixel take the samples of every slow pixel in up to nine tiles around it, and
  one tile would loop over 65 536 pixels in a single shader invocation, which at 1080p costs about
  100 ms however small the vectors are.
- NeighbourMax is one pass at tile resolution over every tile within the limit, not a 3 x 3 lookup, and
  keeps only the candidate whose blur actually **reaches** that tile. So a short vector's neighbourhood
  stays local when the limit is wide.

As in Guertin et al., alternate samples follow the pixel's own vector when it moves, so a pixel with its
own motion keeps it beside a faster neighbour. Their further directional weighting of those samples is
not implemented.

### Layered splat

The default at High and Final with Reconstruction set to Automatic: a forward splat with soft
z-ordering after Niklaus and Liu's softmax splatting (CVPR 2020), reimplemented from the paper (the
reference code is academic-only). Each output pixel integrates over the shutter's samples the sources
that reach it: `q = p - D(q)` is solved per sample by a fixed number of fixed-point iterations from a
seed, and the tents of those that land are weighted by soft depth.

**Deviation from #484:** the issue's row calls for fixed-point integer accumulation for determinism.
The splat here is evaluated as a gather per output pixel (no writes to other pixels, no GPU atomics),
and the float sum has a fixed order, so it is deterministic without an integer accumulator. Colour stays
linear float throughout, so large HDR values do not saturate a fixed-point range.

Curved paths are a quadratic through the previous and next vectors (Xu, Siyao, Sun, Yin and Yang,
"Quadratic Video Interpolation", NeurIPS 2019) when the node's **Curved motion** is on and both vectors
exist.

A matte separates a foreground (Wang and Adelson 1994): the matte's outside fills behind the
foreground outside-in, the foreground splats over the blurred fill, and they merge foreground first.
This is what a depth sort cannot do on its own with a pass that has no depth.

### Limits

- Output is clipped to the hull of the colour window and the display window.
- A pixel has one motion. **Transparency with two motions per pixel has no practical published
  solution** and is a documented limitation: split the layers and blur each.
- Vector Motion Blur at 1080p needs more than 256 MB of CPU cache budget; a smaller budget refuses with
  the budget diagnostic rather than degrading.

## Optical Flow

OpenCV is not in Bloom's locked prefix and could not be taken in under the
[dependency intake rules](dependency-intake.md), so **DIS and TV-L1 are reimplemented from the papers
here, as compute passes with a CPU reference**. No OpenCV code is used, as an oracle or otherwise. No
weights ship: RAFT and SEA-RAFT are research-only as trained and are not offered. NVIDIA's Optical Flow
SDK is rejected because it is NVIDIA-only.

The grey image of each frame is built with fixed weights and a fixed pyramid (a 2x reduction per level,
Sánchez Pérez et al.'s 5 level structure). Iterations are fixed, not converged, so the result is
reproducible and the cost is known:

| Quality | Method (Automatic) | Finest level | Warps x iterations |
|---|---|---|---|
| Draft | DIS (Kroeger, Timofte, Dai and Van Gool, ECCV 2016) | 1/4 | 16 inverse-compositional steps a patch |
| Standard | TV-L1 (Zach, Pock and Bischof, DAGM 2007; Sánchez Pérez et al., IPOL 2013) | 1/2 | 3 x 15 |
| High | TV-L1 | 1/2 | 5 x 25 |
| Final | TV-L1 | full | 5 x 30 |

Choosing a method by name overrides this: DIS at Standard stops at 1/2 and at High and Final at full
resolution; TV-L1 at Draft stops at 1/4 with 2 x 10. Below Final the flow is upsampled from a reduced
resolution, a cost decision: full-resolution flow at Standard would cost four times as much to refine
detail the blur then removes.

Outputs:

- **Forward** and **Backward** vectors in the Bloom convention, in evaluation pixels (so a proxy
  evaluation has proxy-scale vectors), Y down.
- **Confidence** and **Occlusion** from Sundaram, Brox and Keutzer's forward-backward check (ECCV
  2010): occluded where `|w + ŵ|² ≥ 0.01(|w|² + |ŵ|²) + 0.5`, confidence falling with the residual.
- **Cut**, a diagnostic: a whole-frame flow-residual threshold. This is an **uncited heuristic**, off
  by default and never read by a blur.

**Frame edges** are explicit: Extend (the default), Black or Wrap (360). This is engineering policy
with no paper. A **Guide** input solves the flow on another image; a **Matte** restricts the data term
to a layer (Wang and Adelson 1994; Sun, Sudderth and Black 2010).

Flow results are derived cache, not project data.

## Optical Flow Motion Blur

Each step has the cited basis of #484's table:

| Step | Technique | Source |
|---|---|---|
| Estimation | DIS at Draft, TV-L1 otherwise, as above | Kroeger 2016; Zach 2007; Sánchez Pérez 2013 |
| Consistency | forward-backward check | Sundaram 2010 |
| Integration path | linear per interval; quadratic through three frames at High and Final | Xu 2019 |
| Blur synthesis | integrate warps over the shutter | Brooks and Barron 2019 (baseline analysis) |
| Warp | forward splat with soft z-ordering | Niklaus and Liu 2020 |
| Bidirectional blend | visibility-weighted blend of the previous and next warps | Jiang et al., Super SloMo, CVPR 2018 (the formulation, not the network) |
| Hole fill | outside-in occlusion-aware fill | Baker, Scharstein, Lewis, Roth, Black and Szeliski, IJCV 2011 |
| Layers | per-layer flow, composite | Wang and Adelson 1994; Sun, Sudderth and Black 2010 |
| Alpha | premultiplied accumulation; alpha-aware foreground and background | Porter and Duff 1984; Jimenez 2014 |

**Amount** is the fraction of a frame: 0.5 is 180 degrees (RSMB's mapping). **Matte shrink** erodes the
matte before separating, and **Alpha boost** scales the blurred foreground (premultiplied, so its
colour and alpha together; the merge clamps alpha), so an edge that went thin in the blur can be
recovered.

With a matte there are eight solves: four per layer, with the back-solves reading the matte sampled at
the neighbour frame so a moving matte stays on its layer. The foreground splats over the background.
The background behind the foreground fills **outside-in** by default (Baker 2011). **Temporal fill**
through the background flow is opt-in: RSMB Pro does it but its method is unpublished, so it has no
citable full method and is not a default.

The node publishes the flows it solves on **Forward**, **Backward**, **Confidence** and **Occlusion**
when something reads them.

## Echo

The accumulator over a temporal window: count, interval in frames, starting intensity, decay and an
operator (Add, Maximum, Minimum, Screen, Over, Under). **Frame blend** mode is the box average of the
window. **Posterize time** holds the upstream's time at a lower rate before sampling. A sample outside
the composition is absent, and the sum is over what exists.

## Parity, determinism and identity

Every pass is a Required kernel in `gpu_coverage_contract.hpp`, with no exemption and no host
preparation. `gpu_coverage_contract_tests.cpp` holds 5 effects, 18 `feature.motion.*` axes and 8 node
types to production GPU preparation, and `gpu_scene_effect_native_tests.cpp` runs every scene on the
device against the CPU reference.

The motion kernel is **bit-exact** between the CPU reference and the GPU on bit-identical inputs.
Colour parity is 2e-6 and alpha exact in the fixtures. **A limitation is stated plainly here:** nodes
upstream of a solver (a Gaussian blur, a gradient) are only within 2e-6 of their CPU reference, and an
iterative solver amplifies such a difference. So parity of Optical Flow and Optical Flow Motion Blur
is proven on bit-identical inputs (solids, whole-pixel moves, opaque merges, the checker); over an
upstream that is merely within tolerance, a flow solve can differ by more than 2e-6 between devices,
and through a matte's layers the alpha can differ too: `motion-blur-flow-2k`'s export differs from its
CPU reference by at most 0.0093 in colour, in 4538 components of its frame, and in 13973 alpha samples.
This is the flow-amplification the issue warned about. A cross-device final render should render on
one device class.

Cache identity covers every kernel value, the shutter, and the time offsets: a changed value dispatches
again, an unchanged frame runs nothing. Cancellation and budgets go through the same evaluator as every
other effect; each pass allocates through the frame budget and refuses with the budget diagnostic.

## Memory

- Only an effect's output goes to the scene cache. A solver writes hundreds of frame-sized atlases that
  nothing can ask for again (`transientResult` on the pass's commands); offering them to the cache cost
  seconds at 1080p and would evict what is worth keeping.
- The accumulator's atlas takes the new sample as its base and blends the running sum onto it. The GPU
  planner visits a blend's source first, so each step resolves the chain before rendering its own sample:
  a frame holds a running sum and one sample at a time, not every sample of the shutter (64 samples at
  2160p would otherwise hold 8 GiB).
- A solver's atlas holds several frames and is one image: at 2160p it exceeds the executor's 768 MiB
  default per-image ceiling, which production raises to the request budget.
- A frame allocates thousands of images, and most of a heavy frame's cost on the unrooted route is the
  driver allocating them. The preview route takes each back from the frame before (#477), which removes
  that cost; the final-render route does not reuse storage, so its frames in these modes cost two to
  nine times as much ([measurements](../development/benchmarks.md#motion-blur-484)).
- Storage reuse holds far more than a frame's live bytes: each of a solver's atlas shapes keeps as many
  images as a batch held at once, so a flow blur at 2160p with 4.5 GB live parks more than 12 GiB. The
  preview route frees parked storage when work waits for room. An image that nothing makes room for
  lands in host memory, and frames then take seconds.

## Project format

The motion-blur nodes are new types. Transform 2D stays at schema 1 and gains `motionBlur` and the
shutter's parameters as optional bindings: opening an older project binds them at their defaults (off),
so its picture is unchanged. See [Project format, "Transform 2D Motion Blur"](project-format.md#transform-2d-motion-blur).

## Sources

The references are those of #484. The papers were cited from their published forms; the [S] entries
are the issue's source list.

- Baker, Scharstein, Lewis, Roth, Black, Szeliski. "A Database and Evaluation Methodology for Optical
  Flow". IJCV 2011.
- Brooks, Barron. "Learning to Synthesize Motion Blur". CVPR 2019.
- Guertin, McGuire, Nowrouzezahrai. "A Fast and Stable Feature-Aware Motion Blur Filter". HPG 2014.
- Jiang et al. "Super SloMo". CVPR 2018.
- Jimenez. "Next Generation Post Processing in Call of Duty: Advanced Warfare". SIGGRAPH 2014.
- Kroeger, Timofte, Dai, Van Gool. "Fast Optical Flow using Dense Inverse Search". ECCV 2016.
- McGuire, Hennessy, Bukowski, Osman. "A Reconstruction Filter for Plausible Motion Blur". I3D 2012.
- Meister, Hur, Roth. "UnFlow". AAAI 2018.
- Niklaus, Liu. "Softmax Splatting for Video Frame Interpolation". CVPR 2020.
- Porter, Duff. "Compositing Digital Images". SIGGRAPH 1984.
- Sánchez Pérez, Meinhardt-Llopis, Facciolo. "TV-L1 Optical Flow Estimation". IPOL 2013.
- Sun, Sudderth, Black. "Layered Image Motion with Explicit Occlusions...". NeurIPS 2010.
- Sundaram, Brox, Keutzer. "Dense Point Trajectories by GPU-Accelerated Large Displacement Optical
  Flow". ECCV 2010.
- Wang, Adelson. "Representing Moving Images with Layers". IEEE TIP 1994.
- Xu, Siyao, Sun, Yin, Yang. "Quadratic Video Interpolation". NeurIPS 2019.
- Zach, Pock, Bischof. "A Duality Based Approach for Realtime TV-L1 Optical Flow". DAGM 2007.
- [S5] RE:Vision motion vector format FAQ, <https://revisionfx.com/faq/motion_vector/>.
- [S10] Foundry's VectorBlur node reference, <https://learn.foundry.com/nuke/content/reference_guide/filter_nodes/vectorblur.html>.
- [S16] The Vector Blur compositor node manual, <https://docs.blender.org/manual/en/latest/compositing/types/filter/blur/vector_blur.html>.
- [S19, S20] Arnold's Motion Vector AOV documentation.
- [S21] V-Ray Velocity element documentation.
- [S23, S24] Redshift motion blur documentation; a community note on raw vectors (anecdotal).
- [S25] SideFX forum, motion vector AOV in Karma (anecdotal).
