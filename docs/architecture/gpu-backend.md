# GPU Backend Architecture

Status: working

Updated: 2026-11-02

## Purpose

Bloom should use the GPU for interactive compositing, display processing, and other parallel work
without making project semantics depend on a vendor, operating system, or UI toolkit. The CPU
reference evaluator remains the correctness oracle. GPU execution is an acceleration strategy over
the same immutable evaluation request.

The initial working direction is one Vulkan renderer on Linux and Windows and the same Vulkan code
through MoltenVK on macOS. This is not accepted until the cross-platform spike in this document
passes.

## Decision Summary

- Target Vulkan 1.2 as the provisional baseline API.
- Use native Vulkan drivers on Linux and Windows and bundle a pinned MoltenVK build on macOS.
- Use Vulkan-Hpp for typed C++ bindings and RAII handles. Wrap Vulkan Memory Allocator behind
  Bloom-owned resource types rather than exposing it through public render interfaces.
- Author Bloom GPU kernels as Vulkan GLSL and compile them to SPIR-V. Compile shipped kernels
  offline; compile generated OCIO shader programs off the UI thread and cache the resulting modules
  and pipelines.
- Keep a Bloom-owned backend interface between the evaluator and Vulkan. It represents Bloom
  operations and resources, not a second generic graphics API.
- Keep presentation separate from offscreen evaluation. Qt may create windows and surfaces, but Qt
  types do not enter `src/render` or `src/runtime`.
- Never wait for GPU work, compile a shader or pipeline, upload a full frame, or destroy a busy GPU
  resource on the UI thread.
- Expose one Bloom-owned immutable capability report. Device initialization establishes only a
  hardware/bootstrap baseline; `Unavailable`, `PreviewOnly`, and `ReferenceParity` are
  per-operation-and-precision outcomes, not a device-wide marketing tier.
- Qualify a closed, versioned operation vocabulary against the CPU fixtures before the scheduler may
  select it. The render capability report's `GpuOperationId` vocabulary is `SolidV1`,
  `TranslationOpacityBilinearV1`, `SourceOverV1`, `OcioDisplayV1`, and `PackedDisplayV1`; the report
  advertises no operation until its frozen fixtures pass. The shared scene executor's additional
  typed operations (covered solid and native coverage, affine, typed blend, point resample, OCIO
  effect) are gated by the runtime operation/route coverage contract and their own native parity
  fixtures rather than by a capability-report entry.

The Vulkan specification describes explicit graphics and compute control, while MoltenVK implements
a Vulkan subset over public Metal APIs and converts SPIR-V to Metal Shading Language. MoltenVK also
documents known portability limits, so support is capability-driven rather than inferred from an API
version alone. [Vulkan specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html),
[MoltenVK documentation](https://github.com/KhronosGroup/MoltenVK/blob/main/README.md)

## Implementation Status

Status vocabulary: `implemented` means present in the repository and locally exercised on Linux;
`pending` means defined but not yet built or qualified. Nothing below makes a Windows or macOS GPU
claim. This section is the current ownership/behavior/gate summary.

Capability surface and device bootstrap. A Qt-free and Vulkan-free `bloom::render` public surface
(`bloom/render/gpu_device.hpp`) exposes typed device state, per-operation-and-precision
`GpuQualification`, ordered structured diagnostics, an immutable generation-scoped capability report,
and opaque move-only buffer ownership; no `Vk*` type, native handle, or catch-all backend interface is
public. An optional Vulkan bootstrap resolves `Vulkan::Headers` and
`GPUOpen::VulkanMemoryAllocator` only from the validated dependency prefix in `qualified` mode;
absent Vulkan, or when disabled, the same API is a CPU-unavailable stub that reports a typed
`BackendNotBuilt` diagnostic, with no Vulkan header reaching the stub, the desktop application, or a
public consumer header. Device probing checks Vulkan 1.2, `timelineSemaphore`, and a compute-capable
queue family before use; a missing loader, device, or entry point produces a typed `Unavailable`
diagnostic instead of a startup crash. A privately opened dynamic loader (platform loader name only)
and a Vulkan-Hpp RAII owner host one bounded host-visible allocation path (64 MiB ceiling) that proves
the allocator is live; outstanding allocations co-own the allocator generation, and creation,
exposed operations, and destruction are owner-thread-bound. The capability report advertises no
operation: every operation/precision stays `Unavailable` until the frozen fixtures pass, and an
unsupported entry is never inherited from another operation.

Native render primitives. Their public headers are Qt-free and Vulkan-free with portable
CPU-unavailable stubs. `GpuSolid` runs `SolidV1` and the covered arm `CoveredSolidV1` (a host-built
premultiplied palette from the existing CPU coverage primitive, bit-exact on readback). `GpuImage`
owns a device-resident RGBA32F image; `GpuImageUpload` stages one already-decoded host `Rgba32fImage`
with no media decode on the GPU thread (a staging copy of more than one 8 MiB chunk runs through the
caller's optional `parallelFor`; the scene executor passes the scheduler's row-band executor via
`GpuSceneExecutorBudgets::uploadCopyExecutor`, so a many-layer frame's uploads no longer serialise
on one single-threaded memcpy on the owner thread, issue #401; a proven-retired job begun with
`reuseStagingWithinScene` keeps its staging buffer and transient half image as spares that the next
opted-in upload under the same processing phase reuses when large enough and still within the
request budget; only the scene executor opts in, releasing them when its scene ends, so a frame
allocates one or two staging buffers instead of one per layer, while every other upload releases its
staging at its fence so its phase and memory grant retire with the job; a source whose every value is
exactly representable in binary16, such as EXR half, crosses as RGBA16F, half the bytes, into a
transient image that one same-extent `VK_FILTER_NEAREST` blit widens into the RGBA32F resident image,
which is exact, so the resident bytes and every cache identity are unchanged; any inexact value, an
upload queue without graphics capability, missing blit format support or a half image that does not
fit the request keeps the RGBA32F copy); `GpuComposite` runs `TranslationOpacityBilinearV1` and
`SourceOverV1`, each writing a new resident image; `GpuResidentDisplay` copies a resident RGBA32F
image device-to-device through the embedded Bloom Neutral V1 shader into a resident packed RGBA8
image, reading back only the 4-byte status word; `GpuNeutralDisplay` runs the fixed `OcioDisplayV1`
compute operation. `GpuPathCoverage` is the vector coverage producer: the CPU builds the immutable
`PathRasterCoverageGeometry` (bounded integer scanline sample spans, O(edges x rows), with no
per-pixel mask and the exact `coverageRow` boundary comparison preserved through a bounded binary
search over the real quarter-sample positions), and one pure-integer compute dispatch turns those
spans into the exact 8-bit coverage mask `PathRaster::coverageRow` produces, retained device-resident
as a packed little-endian R8 storage buffer. `GpuSolid::beginCoveredResident` then binds that resident
buffer directly for the covered fill, with no download or re-upload; the coverage buffer is
co-owned until the consuming submission is proved retired, and the consume path validates owner
thread, exact device identity, and generation before readiness or dimensions. In-flight submissions
are serialized by one process-wide bounded reservation claimed before any native allocation: a
proven retirement returns it, an unproven one moves the exact submission into an owner-only
quarantine, and admission is refused while occupied: `Busy` when the same owner thread holds it,
`ReservationHeld` when another device owner's live submission does (#553; the same two codes
apply to the `GpuCryptomatteCoverage` and `GpuPointResample` reservations). The failed producer keeps its exact
reservation token and device identity: `hasUnretiredSubmission()` remains true until owner polling
proves that quarantine retired. Failed polling never publishes a mask, and a stale token cannot
retire a later job. Native access also checks the exact owner thread lifetime. Destroying a
quarantined producer orphans its existing resident slot so descriptors and pipeline outlive the
submission; non-blocking owner drain releases that slot only after real fence completion or actual
driver device loss. Injected loss diagnostics require real fence completion before cleanup. The
quarantine fuse records historical unproved retirement, independently of current slot occupancy.
Every producer native resource set (pipeline
plus resident mask) holds one of a fixed number of process-wide resident slots, acquired before its
first native allocation and held until owner-thread retirement; creation is lazy, so idle producers
allocate nothing. A producer destroyed on a foreign thread only marks its already-owned slot
orphaned (ownership is preserved, never destroyed off-thread); the owner drain retires orphaned
residents allocation-free and returns their slots, and it gates every fence query and teardown on
the exact owner thread and generation, so one device thread can never touch another's orphan. It
therefore refuses cleanly when the pool is full and recovers once the rightful owner drains. The
dispatch is a flattened 2D grid bounded by
the real device workgroup-count limits, so a capacity-valid large geometry is not refused by a 1D
grid. The kernel requires no Float64/Int64 capability, and a configure-time disassembly rejects
either. Every shipped Bloom-authored kernel is an offline artifact under `tools/gpu-shaders`, pinned
by SHA-256 with a manifest binding and a configure-time `glslangValidator`/`spirv-val` regeneration
check against the embedded SPIR-V digest, so the source -> SPIR-V -> embedded-array relationship is
closed and no runtime code loads or compiles a Bloom kernel. OCIO-generated shader text is the
separate runtime-compiled path, described under "Shaders And OpenColorIO". `GpuAffine` runs
`AffineBilinearV1`, a complete composed affine (rotation, uniform/nonuniform/negative scale, anchor,
translation, and any precomposed parent matrix) over one resident image; the host uploads one compact
O(1) affine map and the kernel reconstructs each source-local coordinate on the device with portable
split-precision arithmetic that does not require a device float64 or 64-bit integer capability, so
no O(width*height) host metadata loop and no host pixel resampling occurs. That double-float
reconstruction is the definition of the sample coordinate (#545): the CPU reference of the
composed-matrix form, `render::affineMatrixResampleRow()`, which every production CPU affine
resample uses, runs the same probe, the same Float32 (hi, lo) split and the same error-free
two-sum/two-product steps, so CPU and GPU frames agree bit for bit on all four components, alpha
included, on every platform. A binary64 device path would not be portable (Metal through MoltenVK
has no float64, and consumer GPUs run it slowly). The LayerTransform form, which no production
route uses, keeps the binary64 `layerTransformBilinearRow()` oracle and the 2e-6 abs-or-rel gate. `GpuBlend` runs `BlendV1`
for any `core::BlendMode` over two resident images, and `GpuPointResample` runs `PointResampleV1`,
the bit-exact nearest-neighbour gather the media-image proxy path uses. `GpuImageUpload` also
uploads data channels (`render::ChannelSetImage`, #549): up to four planes become the R, G, B and A
lanes of a resident RGBA32F data image, interleaved into the staging buffer with their bits as they
are (never as binary16); a Cryptomatte rank layer crosses this way, one frame-sized upload per layer
that every matte of the type reads. It also
accepts decoded media in its native sample layout (`render::NativeSampleImage`, #422): 8-bit 4:2:0
or 16-bit 4:4:4:4 YUV(A) planes, packed 16-bit RGBA, or packed binary16 RGBA. The planes are copied
as they are into one storage staging buffer and `SampleConvertV1` widens them on the device to the
resident RGBA32F working image: the YUV matrix and range, the 4:2:0 chroma filter, bit-depth
expansion, the binary16 decode with the OpenEXR ingest repair policy, alpha association and the
#381 stored-samples encoding. `render::convertNativeSamples` is its CPU reference and reproduces the
decoders' own CPU conversions value for value. Every division (by a code range or by alpha) is an
exact integer long division of the significands that rounds to nearest even, and every other step
is one correctly rounded add, subtract or multiply marked `precise`, so the config-managed route is
bit-exact against the CPU reference; only the legacy BT.709 and sRGB inverse curves (transfer 1, 6
and 13), which use `pow` and which the config-managed route never selects, are held to the 2e-6
tolerance. The upload's cache identity folds the decode identity, the kernel's SPIR-V digest and
every conversion parameter. `GpuProcessReadback` and
`GpuOutputColorReadback` are the single bounded, cancellable device-to-host transfers reserved for
the final export boundary, never the preview path. All primitives share one bounded policy:
owner-thread operations, explicit byte budgets checked against actual VMA allocation sizes and the
transient staging peak with overflow-safe arithmetic, cancellation, device-binding checks, and a
bounded teardown that quarantines an unproved submission rather than destroying it in flight.

Initialization failure cleanup (G2A). The shared Solid base pipeline and Upload command resources
reset native children before their descriptor/command pools, matching reverse member declaration
order. Composite, Affine, and Blend already follow that order. Their private native regression
fixtures now fail before every Solid/Upload initialization API and after the complete native set
exists, run the actual lazy `begin` cleanup, and retry the same owner successfully. The fixtures
assert exact native-object creation/destruction and slot returns, no submitted work or published
image on failure, and baseline live VMA allocation count/bytes after failure and retry destruction.
The existing bounded pool, foreign-owner isolation, unproven-fence retention, and cancellation
fixtures remain in place. Linux native core/synchronization validation and Solid, CoveredSolid,
Upload, Composite, Affine, and Blend parity pass locally; this does not qualify Windows/macOS GPU
execution. Native-object counts and live VMA allocations do not measure driver-reserved memory or
reusable committed allocator blocks. Metadata staging lifetime, foreign-thread image-handle release,
and broader submission retirement remain subsequent G2 work.

Qualification. `bloom/runtime/gpu_neutral_display_qualification.hpp` runs the native device/pipeline
on the owner thread, refuses any processor other than the exact default Bloom Neutral v1 handle
(canonical identity byte comparison plus processor cache ID, OCIO version, and display/view
provenance), and executes real `begin`/`poll`/`readback` at the production 65536-pixel chunk. Parity
holds to the frozen contract (RGB within one straight-RGBA8 code, alpha exact) over 1 px, an odd
257-pixel tail, alpha endpoints, quantization-adjacent samples, and signed/HDR and tiny-normal
values; a nonzero subnormal frame is measured as whole-frame shader rejection and remains a per-frame
CPU fallback, never a parity failure or a claimed supported domain. Timing measures a warmup plus
alternating pairs, and the eligible interval is the contiguous faster suffix ending at 4K, so an
unmeasured or slower 4K leaves the operation CPU-only, and no measured speedup is claimed. The
qualification outcome is `PreviewOnly`; it is not selected for final output.
`gpu_resident_preview_qualification.hpp` reuses the caller's already-created typed
pipelines, refuses any pipeline not bound to the exact actual device (`GpuDevice::ownershipEpoch()`
is the unique ownership identity), and compares every pixel against the CPU primitives and the
independent OCIO oracle; its report is constructed only by its private factory and is `PreviewOnly`
only. Resident timing samples and the faster suffix are advisory telemetry: successful parity and
capability qualification admit resident requests without a timing-derived minimum image area. The
existing resident 4K profile ceiling and actual native geometry, byte and ownership checks remain.

Scene preparation, caches, and executor. `CpuGpuSceneBuilder` (`prepared_gpu_scene.hpp`) turns a real
`CompiledCompositionPlan` plus an `EvaluationRequest` into ordered immutable `GpuSceneCommand`s
(solid, covered solid and native vector coverage, translation, affine layer transform, source-over
and typed blend, OCIO color/effect transform, point-resample proxy gather, image/video upload,
nested composition, merge, and composition output) using the evaluator's real preflight resolution
and the exact resolved operands; it allocates no full RGBA CPU image and fails closed `Unsupported`
for every reachable operation it cannot prepare. A vector source emits the
immutable bounded `PathRasterCoverageGeometry` (integer scanline spans, never a host per-pixel mask)
that the executor rasterizes through the native `GpuPathCoverage` producer; scene integration keeps
that producer's device provenance and dispatch counters, so the Required
`feature.geometry.vector_coverage` axis is carried by real GPU work rather than a host mask. A Text
layer whose composed matrix is an identity linear map on the integer device grid keeps the CPU
reference's leaf form: the glyph coverage over its own bitmap window, then one translation by the
composed matrix's translation, parents included, so a parented text layer moves exactly as its
CPU reference does. A Shape, Text or Solid source read as an image rather than through a Layer (an
image effect's input, or an entry of a Merge such as Glow's or Drop Shadow's) is materialized as
the CPU reference's leaf image: a shape is rasterized by `PathRaster::create` at the proxy scales
(the layer arm's `PathRaster::transformed` flattens curves at a tolerance that follows its matrix,
so it is not the leaf's raster), text by its leaf coverage form, each over the leaf's own window.
An image effect publishes its data window as its content bounds, as the CPU reference does (only
Transform 2D keeps its input's). A Composition Source publishes its nested frame as its bounds, as the CPU
evaluator does ([Layer And Graph Model](layer-graph-model.md#composition-sources)). Command
semantic keys carry the resolved operands plus the pinned render SPIR-V digests and never node/layer
IDs, operation indexes, or the revision. `ImageSource`/`VideoSource` leaves decode raw on the CPU
task thread through the evaluator's own entry points and publish a frozen upload command; when the
shared GPU color context is available, a real input-to-working OCIO transform and a fractional
proxy point-resample then run as GPU commands over that upload, so the decoded samples are not
color-converted on the host. On that route a video frame, a PNG or JPEG still and an all-half
OpenEXR colour layer publish their native samples (#422) and the host builds no float frame at
all: the upload carries the decoder's planes and `SampleConvertV1` produces the working image on
the device. Only a still without a native form (TIFF, a FLOAT or UINT OpenEXR layer, a missing
sequence frame) still uploads its raw RGBA32F decode. Without that context the leaf keeps the
connected CPU conversion path as the fallback. The upload/source semantic key contains no node id, plan index, frame time, or
layer transform; a reachable unsupported layer is screened before any decode. `GpuSceneCoverageCache` is a bounded transactional LRU keyed on raster
geometry alone. No converted upload is retained between builds (ADR 0037). Within one build,
media commands with the same semantic key are emitted once (#386): footage that several layers use
is uploaded once, and its footage interpretation pass, Cryptomatte matte, proxy point-resample and
input-to-working OCIO conversion each run once, with every layer consuming that one resident image.
Equal keys are the identities `GpuSceneCache` already reuses across frames, and the executor counts
each consumer before it releases the shared image. A nested composition's child build keeps its own
commands. `GpuSceneExecutor` (`gpu_scene_executor.hpp`) drives
one immutable `PreparedGpuScene` as a bounded sequence of already-typed native operations on an
existing `GpuDevice`; `begin` validates and flattens the reachable DAG consulting `GpuSceneCache`
first, so a warm unchanged output performs zero native dispatches, `poll` is non-blocking and starts
at most one native dispatch per call, and the request byte budget bounds the actual LIVE unique pinned
bytes with each charge released at the last dependency. One upload may run ahead of the plan (#405,
`GpuSceneExecutorBudgets::enableUploadLookahead`, on by default): uploads are leaves whose input is
the scene's converted host image, so while any native job is in flight the next planned Upload step
is begun on the executor's second `GpuImageUpload` pipeline, and its host staging copy and queued
copy overlap the GPU work in front of it. When the cursor reaches that step the pipelines swap and
the step completes through the ordinary upload path: the same proven fence, descriptor check,
live-pin charge and cache insertion, in plan order, so the output and every cache identity are
unchanged. Both submissions go to the device's one queue in submission order; the upload writes a
fresh image and its closing barrier orders it before every consumer, which is submitted only after
its fence is observed, so no semaphore or separate transfer queue is involved and a device without
one takes the same path. The lookahead begins only within the request budget left beside the live
pins and the in-flight job's actual bytes, and its own actual bytes stay reserved against the budget
until its step adopts it. A later step refused beside that reservation drops the lookahead, waits for
its fence and runs on the serial path with lookahead off for the rest of the job, so the lookahead
never fails a scene the serial executor runs. A step whose process-wide native reservation (path
coverage, Cryptomatte coverage, point resample) another device owner holds, such as the export's
frame beside the Viewer's on another owner thread, is not a failure either (#553): nothing was
allocated, so the executor stays Pending at that step, counts one `reservationWaits`, and retries it
on each poll until that submission retires; only a wait past the job deadline fails the step, with
its reason. Cancellation, a failure or device loss drops the lookahead with
the job; a lookahead whose fence is not yet proven keeps the failed job owner-drain-required until an
owner poll proves it, like any unproven native job. It allocates under the request's processing phase
like every upload, so device admission sees it in the phase's measured peak. Each executor therefore
holds up to two upload pipelines, and the bounded upload resident pool is 16 slots. Each native op's real retained allocation
comes from an additive `hasUnretiredSubmission()`/`lastJobAllocationBytes()` accessor pair, and only a
native result that proves fence retirement may release pins: a failure with a submission still
outstanding retains every pin and refuses reuse. `GpuSceneCache` maps an already-computed semantic
digest to a resident image, charges `GpuImage::allocationBytes()`, runs only on the device owner
thread, rejects a foreign-device image by ownership identity, and never evicts an image still pinned
by an external `shared_ptr`. `GpuResidentFrameLeaseRegistry` takes strong native ownership of one
device-bound `GpuDisplayImage`, charges the actual VMA allocation bytes, and returns a move-only
owner-thread `GpuResidentFramePin`; there is deliberately no accessor returning a strong image outside
that pin.

Coverage contract and final render. `bloom/runtime/gpu_coverage_contract.hpp` is the exhaustive,
compile-time-checked registry of every `CompiledOperation`/`ImageEffectKernel` alternative, every
image-producing authoring lowering, every built-in pixel node type, and the required blend, shape,
geometry (native vector coverage), region-of-interest, layer, color, and display feature axes and
render routes. GPU
production preparation is required by
default; adding an alternative, lowering, or pixel node type without classifying and fixturing it is
a compile failure or a missing-fixture failure, and an unclassified or unfixtured required id keeps
the coverage gate RED by name rather than warning. A fixture passes only with a genuine prepared GPU
scene (or, native-only, real native execution with parity and cache evidence); there is no
admitted-but-not-prepared pass, and an invalid plan or missing media is never a coverage pass.

The only permitted exceptions are typed and narrow: an actual pixel operation, feature, or route
exception requires a non-empty id, a rationale, and an owning document or accepted decision
reference, and no such exception exists today. Host-preparation declarations -- media container and
sample I/O and decompression, font load/shaping, parameter/curve/geometry resolution, and one final
readback -- are a separate audited list and are not a route to exempt a pixel operation, feature, or
route. Approval identifiers are never fabricated, and working-space color conversion is a pixel
transformation that is never an opt-out. Per-pixel vector coverage rasterization is likewise a
pixel transformation, not host preparation: a `CoveredSolidV1` fill from a host-built
`PathRaster::coverageRow` mask does not satisfy the Required `feature.geometry.vector_coverage`
axis. The resident `GpuPathCoverage` producer performs that rasterization from bounded CPU scanline
geometry, and the shared scene builder/executor now emit and dispatch it while retaining its
device provenance and dispatch counters; the native coverage fixture owns that evidence. A requested
region of interest is likewise a Required route (`feature.roi`): the prepared scene clips its native
output to the requested data window exactly as the CPU evaluator does, preserving native windows and
pixel aspect, with a native ROI fixture and missed-GPU detection. Widening decoded media samples to
the working image is pixel work too (#422): each layout in `render::kNativeSampleLayouts` is a
Required `feature.media.native_samples.<layout>` axis (`yuv420p8`, `yuva444p16`, `rgba16`,
`rgba_half`; a new layout fails the build until it is listed). Each fixture prepares a production
video or still scene on the GPU colour route, fails unless its upload carries that layout's native
samples rather than a host float frame, and the native gate executes it against the CPU oracle.
Media decode and decompression stay in the host-preparation list.

Preview and final rendering are both required. Interactive viewer preview, RAM preview fill and
playback, still-frame export, sequence/range export, video export, and headless/scripted render are
required routes through the actual production evaluation paths, with no preview-only exemption. A
final render may read the single composited image back once at the CPU codec/file boundary; the GPU
route's one combined readback transfers the process payload plus, when present, the encoded output in
a single submission, and per-node or per-operation full-frame roundtrips are not a GPU
implementation. Native acceptance is a distinct
gate: on a device it must execute the routes and assert native dispatch, resident provenance, a pixel
oracle, cold/warm cache state, and no silent CPU whole-frame render for an ordinary operation; with
no device it reports an explicit skip, never a pass, and `--require-device` fails. An unavailable
device must publish explicit Unsupported/CPU-fallback provenance, never a CPU frame labelled GPU.

Values measured from pixels in the viewer and RAM preview (#349). A frame whose value graph reads
Image Statistics (an Image Statistics node, or Denoise with Auto strength) needs the statistics before
its scene is built. The preview's GPU scene stage is handed a device measure
(`ImageStatisticsDeviceMeasure`); on its preparing worker, before the frame holds a processing phase
lease, it submits each statistics plan to the application's GPU final-render evaluator (the
`GpuExportProvider`'s `GpuProcessFrameEvaluator`) as an ordinary request with a two-pixel region, and
reads back only that two-pixel result (`prep.statistics.result_readback`). The preview service's own
device and executor are not involved, and the evaluator's owner is another thread, so the measure
cannot wait on the frame it serves. A measure the device cannot make returns the frame as outside the
GPU subset (`bloom.preview.gpu-scene.statistics-unavailable`) and it takes the CPU path, labelled
CPU. [Image Statistics](image-statistics.md#routes) owns the details;
`bloom.ui.gpu_ram_preview_image_statistics` proves the viewer and RAM preview frames GPU-resident
against the CPU oracle with each frame measured once on the device.

Resident preview product and service. The closed `PreparedPreviewFrame` display variant has a fourth,
GPU-resident arm, `PreviewResidentDisplayFrame`, built only by the validating owner-thread factory in
`gpu_resident_preview_product.hpp`. Its only pixel storage is the opaque, owner-bound
`GpuResidentFrameLease`; it retains no CPU pixel vector, no native object, and no Vulkan handle, and
performs no readback. Alongside the lease it retains the genuine immutable resident qualification
report, the request/process identity, the evaluated bounds, and explicit
`EvaluationProvider::GpuResident`/`CpuReference` provenance. The factory validates the report's
eligibility against the exact device ownership epoch and processor, the registry binding and owner
thread, the trusted expected descriptor, the native display geometry against that descriptor, and
the actual native allocation plus geometry inside the request budget. A noisy or slower startup
timing sample cannot refuse an otherwise qualified resident frame.
`GpuPreviewDisplayService` owns one dedicated service thread, the native device/pipeline created and
used only on it, the scheduler GPU executor, and one outstanding native job at a time. It exposes the following
routes on that same owner thread and scheduler lease:

- The packed-readback route (default constructor) composes the accepted CPU stage with the display
  fallback and, when the report's measured eligible interval and the overhead-adjusted comparison
  admit it, runs one bounded native `OcioDisplayV1` dispatch producing packed RGBA8 with
  `PreviewDisplayProvider::GpuNeutral` provenance; otherwise the same evaluated stage is mapped by
  the CPU display fallback and is never compiled or evaluated twice. `GpuNeutral` names the display
  transform only: the scene came from the CPU stage, so the frame's process identity keeps
  `EvaluationProvider::CpuReference` and the status bar reports a CPU render path with a GPU
  display.
- The opt-in resident route (additive constructor) runs a genuine startup `qualifyResidentPreview()`,
  prepares the shared GPU scene on a CPU child, drives it through `GpuSceneExecutor` ->
  `GpuResidentDisplay` -> the resident product factory, and publishes an opaque owner-bound
  `GpuResidentFrameLease` into the service's presentation registry with zero full-frame readback. The
  prepared scene now covers every current compiled pixel operation: solid and covered-solid (native
  vector coverage), translation, affine layer transforms, `SourceOver` and typed `BlendV1`, OCIO
  color/effect transforms, point-resample proxy gathers, media upload, merge, nested composition,
  composition output, and a requested region of interest, plus the default Bloom Neutral display. A
  request the builder refuses (for example non-`Reference` quality, or a working space reaching an
  operation without verified working-space semantics) takes the full original CPU path on the same
  snapshot/identity/overrides rather than mis-rendering.

A stage whose failure, deadline, cancellation, or lost generation leaves a native submission
unretired enters an explicit `Retiring` phase that retains the stage, its completion token, and the
native pins until retirement is actually proven. Before releasing a proven phase, dispose any
unpublished Neutral/OCIO display output and retired failure storage, including cancelled jobs.
Those tokens must not retain the whole reservation until a later request: that request would be
unable to obtain admission. Unknown fences retain their existing ownership and quarantine.
Healthy registry budget pressure is a temporary
refusal that falls back to the CPU without invalidating live leases or pins; only an actual unproven
retention or allocation failure latches the process-wide fuse. The bounded counter snapshot exposes
`fullFrameReadbacks` (zero on the native resident route; scene readbacks plus final packed
transfers on the packed route), `displayStatusReads` (the 4-byte status word),
cache hits/misses, fallbacks, and refusals, so a test can prove no full-frame readback occurred.
Every request that takes the CPU path on either route increments `cpuFallbacks` and exactly one
`cpuFallbacksByReason` entry (`GpuPreviewCpuFallbackReason`: GPU unavailable, non-Neutral request,
not worthwhile under the measured cost model, ineligible stage, unsupported GPU subset, native
failure or device loss, native timeout); a bounded-admission refusal is counted by
`gpuAdmissionRefusals` instead. On the resident route a step still pending at
`nativeDispatchDeadline` is counted and named as a native timeout. The deadline measures device
work only: the wall time the scene executor spends creating native programs
(`ocioProgramCreationNanoseconds`, which includes the driver's own pipeline compile when its
on-disk shader cache is cold, as for a new or changed kernel) moves the stage's deadline later.
Before this, a cold first frame whose driver compile took longer than the deadline fell back to the
CPU once and then prepared on the GPU from the next frame. The `cold-*` node viewer scenarios start
with empty driver shader caches to keep that first frame on the GPU.
A resident-route request made while the startup task is still pending, such as the first Viewer
frame of a project opened at launch, does not take the CPU path as GPU unavailable (#542). It
queues behind the startup on the owner's serial GPU executor and is routed when it starts, against
the startup outcome and the allowance the owner has resolved by then: a Ready route prepares it on
the GPU, and a startup that ended `Unavailable` gives it the full CPU path, counted as GPU
unavailable. Before this, that first frame was a CPU reference render although the device was
Ready about 1.3 s after launch: about 33 s for a 4K source through a Glow of size 64, 117-123 s at
size 244, and 149 s for three 4K layers through a large Gaussian blur, cold or warm. It now paints
2.0-2.6 s after the Viewer opens, the startup included.
`beginShutdown()` is non-blocking; the destructor joins the service thread and drains child/native
ownership before releasing the lease.

GPU-prepared packed service foundation (2026-10-09). The explicit `GpuPreparedPacked` product
mode runs the genuine GPU scene and selected Neutral or general OCIO display processing, then
copies only the final RGBA8 data window into host storage. It works without a WSI target. This is
separate from the older CPU-stage/`GpuNeutral` display route above. The process identity remains
`EvaluationProvider::GpuResident`; the display provider explicitly records transferred host pixels.
Ordinary application preview now selects this packed product for QWidget painting, with explicit
GPU-prepared/CPU-painted status. The native service route remains available and required.

The owner submits and polls the real fence without a GUI wait; host materialization also stays off
the UI thread. Admission validates exact device control-block identity, allocator generation,
owner thread and lifetime token. Numeric allocator generation alone is not device identity.
Completed immutable products retain sealed fence evidence and weak exact-device provenance,
not a native image, lease or Float32 process frame. Full `displayWindow` maps the composition;
`dataWindow` determines packed ROI extent, row addressing and transferred bytes. Consumers must
place that rectangle within the display mapping and paint ordinary background outside it.
Display-only restamps share bytes and proof while preserving exact request/display identity.

Actual source allocation, rounded staging allocation and host vector capacity are distinct from
conservative admitted reservation ceilings. Retained product cost includes pixel/bounds capacity,
proof, outer envelope and both owned request identity strings, with overflow refusal. Successful
`take` moves host storage once and clears transfer retention; product/mailbox/cache ownership is
then charged separately. One bounded active transfer and queued newest request cannot publish a
superseded result. Cancelled or unproved work remains charged until actual fence retirement.
Bounded shutdown quarantines only the final image/device/fence/staging, releases unrelated scene
and host context, and preserves its independent reservation through scheduler/service destruction.
An exited owner is explicit; future recovery is not promised.

The resident packed route creates a frame's rooted host storage on the CPU worker that prepares its
GPU scene, from that scene's own phase, so the worker rather than the device owner first touches its
pages (#477: faulting a fresh 2160p frame in cost the owner about 27 ms of kernel time per frame).
The transfer adopts that provisional storage only when it is exactly the transfer's size and still
admits it with the same staging and host checks; otherwise it allocates as before. A cancelled or
failed stage drops the unused storage with its record.

The preview device reuses a frame's transient native storage for the frames after it (#477). Each
frame's processing images, upload staging, display image and packed-readback staging used to be
created and destroyed per frame, and each creation was a driver allocation and mapping: 17.3
device-memory allocations and as many frees per 2160p PNG16 frame, 12.0 per 1080p EXR half frame.
With reuse enabled (the rooted preview route enables it once its resident route
qualifies), an image or buffer the device allocator issued is parked instead of freed when its
owner releases it, which owners do only after its submission is proven retired. The next
allocation with the identical create info and allocator flags takes it back under a fresh storage
token reserved and reconciled against the current phase, so a parked object is charged to no phase
and a reused one to exactly the phase that uses it. Parked storage holds no content anyone reads: a
reused image starts in an undefined layout like a new one. It is not a cap and not a cache of
pixels; it follows the memory rule like the device caches. Device-local parked storage is freed
first, least recently parked first, whenever work waits for device memory or the driver's free
memory falls below the VRAM headroom (before the scene cache and the retained OCIO programs, and
whatever those releases park is freed with them); parked host-visible staging is freed whenever
work waits for host memory. A nominal-heap device counts parked bytes as allocated. Retained OCIO
programs are always freed, never parked. `GpuDevice::nativeStorageReuseStats()` reports parked bytes
and reuse counts.

The preview owner also overlaps consecutive frames (#477). The scene executor is free as soon as
the display took its output, so while the active stage displays and transfers its frame, the next
ready stage begins its executor under its own processing phase. The device retains one phase and
parks at most one more (`GpuDevice::parkProcessingPhase` and `selectProcessingPhase`); the owner
selects a stage's phase before every native step of that stage, so storage tokens and retirement
always apply to the stage doing the work. Only a clean start happens ahead: a cancelled stage, a
refused phase or a refused executor begin leaves the next stage queued for the ordinary path. The
stage ahead is polled until its executor settles and becomes the active stage when the previous one
is done, where its result (Ready, failure or cancellation) takes the existing path. While a stage
is ahead the executor is that stage's: the active stage's retirement cancels and waits for only its
display and transfer, and a terminal route teardown sends the stage ahead to the CPU path with a
conservatively unproven phase, as it does the active one. The service counts `scenesAhead`.

Measured on 2026-11-04 (16 threads, RTX 4090, cold page cache, Full resolution; the same base with
and without the change, runs interleaved, no other GPU work): with reuse and the overlap a cold B
fill of 240 frames made 0.15 device-memory allocations per frame at 2160p PNG16 and 0.41 at 1080p
EXR half (the first frames' storage), against 17.3 and 12.0. The 2160p PNG16 fill went from 20.5 to
27.6 frames/s, the preview owner busy for 52% of it instead of 64% and the GPU busy 29% of the time
instead of 79%; the 1080p EXR half fill went from 71.7 to 112.8 frames/s, the owner busy 63%
instead of 47% and the GPU 17% instead of 58%. The frames' pixels are identical with and without
reuse and the overlap.

Private Linux strict core/synchronization checks qualify WSI-disabled Neutral/general Full, Half,
ROI and adjusted output against CPU pixels with zero CPU fallback. Full 1920×1080 measured source
8,847,360 B, staging 8,294,400 B and host 8,294,400 B. Shutdown retained the source/staging sum
17,141,760 B with host zero after service/Core/scheduler destruction; its approximately 1 GiB
reservation was an admission ceiling, not actual allocated memory. Two hundred restamps reused
bytes/proof without extra preparation, dispatch or transfer. This is core reuse evidence, not
normal Viewer/cache-100 playback, clipping, sustained performance or other-platform qualification.

Resident capacity and host/device separation. Cache targets govern reusable retention; they must
not make foreground capacity shrink merely because the user chooses a larger RAM-preview target.
The device owner observes `GpuDevice::availableAllocationBudget()` through the existing native
capability boundary. A live `VK_EXT_memory_budget` result already excludes allocated device memory.
A nominal estimate or unknown fallback must separately account for the owner's live and retiring
allocations; it is not treated as fresh free memory.

Owner-thread budget sampling explicitly refreshes VMA's driver sample before reading heap budgets.
Reading `vmaGetHeapBudgets()` alone can retain an old external-pressure reading until enough local
allocations occur; rejected work must recover when another process frees VRAM, even while Bloom
makes no allocations. The existing owner sampling cadence bounds these refreshes.

**One device ceiling (#425).** Usable VRAM, the driver's live budget minus the VRAM headroom
preference (the device-local heap minus the headroom without `VK_EXT_memory_budget`), is the only
device ceiling. The resident capacity plan (`gpu_preview_resident_capacity.hpp`) no longer splits a
pool into fixed shares: the former lease 1/2, scene cache 1/5, request remainder, cache 4/5 of the
lease and nominal-heap divisor 2 are gone. The owner resolves one device pool (the live budget, the
device-local heap for rooted construction, or the 256 MiB unknown-device fallback), and each ledger
is its configured ceiling clamped to that pool on its own: the scene cache, the resident frame cache
(the configured lease ceiling) and the request take the pool or a smaller explicit value, and the
lease registry, which only accounts leases that are real device allocations, takes the whole pool
so an in-flight or on-screen lease beside a full cache is never refused while VRAM is free. The
ledgers are not additive. The application configures no device ceiling of its own and never derives
one from host memory; tests and explicit callers may still pass a smaller one. What the ledgers hold
together is bounded by real allocation admission against usable VRAM, and they give memory back on
demand (below). Rooted requests without an explicit request override use the non-additive
foreground allowance refreshed on the GPU owner from actual available capacity; under a nominal heap
that is the heap minus the generation's live and retiring image ownership, and an image outside the
root's claims is subtracted once from the nominal report (it was subtracted twice).

**Device caches are evicted on demand (#425).** The rules are the host's: staging is scratch,
caches keep what fits and are evicted when work needs the room, finished frames go last, and work
waits only on genuine exhaustion. A phase whose device need fits usable VRAM but not beside what is
held returns `AwaitingCapacity` with domain Device (retained cache images are root-owned claims, so
they make work wait, never refuse it). The preview service and the final-render evaluator each
register a Device reclaimer (`registerDeviceReclaim`) that records the shortfall in a
`GpuDeviceReclaimRequest` and wakes their GPU owner. The owner takes it (`deviceReclaimBytes`), evicts
its scene cache least recently used first (`GpuSceneCache::reclaim`, pinned entries are never
invalidated) and reports the live budget again, which resumes the waiter. What the scene cache could
not give back is published on the preview status as a finished-frame eviction
(`residentFrameEvictionSerial`/`residentFrameEvictionBytes`); the bootstrap's status poll applies it
to the frame cache (`PreviewFrameCache::evictGpuResidentBytes`), dropping least recently used
resident frames. A native allocation refused for device room in a device stage asks for the phase's
need the same way before its retry. When another program takes VRAM and the live free memory falls
below the VRAM headroom, the owner evicts the difference at its next device sample, as host caches
shrink when free RAM falls below the RAM headroom. A recorded shortfall does not wait for that
sample (#474): it wakes the final-render owner too, which evicts at once when idle and between
executor polls while a frame runs. The retained OCIO native programs follow the same rule: neither
the scene executor's effect-program cache nor the `GpuOcioProgramExecutor` behind the display arm and
the output colour stage has a fixed byte ceiling any more (they were 256 MiB retained and 256 MiB
LUT per program in the scene executor, 512 MiB retained and 512 MiB owned per program in the display
arm). Each keeps what fits, and when the scene cache cannot cover a shortfall the owner releases
their programs least recently used first (`GpuSceneExecutor::reclaimOcioPrograms`,
`GpuOcioProgramExecutor::reclaim`), never the running job's program or one whose submission is not
proven retired; a released program is created again by the next job that needs it. Each dispatch
stays bounded by its request's live byte budget. The final-render owner's anticipatory room for
its next frame (#492) evicts only the scene cache, so a run's programs are not recreated every
frame. Because retained cache images are root claims, a full cache
leaves little room beside it: a measured envelope's executor ceiling is therefore never below its
measured need (which fits the remaining room), and the rooted foreground request credits the scene
cache's retained bytes back, so a cache that fills the device makes work wait or evicts, never
squeezes a frame onto the CPU. A stated (first-frame) need keeps the working allowance apart from
the executor ceiling as before. The final-render evaluator's scene cache uses the
same unpartitioned pool. The scene executor no longer caps one OCIO program's owned bytes at a fixed
512 MiB: a 4320p RGBA32F effect output alone is 506 MiB, and beside the program's LUTs the fixed cap
refused it and sent every 4320p B frame to the CPU (#474); each dispatch is bounded by its request's
live byte budget instead.

Measured on an RTX 4090 (24 GB, default VRAM headroom), RAM fill at Full
from a cold cache in a 40G memory scope; device figures are the driver's peak for the process, the
root's own native allocation and the peak claims:

| Cell | Fill fps before → after | CPU-fallback frames | Device peak / own / claims before → after |
|---|---|---|---|
| 2160p multilayer EXR B | 6.98 → 7.41 (240/240) | 0 → 0 | 1.72 / 1.53 / 2.47 → 4.15 / 4.04 / 5.01 GB |
| 4320p PNG16 B (48 frames) | 0.03 (20/48, timed out) → 2.16 (47/48) | 699 → 0 | 3.89 / 3.80 / 5.12 → 6.30 / 6.44 / 7.77 GB |

The 4320p cell's media was a local 2x upscale of the 2160p PNG16 frames. Before the change every
4320p B frame failed with the OCIO program's fixed ceiling; the resident fractions were not the cause
of those refusals. With the fractions gone the scene cache keeps more of the device (the higher
2160p peak) and gives it back when work waits; no cell had an admission refusal.

**The final-render owner keeps room for its next frame (#492).** An export renders distinct
frames, so its scene cache retains images no later frame reuses. The final-render owner remembers
what its last finished phase needed (its admitted need, or its working allowance beside the most
native storage it held, whichever is larger) and evicts its scene cache, least recently used first,
until the root's usable device room (usable VRAM minus every claim) holds that again: after a
request releases its phase, before its completion is visible, and as a scene's steps complete,
because the steps insert their images into the cache. The frames queued behind the running one are
admitted at once and their executor ceilings are not squeezed. The cache has no ceiling of its own:
it keeps everything that fits beside that room, and pinned entries are never evicted. Since #425
removed the scene cache's fixed share the cache had filled usable VRAM (about 20 GB on a 24 GB card)
and each next frame waited for the room its own cache held or was squeezed onto the CPU.

Measured on an RTX 4090 (24 GB, default VRAM headroom), ProRes 422 HQ export of the PNG16 B
benchmark shot, 240 frames; the driver's peak device memory for the process:

| Export | Before #425 | Before this change | After |
|---|---|---|---|
| 1080p | 0.086 s/frame, 0 CPU-fallback, 3.1 GB | stopped at frame 232 ("needs more memory than this computer can give the export"), 229 CPU-fallback, 20.8 GB | 0.092 s/frame, 0 CPU-fallback, 19.0 GB |
| 2160p | 0.260 s/frame, 0 CPU-fallback, 3.8 GB | 1.49 s/frame, 69 CPU-fallback, 20.6 GB | 0.274 s/frame, 0 CPU-fallback, 17.2 GB |

Each request also passes the shared phase reservation and native resource/capability checks.
Host decode capacity stays independent of device capacity. Unknown capacity uses an explicit
fallback; measured zero stays zero. Allocation failure is diagnosed and uses the supported CPU
fallback without claiming GPU execution. A scene cache with zero retention is valid: it refuses
insertion while allowing foreground preparation and execution. A native presentation lease still
requires genuine owned capacity; disabling retention is not permission to discard a live lease.

Resident metadata admission is independent of pixel extent. The lease registry retains its finite
4096-entry ceiling; the cache uses at most 3584 slots, reserving 512 for in-flight, presented and
tombstoned leases. Entries allocate lazily. A positive byte pool no longer derives a cache count by
dividing by an assumed 8 MiB frame, and a zero pool remains disabled. Actual native allocation bytes,
retained metadata and the owner-resolved device clamp remain authoritative. Full and Half retain
separate resolution/policy keys: 1920×1080 and 960×540 resident RGBA8 display products respectively.
Working images remain RGBA32F; this count-policy correction changes neither processing precision
nor the explicit resident lease/scene/request byte split. One hundred packed Half display images occupy
about 198 MiB before native alignment and metadata; the actual device charges must still fit the
cache and lease ledgers. Metadata headroom is a finite allowance, not a guarantee that arbitrary
concurrent requests fit either byte or entry admission.

Presentation lane. The qualified Linux loader is rebuilt with `BUILD_WSI_WAYLAND_SUPPORT=ON` alone
(XCB/Xlib/Xrandr and DirectFB stay `OFF`; the Wayland branch adds no pkg-config or `DT_NEEDED` entry).
Device bootstrap adds an opt-in presentation request that enables `VK_KHR_surface`,
`VK_KHR_wayland_surface`, and `VK_KHR_swapchain` only when actually advertised, records whether the
accepted `VK_EXT/KHR_swapchain_maintenance1` present fence or `VK_KHR_present_wait` retirement
mechanism is genuinely enabled, borrows the instance to a caller-minted surface through integer
handle bits plus a per-device epoch, prefers one combined graphics+compute queue so resident images
stay exclusive, and reports `Unavailable` (CPU fallback) when the compute family cannot present.
`GpuPresentationTarget` owns the swapchain, per-image semaphores/fences, and the clear-and-present
command; it never calls `vkQueueWaitIdle`/`vkDeviceWaitIdle`, retains an image until the
presentation-engine signal actually proves retirement, propagates device loss, and quarantines a
foreign-thread or unproven teardown under a bounded process fuse. `gpu_present_image.hpp` and
`presentImage` sample the resident display image into the acquired swapchain with an explicit affine
destination/source mapping, viewer channel remap, backdrop (the display-referred surround color
outside the destination rectangle; inside it the composition's background color, or the
checkerboard when the Transparency Grid is on, exactly as the Viewer's CPU paint), and an
optional premultiplied RGBA8 overlay; both shaders are SHA-256 pinned with a manifest binding and a
configure-time regeneration check. `GpuPresentationCoordinator` (with the Qt-free, Vulkan-free
`GpuPresentationClient`) is a coordinator, not a thread, service, device, or message bus: it is
constructed on the existing service owner thread, does no work until its `pump()` is called, crosses a
UI-created surface as integer handle bits, reuses the target acquire/present/retire path and the
strong-lease-pin contract, publishes `Retired` only after presentation-engine proof, and bounds
retained records with `maxRetainedTargets`, an explicit `forget()` terminal acknowledgement, and a
fixed process quarantine reservation store.

The clear/discard layout transition starts at `TRANSFER`, matching the acquire semaphore wait
stage; discarding contents with `UNDEFINED` does not remove the acquire execution dependency.
Present-fence and present-id scalar backing storage stays alive through `vkQueuePresentKHR`.
Target acquire defaults to zero-timeout `Poll`; the explicit owner-only `BoundedProgress` mode
requests a fixed 1 ms maximum driver wait. `VK_NOT_READY` and `VK_TIMEOUT` remain temporary
`NotReady` results, without an acquired index or acknowledgement. After an ordinary poll returns
`NotReady`, the coordinator admits at most one such probe across the entire owner pump, rotating
traversal after the consumed target. All targets still receive retirement driving. Locked admission
rejects newly closed, resizing, retiring, or shutdown targets; no mailbox lock spans the driver call.
An already admitted finite call may finish after a concurrent close request. This bounds aggregate
requested driver wait to 1 ms per pump, independent of target count, rather than promising a hard
wall-clock deadline or adding UI waits.

Isolated native evidence on 2026-10-09 reproduced the clear acquire/layout synchronization hazard.
Zero-timeout and a controlled 1 ns acquire stalled after initial buffers; server release messages
were sent but the client did not process them. A controlled 1 ms acquire restored progress.
The final typed policy passes repeated clear/image reuse, resize, acquired-unpresented retirement,
and eight simultaneous three-target reuse rounds with core and synchronization validation on the
isolated Vulkan and GL compositors. These are lifecycle/progress checks, not viewer performance or visible-intent
qualification. Qt before/after present hooks remain a separate integration gap assigned to G4D,
after completion notification work: fixture-only hooks did not resolve the observed stall, and the
Wayland before hook can wait for frame synchronization. That integration needs a bounded UI-thread
design and its own qualification.

`bloom::ui::ViewerGpuPresenter` adopts the coordinator
client's borrowed instance through `QVulkanInstance`, creates one `QWindow`/container on the UI
thread, forwards actual Qt input, and presents only through the opaque lease/params/overlay port; it
creates no device, pipeline, queue, service, or thread and performs no native work on the UI thread.
It never reparents, hides, or destroys the container while a target is live: the host must call
`prepareForMutation()` and wait for `SafeToMutate`.

Host gating and application integration (current source). A typed optional `EditorNativeSurface`
lifecycle interface lets a host ask an editor's presenter to retire a live native target and wait for
a genuine `SafeToMutate`. `NativeSurfaceRetirementGate` makes every affected mutation all-or-nothing;
`EditorArea` defers a picker replacement of a live-native editor and reverts the picker until the
rebuild actually applies; `WorkspaceHost` retires the whole conservative tree before
split/close/collapse/root-replace/restore, reports `Deferred` rather than a premature `Restored`, and
resumes only targets still attached to the live root; `ApplicationShutdownCoordinator` emits
`shutdownQuiescent` only once BOTH task quiescence and native-surface retirement are observed, and the
application begins service shutdown on `shutdownQuiescent`. `apps/bloom/main.cpp` now constructs the
resident service overload (GPU scene stage + CPU stage + display fallback), derives the configured
resident retention budgets from the UI frame-cache target, and clears the request override so
rooted foreground work uses delivered host capacity and owner-observed device availability.
Ordinary widget preview uses GPU-prepared packed products with native presentation disabled;
qualified native fixtures select their presentation route explicitly. The root shares the coverage
cache and the one shared OCIO resolver/context, and builds a session-refreshing GPU
scene stage so relative media follows the live session base directory.
`bloom::ui::GpuViewerBootstrap` caches the service's presentation client/availability and installs
the owner-resolved, capacity-clamped resident plan on the shared frame cache through the existing
status poll, then hands every current and future `ViewerEditor` a typed `ViewerGpuDependencies`;
`ViewerEditor` presents the
resident arm through `ViewerGpuResidentController`, which owns the same-request CPU fallback, the
native CPU cover, and off-thread overlay rasterization, and forwards native window input back through
the real event handlers. The embedded window's logical geometry belongs to its Qt window container;
device-pixel extents are passed separately to the presentation service. The CPU cover is an
output-only raster `QWindow` with its own public `QBackingStore`, owned through an alien wrapper and
host. Its persistent alpha-capable child is established under the presenter's GPU window before
first presentation and stays mapped until genuine target retirement. Concealment paints transparent
pixels; a required cover paints opaque Canvas even when its snapshot is unavailable. This preserves
the Wayland subsurface role: creating a new role while an idle/refused GPU parent never commits again
cannot make the cover visible. Before first GPU application, one temporary top-level child shares
the same snapshot, then is destroyed on genuine application. Steady state has one raster backing
store. Every genuinely new presenter establishes its first-attach cover again, including neutral
recovery after CPU adjustment retirement; previous CPU concealment cannot suppress the new child.
Initial attach has at most two raster backing
stores. Qt Wayland bounds each platform pool to five buffers, so this
is a retained-pixel inventory, not an aggregate-budget claim. Snapshot/resize/expose updates coalesce
through Qt, and unchanged content/geometry/cover state skips painting. This avoids Qt Wayland's
decorated-parent native-widget backing-store crop and undefined overlapping container stacking,
while keeping editor siblings alien. It uses no private Qt APIs, decoration-offset compensation,
native-decoration bypass or extra parent/GPU commits. A blank frame or project replacement
retires the previous target before unmounting it;
completion is queued onto the UI event loop before destroying the presenter. Resizing retains a
pending presentation of the current frame until the new extent is ready, and an older acknowledgement
cannot complete that pending presentation.

Coherent outline publication (G3B1). The controller captures a complete immutable image/parameters/
overlay-recording pair, with one active raster and one newest request. A changed required overlay
never causes an outline-free replacement: the last complete pair remains visible, and a completed
older coherent pair may advance the display while newer geometry is still being prepared. Its exact
owner-applied identity remains stale until the newest pair applies. Project/composition, surface,
selection/style, removal, and viewport extent domains prevent late completions from resurrecting
removed handles or crossing a replacement. Reuse compares the actual recorded commands and logical/
device extent, rather than trusting a hash or request generation; direct UI comparison is capped at
64 KiB of commands, and larger recordings conservatively rasterize on the existing worker. Raster
failure retains the last complete pair only when its requested safety domain still matches, with a
concrete diagnostic; otherwise it raises the CPU cover/fallback. There is no scheduler-cadence
change or UI wait.

Publication admission is coalesced to one owner-outstanding update plus one newest complete update.
The controller retains at most six distinct immutable frame handles across applied, two submission
correlations, ready, active, and newest slots; the command-stream reuse key retains no frame, and
the 32 trace correlations retain identity only. An exact consumed-owner `rejectedSequence` releases
the admission without inventing an applied acknowledgement.
Native owner acknowledgement proves submitted presentation, not compositor-visible pixels; external
bounded fixture captures are the separate visible oracle.

Actual displayed intent (G3B2B). Desired viewer controls, a complete prepared candidate, mailbox
admission, an exact owner-applied native descriptor, and a painted CPU/cover descriptor are separate
states. Missing or failed adjusted preparation retains a same-project/composition last-good image
visibly stale; it does not retire the native target or substitute neutral pixels as the requested
adjustment. Identical failed adjustments are not retried on polling. Cancellation completion may
schedule only the newest still-live desired identity. Shared Failed/Unsupported configuration
diagnostics remain authoritative. A ready candidate without actual publication has no displayed
frame and reports presentation pending.

Native availability and currentness are distinct. A mapped, exposed previous publication remains
the actual displayed descriptor, labelled Stale while a changed overlay or retirement is pending.
Currentness and safe retention additionally require Active and the exact requested domain. Hidden
or unexposed Viewer/native/active-cover output has no actual descriptor; an unavailable live native
surface cannot expose an old CPU-painted descriptor underneath it. Native exposure is a necessary
availability check, not an independent compositor pixel acknowledgement.

Resize completion has its own `completedResizeSequence`, advanced only after successful native
recreation. Image `appliedSequence` continues to describe a submitted image publication. The
presenter coalesces newest requests during attach/resize and keeps admission closed until that
resize sequence completes, including A→B→A where matching extent alone cannot prove completion.
Retirement takes precedence. These lifecycle checks do not solve Qt container clipping: external
private captures still reproduce spill on held resize and top-level shrink. Native-widget viewport
and geometry-admission experiments were rejected before production integration. The next bounded
repair is an independently reviewed asynchronous final GPU RGBA8 transfer into ordinary QWidget
painting; it must preserve GPU scene preparation, exact provenance/identity, budgeted host storage,
safe cancellation/retirement and the required native rendering route.

The correlated native descriptor captures frame, geometry, destination, channel/background,
selection/removal and surface domains, logical container rectangle, and DPR before enqueue. Its
metadata remains readable when the lease stops admitting work; it never grants reuse permission.
Refusal retains native pixels only in the same project/composition, selection/removal, live surface,
logical/device extent and DPR domain. An unsafe refusal, resize or domain clear requires an opaque
CPU cover or explicit unavailable display. Late old-domain acknowledgements cannot uncover it or
restore currentness. CPU cover metadata describes the exact completed capture, including blank or
budget-refused capture; it never inherits the newest candidate's identity. Project/composition
cleanup is keyed to each distinct domain and precedes the retirement early return. A second domain
change refreshes or blanks the installed cover while preserving the mapped target and the single
pending retirement. Failure notifications carry concrete diagnostics without synthesizing present
acknowledgements.
An explicit document rebind invalidates all viewer-local retained frames, probe/cache ownership and
publication domains even when numeric project/composition/revision IDs collide. Cancelled adjustment,
fallback and probe handles are detached; an old completion cannot publish into the new owner.

Passive hit mapping and evaluated bounds use the same actual frame and captured destination. They
reject foreign project/composition/time and pending selection domains before retained-layer remapping.
An ongoing gesture freezes its actual mapping and validates real time/domain/format/viewport/
transform/selection/lifecycle changes, rather than cancelling on preparation lag or lease admission
expiry. GPU Auto resolution stays stable while a per-viewer adjustment is pending. The pixel probe
is unavailable for another displayed intent or channel; it cannot sample the newest prepared frame
and label it as painted pixels.

The applied descriptor aliases the existing applied frame; immutable geometry adds no distinct
frame. Each viewer adds two bounded CPU descriptor slots (paint and cover), which may retain older
frames than its current preparation/fallback slots. The temporary capture descriptor aliases the
capture source and is reset after installation. These metadata slots add no pixel copies or GPU
readbacks; the existing bounded cover pixmap remains the only cover capture. The independent raster
surface replaces the former child crop/copy from the parent's shared QWidget backing store and still
paints into its own platform backing-store buffers. Qt Wayland 6.11.2 bounds each pool to five
buffers: at most ten during temporary first attach, and five in steady state. This is retained
platform pixel storage for the G6 inventory, not an aggregate-budget repair or a claim of one
platform buffer. Foreign CPU handles are cleared on replacement, and native
descriptors/correlations are cleared after real retirement when the presenter resumes.

The owner pins a replacement lease before acquiring a swapchain image. Rejection releases only
that replacement's pending overlay charge and publishes its exact rejected sequence, retaining
the previous native image and active overlay charge. Acquire/retry/retirement proof is unchanged.

G3B remains incomplete until its independent visible-intent and presentation-integration gates are
qualified. G3B2B covers actual display identity, last-good/refusal policy, failed/non-neutral
adjustment, passive/frozen gesture mapping, and selected-viewer status. G3B2A repairs the underlying
clear/discard dependency and bounded WSI progress; that bounded slice is accepted after independent
source review and native reruns, CPU checks, a Vulkan-disabled render build and source-quality checks.
Metadata/identity and lifecycle checks do not alone qualify compositor-visible pixels or every
refresh during interaction. This does not claim cross-platform native GPU qualification; the explicit CPU
fallback remains supported on macOS and Windows.

The RAM preview controller fills its range with a bounded two-deep pipeline
so the next frame's CPU preparation overlaps the previous frame's display stage, with the
pixels-identity duplicate guard, out-of-order collection, cancel-all, and no cross-frame coalescing
key. Frames finish in any order (several prepare at once, and one may wait for device capacity
while later ones run), but they land in the preview cache in frame order (#544): a finished frame
ahead of an unfinished earlier one is held until the earlier one lands, is never submitted again,
and counts against the fill width meanwhile, so the cached range is always one run from the start
of the work area. A rescan or the end of the run lands whatever is held; a frame that is skipped
after its retries no longer holds the frames after it.

Honest outcome: the production application source wires the resident display route end to end, and a
main-linked vertical acceptance now passes over a genuine OpenEXR document (a real `AddImageLayer`
image source compiled by the real `SnapshotCompiler` and the unmodified stage factories) through the
resident frame cache into the actual `ViewerEditor`: fit, zoom-pan, and Red-channel captures match the
CPU reference with zero byte difference, and the viewer backdrop phases (the Solid/Canvas, Gray,
Black and White surrounds over the composition background color, and the Transparency Grid) each
emit interior samples over transparent composition pixels and a surround sample outside the frame
for comparison against the CPU paint, so the GPU and CPU display paths agree. Warm cache reuse adds no native dispatches, the unsupported path
returns the CPU fallback, and shutdown drains cleanly. This document makes no full-application FPS
claim, no measured speedup claim, and no universal or cross-platform qualification claim, and the
resident-preview outcome remains `PreviewOnly`. The final-render route now evaluates through the
same application-owned GPU provider for still, range, video, and headless export: the general
output-color production context resolves the OCIO display/effect program on the blocking CPU stage,
the shared scene composes and reads the single combined process-plus-encoded result back once at the
codec/file boundary, and a 6000x4000 media+CST+text export has locally verified native execution.
The CPU reference evaluator remains the correctness oracle and the explicit supported fallback. GPU
execution is locally exercised on Linux only; macOS and Windows have an explicit supported CPU
fallback, not a GPU parity claim.

Genuinely unimplemented / future work. A whole-application benchmark; the full per-operation
qualification fixtures for a future `ReferenceParity` profile (the qualified display transform and
the resident scene route remain `PreviewOnly`; no operation reaches `ReferenceParity`); the
cross-platform Linux/macOS/Windows parity spike; Windows/macOS GPU support; and a reviewed
XCB/Xlib/Xrandr presentation intake. The scene evaluator now prepares every current compiled pixel
operation through one shared scene, including a requested region of interest. The qualified Linux
prefix manifest remains pending, so this direction stays `working`.

Audit checkpoint (2026-10-08). A source review of the merged implementation identified native
buffer-usage/visibility and failure-retirement defects, repeated source verification before warm
reuse, production scheduling delays, incomplete aggregate memory ownership, and a gap between
prepared/cache progress and actually presented frames. Earlier pixel-parity and route evidence
does not establish native validation cleanliness, sustained playback, or whole-application memory
stability. The [GPU reliability and preview plan](../plans/gpu-reliability-and-preview.md) records
the evidence, its limits, and scoped repair/qualification packages. Its redesign choices remain
proposed; this checkpoint does not adopt them or weaken the existing coverage contract.

G1 native-validity repair (2026-10-08; source reviewed and locally qualified on Linux).
Resident Neutral display buffers now declare their image-copy transfer usages, and its mixed
host/transfer input dependency names both source stages. CompositeBuffer initialization flushes
both direct mappings and metadata staging through allocation-relative VMA ranges; staged metadata
also records transfer-write to shader-read visibility. Composite translation/source-over, Affine,
and Blend make shader-written status visible to the host before their fences and invalidate the
status word before CPU inspection. A missing mapping or failed flush/invalidate rejects output.
General OCIO status uses the same compute-to-host dependency. Final process/combined-output
readbacks and full/sparse pixel-oracle readbacks record transfer-to-host dependencies covering the
exact copied destination bytes before fence retirement and invalidate. Shader identities, CPU
semantics, single final-readback policy, and coverage requirements are unchanged.

G2B metadata lifetime repair (2026-10-08; implementation qualification recorded in the reliability
plan). Buffer preparation no longer submits or waits on a private queue operation. Translation's
X/Y metadata and PointResample's axis initialization copies execute in the owning compute command
buffer, with transfer-write to compute visibility after flushed staging writes. Staging shares the
real allocator owner and moves with its destination through the existing job/quarantine lifetime.
Both Translation axes remain alive concurrently until the same completion proof; actual VMA
staging allocation sizes are summed in admission and live-byte accounting. Proven completion frees
staging before a successful image remains cached. Timeout and cancellation never authorize early
release; injected device-loss diagnostics first prove the real submission retired. Direct mapped
Affine/status initialization retains the flush contract and rejects a missing mapping.

Presentation enables swapchain maintenance only with the matching EXT/KHR surface-maintenance
instance extension and its surface-capabilities2 prerequisite. Without them, existing present-wait
or unsupported-presentation behavior remains available. The present render pass explicitly chains
its initial layout transition to the acquire semaphore's color-attachment-output wait stage.
Composite's descriptor pool now reserves five storage-image descriptors for its two actual layouts.

The local G1 native run reported zero core/synchronization validation errors for Neutral/general
display, Composite/Affine/Blend CPU parity, PointResample, the production general display service,
full-resolution viewer and original interaction fixtures, genuine RAM fill/replay, and the mixed
media/CST/text final-render route including 6000×4000. The RAM fixture retained and replayed four
frames with zero warm dispatches, zero CPU fallbacks, and zero production full-frame readbacks.
Instrumented native tests call real VMA on success, check mapped allocation-relative ranges in both
initialization paths and actual operator consumers, and inject flush/invalidate failures. Observed
mappings were host-visible, coherent, and cached; no noncoherent memory execution was exercised.
Present fences and present-wait were available locally; unavailable-extension fallback combinations
and macOS/Windows GPU execution remain unqualified. These bounded results do not qualify sustained
playback, whole-application memory stability, or G2 failure/retirement behavior. The visibility
sequence follows the [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).

## Boundaries

```text
Qt UI thread
  - input, panels, surface lifecycle, completed-frame notification
                 |
                 | immutable evaluation request / cancellation token
                 v
Runtime scheduler and CPU worker pool
  - graph planning, decode, CPU reference nodes, staging preparation
                 |
                 | GPU work packet; no Qt types
                 v
GPU service thread
  - device and queues, submissions, fences, caches, resource retirement
                 |
          +------+------+
          |             |
          v             v
  offscreen images   presentation bridge
  and buffers        and per-viewer swapchains
```

`src/render` owns logical GPU resources, command generation, synchronization, and backend errors.
`src/runtime` decides which validated implementation evaluates a node. `src/ui` owns any `QWindow`,
`QVulkanInstance`, or `QWidget` integration. The document model does not know whether a result came
from CPU or GPU execution.

The backend interface should expose operations such as device discovery, capability reports,
resource creation, asynchronous work submission, completion, cancellation state, and diagnostics.
It must not mirror every Vulkan object or flag. Backend-native handles remain internal except through
a narrow, non-owning presentation interop contract.

The public capability boundary is Qt- and Vulkan-free. Conceptually it contains:

- `GpuQualification`: `Unavailable`, `PreviewOnly`, or `ReferenceParity`, stored on each
  operation-and-precision capability;
- `GpuDeviceState`: `Initializing`, `Ready`, `Lost`, `Recovering`, `Unavailable`, or
  `ShuttingDown`, followed by terminal `Stopped`;
- backend/build/shader/compiler identities and the selected physical-device/driver identity;
- checked limits, memory-budget support, floating-point controls, portability-subset facts, and
  presentation capabilities; and
- one `GpuOperationCapability` per operation ID and precision, with required features, tested
  implementation revision, numeric contract, fixture-set digest, qualification outcome, and ordered
  structured diagnostics.

The report is immutable and generation-scoped. `Ready` means the device passed the bootstrap
requirements needed to probe and submit work; it grants no operation a qualification outcome.
A resource, pipeline, or frame created under one device generation cannot be published or looked up
under another. No `Vk*` type, raw handle, extension flag, or MoltenVK object appears in these Bloom
public values.

## Execution And Threading

One long-lived GPU service owns a logical device and its queue submission state. Command recording
may later use worker threads, but submission ordering and resource retirement remain centralized.
The UI submits a render intent and receives either a completed-frame notification or a structured
failure. It never polls a fence.

The Qt- and Vulkan-free runtime now implements the asynchronous `TaskExecutor::Gpu` scheduler
contract; it does not reuse the synchronous `TaskFunction`, which becomes terminal when its call
returns. The scheduler owns bounded admission and state machines but no GPU worker. One exclusive
generation-scoped lease lets the render-owned service thread pull a starter. Dispatch creates its
move-only `GpuTaskCompletion` token and leaves the task `Running` after the starter returns. A later
fence/timeline completion, device-loss event, or cancellation cleanup consumes the token exactly
once and publishes the terminal result through the ordinary task mailbox. Dropping or overwriting
an unconsumed token is an internal failure and terminalizes the task; it never leaves a permanently
running record.

The token carries `TaskId`, GPU service generation, attachment identity, cancellation state, and a
weak scheduler completion sink, but no Qt or Vulkan handle. Only the GPU service may consume it. The
service owns device and queue submission; ordinary CPU or blocking-I/O workers prepare immutable
inputs but never submit or wait for GPU work. Separate caps bound admitted state machines, queued
command bytes, live tokens, and request-owned bytes before the scheduler retains or queues a
request. Callers must preflight those declared sizes before constructing large request-owned
products; this slice does not expose a reservation API. Queue exhaustion applies back-pressure at
admission.
This continuation path, exactly-once terminalization, strict service-thread completion, independent
admission caps, generation loss/recovery, and scheduler shutdown fallback are implemented and
locally exercised on Linux. The Vulkan device service, fence integration, and resource retirement
are implemented; the cross-platform Linux/macOS/Windows qualification spike remains pending, and a
synchronous fence wait hidden inside a task function is not a valid interim implementation.

### Bounded Native Command Batches

The production scene executor records two to sixty-four consecutive eligible Solid, gradient,
translation, affine, source-over, blend (every blend mode and image combine, masked or not) and
OCIO program kernels into one command buffer with explicit dependencies and one fence (#386).
Blur, grade and the glow and light-wrap parts run as OCIO programs, so an effect chain shares
submissions instead of taking one per step. A batched blend uses the same plan (`planBlend`), kernel and
push constants as `GpuBlend`; a batched OCIO program records the program's own pipeline, uniforms
and status word, is marked submitted until the batch retires, and cannot be polled, retired or
reused by another step meanwhile. A run stops before the step whose output would exceed the
executor's batch allowance or `maxNativeBatchOutputBytes` (128 MiB): a batch keeps every output
until its fence retires, where serial steps release each intermediate after its last reader, so
long batches raised a frame's device peak and fewer frames fit in flight (a 2160p three-layer PNG16
fill fell from 21.5 to 15.5 frames/s with whole-frame batches and rose to about 25 with the bound,
which also lowered the frame's device peak below the earlier unbounded translation and source-over
runs: 0.88 to 0.56 GB). The batch pipelines are created once per device generation. The
executor keeps every OCIO program its current scene uses plus up to `maxOcioPrograms` (64) more,
least recently used first, so a frame never recompiles its own effects. Upload, coverage and point
resample still delimit runs and keep one submission and fence each.

Between steps neither owner waits on a clock: the preview service and the export evaluator start
the next step as soon as a poll made progress. With a fence pending they wait on the fence itself
(`GpuSceneExecutor::waitForNative`, which every native kind implements as `waitForSubmission`, for
at most 1 ms in the preview owner and 2 ms in the export loop) and fall back to a 50-microsecond
wait only when the owner has other work (a display or transfer in flight) or a fence wait made no
progress. Inputs may reference earlier batch outputs or Ready images from the exact device
generation; every GPU status word is validated before outputs leave the native batch owner.

All image and buffer backings reserve and reconcile against the existing phase. Translation-axis
scratch first transfers the native phase's separately reserved processing-metadata allowance in
both serial and batched paths. Scratch beyond that allowance requires fresh additive host admission;
it cannot consume output metadata or source/upload promises. Oversized or
failed pre-submit preparation may fall back to serial native execution only after actual backing
destruction and same-origin prefix restoration. An uncertain attempted submission retains the exact
cancelled owner and all inputs until real fence/loss proof; later proof discards its outputs.
Foreign release queues that bounded owner for its native thread. The default remains one admitted
frame; measurements and width-two limits are in the
[Linux qualification record](../plans/gpu-304-linux-qualification.md).

### Reused Command Sets And The Pipeline Cache

The single-submission native jobs (the packed display transfer, path coverage and OCIO LUT uploads)
take their command pool, primary command buffer and fence from the device generation instead of
creating them per job (#457). A job gives its set back only after its own fence proved the
submission retired; the next job resets the pool and the fence and records into the same buffer.
A submission whose retirement is unproven keeps its set in its quarantine exactly as before, and
the device creates a new set for the next job. A generation parks at most eight sets and releases
them before its device. The preview owner also keeps one `GpuDisplayTransfer` for consecutive
frames (a transfer that still owns a submission is replaced, and its destructor quarantines that
submission). The transfer's staging buffer is the parked native storage of the frames before it
(#477), so a steady preview frame creates no command pool, fence or staging allocation for its
transfer. A LUT upload waits on its own fence once, event-driven as the #386 owners do, and returns
as soon as the copy retires; the two-second bound only covers a queue that never retires, which
quarantines the upload as before.

Every pipeline a generation creates (the composite, blend, affine, operation-batch and other
Bloom kernels, OCIO programs, path coverage, the displays and the Viewer's present pipeline) goes
through one `VkPipelineCache`, so a route rebuilt on the same generation reuses compiled pipelines.
When the device is created with a cache directory, the cache is persisted across launches as
derived per-user cache state, never project state: `platform::gpuPipelineCacheDirectory()`, the
`gpu-pipeline-cache` leaf of the per-user cache root (Linux and macOS `$XDG_CACHE_HOME/bloom` or
`$HOME/.cache/bloom`, Windows `%LOCALAPPDATA%/bloom/Cache`). The application, `bloom-cli`,
`bloom-mcp`, the headless export queue and the Python provider pass it; tests and tools that pass
none keep the cache in memory. One file per physical GPU
(`vulkan-<vendor>-<device>-<device UUID>.pipelinecache`) holds an 80-byte Bloom header (magic,
format version, payload size and FNV-1a checksum, vendor and device id, driver version, device
UUID, pipeline-cache UUID) before the driver's data. A file is used only when every field matches
this device and driver, the checksum matches and the driver's own `VkPipelineCacheHeaderVersionOne`
names the same vendor, device and pipeline-cache UUID; another GPU, a driver update or a truncated
or corrupt file is rejected (`GpuPipelineCacheLoad::Rejected`, with the reason), the cache starts
empty, and the next write replaces the file. A driver that refuses data which passed these checks
gets an empty cache. Payloads are bounded at 64 MiB on read and write, the directory keeps the eight
newest cache files and drops temporary files a crashed writer left, and every write goes to a unique
temporary file renamed over the target, so a reader never sees a partial file. Only the device
owner writes: the preview owner at most every two seconds at a point where it has no step to start,
the export owner when it is idle, and every generation when it is destroyed; nothing is written
unless pipelines were created since the last write. `GpuDevice::pipelineCacheStats()` reports the
load outcome, loaded bytes, pipelines created and writes. Measurements are in the
[#457 results](../development/benchmark-results-2026-11-06-457.md).

### CPU Composition Seam

The composition preview's CPU half is now split from the display product applied to it, so a later
GPU display stage can reuse the evaluated frame instead of recomputing the graph:

- `bloom::runtime::PreviewCpuStage` (`src/runtime/include/bloom/runtime/preview_cpu_stage.hpp`) is
  the immutable result of compiling, evaluating, and selecting a CPU display processor for one
  request: the evaluated `ProcessFrame`, the selected
  `color::PreparedCpuDisplayProcessorHandle` (null on the reference/unqualified startup path), the
  request identity, the display byte budget, and the compile/evaluation diagnostics. It introduces
  no new document, plan, or color type.
- `PreviewCpuStageFunction` runs the compile/evaluate/selection half and returns an explicit
  `Evaluated` stage or an explicit `Unsupported` outcome; cancellation and genuine failure stay
  terminal `TaskResult` states, never a fabricated empty frame.
- `PreviewCpuDisplayFallback` applies the reference or qualified display product to the stage's own
  `ProcessFrame` and never compiles or re-evaluates. The stage owns the identity and budget, so the
  fallback reads them from the stage rather than taking duplicate parameters.
- `bloom::ui::makeCompositionPreviewPipeline()` is now a thin composition of the two factories in
  `src/ui/composition_preview_cpu_stage.cpp`; the public `PreviewPreparationFunction` alias and all
  callers are unchanged. The qualified provider's Ready/Pending/Failed behavior, ACES and
  non-default display/view selection, view adjustments, overrides, ROI, progress, and diagnostics
  are preserved.

The seam itself creates no device, service, or thread; it separates CPU preparation from the display
product. The application now consumes it alongside the GPU scene stage: `apps/bloom/main.cpp` builds
the resident service overload, so an evaluated `ProcessFrame` can flow into the shared GPU executor
and display product instead of only the CPU fallback. The CPU stage and `PreviewCpuDisplayFallback`
remain the correctness oracle and the explicit unavailable path.

- Requests carry snapshot identity, time, output, resolution, quality, color intent, and a
  cancellation generation.
- A newer interactive request supersedes older preview work. Submitted GPU commands may complete,
  but their results can be discarded without becoming current UI state.
- Shader compilation, pipeline creation, media decode, CPU/GPU transfers, and cache eviction run on
  workers or the GPU service thread.
- Resource destruction is deferred until the last submission that references the resource has
  completed.
- Final renders use bounded queues and back-pressure rather than allocating one frame's resources
  per requested output.
- GPU timing, queue delay, compilation time, upload/download volume, cache hits, and memory pressure
  are observable in diagnostics.
- Staged shutdown closes GPU admission, requests cancellation on every live completion token, and
  lets the GPU event loop consume device completions or device-loss cleanup until every admitted
  token is terminal. It then destroys active device resources on the GPU thread and only afterward
  exits and joins. A qualified shutdown has a bounded device-loss fallback that terminalizes all
  remaining tokens before service destruction. The application never destroys the device from the
  UI thread after its owner has exited.

No subsystem may call `vkDeviceWaitIdle` or `vkQueueWaitIdle` as part of an interactive frame loop.
Those operations are limited to controlled shutdown or exceptional recovery paths, outside the UI
thread.

## Capability And Qualification Policy

Bloom probes the physical device, queue families, limits, per-format features, memory budget, and
extensions at startup. The result is stored as a runtime capability report, not project state.

The device bootstrap baseline should require:

- Vulkan 1.2, a compute-capable queue, storage buffers, sampled images, and transfer operations;
- timeline semaphore support from the Vulkan 1.2 feature set; and
- the queue, synchronization, allocation, and limit-query behavior needed to run bounded probes.

Format sampling/transfer/storage support, storage-image versus storage-buffer implementations,
required 2D extent or tiling, precision controls, and memory budgets are checked on each operation
capability. `VK_KHR_swapchain` and surface support belong only to presentation capabilities. Failure
of one such check makes that operation/precision/presentation entry unavailable; it does not erase
valid capabilities or make a successfully initialized headless device globally unavailable.

On a portability implementation, Bloom enables `VK_KHR_portability_enumeration`, creates the
instance with `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`, and enables
`VK_KHR_portability_subset` when the selected device advertises it. Every required capability is
still checked directly. Optional extensions may improve throughput but cannot add authoring or
render semantics that have no portable implementation.

MoltenVK support is never inferred from a macOS version, Apple GPU family, advertised Vulkan
version, or successful device creation. The exact portability-subset restrictions and enabled
features are recorded in the report and exercised by the same operation fixtures as native Vulkan.

Capabilities are grouped by operation rather than a marketing tier. A node declares its CPU
implementation, GPU implementation, required formats/features, and parity tolerance. The scheduler
may form CPU and GPU regions, but it includes transfer cost when choosing a path. Repeated CPU/GPU
ping-pong is a planning failure to report and optimize, not an invisible default.

Large images must not be silently clamped. If an image exceeds a device limit or memory budget,
Bloom either uses a tested tiled implementation, evaluates on CPU, or reports an actionable error.

Qualification outcomes have precise meaning:

- `Unavailable` means no safe implementation is admitted. Bloom uses CPU or fails the operation.
- `PreviewOnly` means the operation passed safety and declared preview tests but not the reference
  precision/parity gate. Preview status names the degradation; final output cannot select it.
- `ReferenceParity` means the exact backend, operation, precision, shader and compiler revisions,
  device capability profile, and fixture-set digest passed the frozen CPU comparison gate. Only
  this outcome may become eligible for strict final work in a later accepted policy.

For a requested operation set and requested precision, the scheduler derives an aggregate result as
the least-qualified required operation. This is a request-planning result, not a mutable field on the
device report. A missing operation/precision entry is `Unavailable`; unsupported operations do not
inherit another operation's outcome, and an `RGBA16F` result never qualifies an `RGBA32F` request.
Capability reports are diagnostic inputs and cache identity, not project authoring state.

## Image Semantics And Correctness

The GPU backend does not choose Bloom's canonical color or alpha model. Evaluation requests provide
explicit color intent, and image descriptors carry channel type, dimensions, alpha semantics, and
color-space metadata independently of Vulkan formats.

`RGBA16F` is the initial candidate for interactive GPU intermediates because it is useful for HDR
compositing and bandwidth-sensitive previews. `RGBA32F`, packed display surfaces, deep data, and
other representations are capability- and operation-specific. Choosing a GPU format never reduces
the precision requested for strict final output without an explicit policy and visible diagnostic.

Every initial accelerated operation also has a CPU reference implementation. GPU parity is tested
with documented absolute, relative, and image-level tolerances appropriate to the operation; it is
not assumed to be bit-identical across vendors. Cache keys include the implementation revision,
shader revision, quality mode, relevant capabilities, and device/driver identity when those can
change results. The normative value, precision, color, alpha, and primitive-version contracts are
owned by [`evaluation-primitives.md`](evaluation-primitives.md).

The first reference-parity profile uses `RGBA32F`. Its numeric gates are:

- solid fill, clear, transparent borders, zero/one opacity endpoints, and alpha endpoints are
  bit-exact to the CPU fixture;
- Translation/Opacity Bilinear v1 and Source-Over v1 use per-finite-component absolute and relative
  error limits of `2e-6`, with the comparison accepting a component only when either limit passes;
- RGB is held to that `2e-6` limit and alpha is exact, with one exception (#545): alpha an effect
  derives from colour that a device route matches within `2e-6` rather than bit for bit (a Luma
  Key, Glow's included, of colour from a device Gaussian blur or a recipe built on it, a generated
  gradient, or a Grade or Levels `pow`) is held to the same `2e-6` limit. The device Gaussian blur
  stays within the tolerance and is not forced bit-exact. Alpha that does not come from such colour
  stays exact; there is no other alpha tolerance and no opt-out. The native recipe test keys a Glow
  from a generated gradient under this rule (max alpha difference 6.0e-8 on an RTX 4090), and the
  scenario suite's `export-cpu-parity` check applies it only to a shot whose spec keys such colour
  ([benchmarks](../development/benchmarks.md#scenario-suite));
- no successful finite CPU result may become NaN or infinity; negative and HDR RGB are preserved;
- subnormal and signed-zero cases follow the primitive fixture expectation. A device that flushes or
  rewrites a required case does not earn `ReferenceParity` for that operation;
- qualified packed display RGB differs from the CPU result by at most one 8-bit code value per
  channel, while alpha is exact; and
- any fixture outside these limits is a capability failure. A tolerance is not widened after seeing
  a device result without a reviewed primitive/qualification version change.

`RGBA16F` remains a distinct `PreviewOnly` profile until it has its own explicit precision and image
quality contract. Passing an `RGBA16F` preview gate says nothing about `RGBA32F` reference parity.

The final-render route now evaluates through the application-owned GPU provider described in
[`frame-output.md`](frame-output.md), with the single final CPU readback at the codec/file boundary
and the CPU reference evaluator as the correctness oracle and explicit `Disabled`/unavailable
fallback. That is a production preparation/execution route, not a `ReferenceParity` claim: any
stricter final-output policy remains an explicit decision rather than an automatic consequence of
having a GPU.

GPU process cache identity includes the process request identity, primitive and backend semantics,
backend/shader/SPIR-V/compiler revisions and options, texture/storage precision, operation
capability-profile digest, and device/driver identity whenever any of them may alter pixels. Display
cache identity additionally includes the qualified OCIO display processor identity from
[`color-management.md`](color-management.md). Request generation, cancellation, queue depth, and
budget do not affect pixel identity. Resource-generation identity is separate and prevents a cache
object from surviving device recreation even when its pixels would be equivalent.

## Shaders And OpenColorIO

Bloom-authored kernels use one reviewed Vulkan GLSL source path. Shipped Bloom shaders are compiled
to SPIR-V at build time, validated, reflected into explicit bindings, and packaged with a source and
compiler version; runtime code never compiles or loads a Bloom kernel. OCIO-generated shader text is
the one runtime-compiled path: it is extracted from the project's resolved config, wrapped, and
compiled to SPIR-V on a CPU worker using the packaged, pinned `glslangValidator`/`spirv-val` tools.
Runtime specialization prefers constants and pipeline variants over unbounded source generation.

OpenColorIO currently exposes `GPU_LANGUAGE_GLSL_VK_4_6` as a GPU shader target. Bloom uses an OCIO
`GPUProcessor` and `GpuShaderDesc` to obtain shader text, uniforms, and LUT textures. Because those
products are derived from project-selected configuration and external resources, Bloom treats the
generated shader and LUT payloads as bounded untrusted-derived input rather than trusted code.
OCIO config, processor cache ID, shader text and resource digests, compiler revision, and
device-relevant pipeline state participate in the cache key.
[OpenColorIO GPU language API](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/include/OpenColorIO/OpenColorTypes.h),
[OpenColorIO shader API](https://opencolorio.readthedocs.io/en/latest/api/shaders.html)

The durable config revision, explicit context, source/output Color Interop IDs, display/view, looks,
and missing-config policy come from [`color-management.md`](color-management.md). The GPU adapter
does not read an ambient config or create a second display identity. CPU and GPU OCIO fixtures use
the same qualified processor intent; processor or resource mismatch is a capability failure, not a
reason to substitute the temporary reference mapper.

Before compilation or upload, the adapter enforces checked limits on shader bytes, tokens, entry
points, uniforms, texture count, LUT dimensions, aggregate LUT bytes, and compile time; validates LUT
shape and finite samples; and rejects unsupported OCIO resource forms. Shader compilation runs off
the UI and GPU service threads, and off the native GPU owner thread, as a bounded, cancellable child
process of the packaged tools with no shell involved; the shared `GpuOcioContextResolver` qualifies
the executable-relative package once on a CPU worker and publishes one `GpuOcioProgramPreparer`.
Bloom accepts the output only after SPIR-V structural validation and reflection exactly match the
declared entry point, bindings, descriptor types, dimensions, and limits. Pipeline creation remains
on the GPU service thread and is covered by device-loss containment. A timeout, tool crash,
validation mismatch, driver failure, or budget breach produces an operation-scoped diagnostic,
publishes no partial pipeline or LUT, and cannot poison an existing qualified cache entry.

Glslang is the provisional GLSL-to-SPIR-V compiler because it is Khronos's reference front end and
can be used as a command-line tool or library. Its exact build and transitive licenses must be pinned
and audited before distribution. [Glslang project](https://github.com/KhronosGroup/glslang)

The pinned `glslangValidator` 16.4.0 and `spirv-val` (SPIRV-Tools `b707790a`) now ship as runtime
tooling so GPU color compilation works out of the box for the desktop, CLI, and MCP lanes.
`cmake/BloomGpuShaderTools.cmake` resolves each executable only from the qualified dependency prefix
(never the ambient PATH), verifies it stays under that prefix, and stages it and its reviewed license
and generated inventory evidence into an executable-relative private `bloom-gpu-tools/` directory for
both the build tree and install (inside `Contents/MacOS` for a macOS `.app` bundle; `bin/bloom-gpu-tools`
on Linux and Windows, with the `.exe` suffix). Installed tools carry only an `$ORIGIN` rpath when the
configure-time ELF inspection actually finds a private prefix runtime library; the operating system's
C/C++ runtime is left to the supported platform floor and is never bundled. Each app target receives
typed compile definitions for that relative layout (`BLOOM_GPU_TOOLS_AVAILABLE`,
`BLOOM_GPU_TOOLS_DIR`, `BLOOM_GPU_TOOLS_GLSLANG_NAME`, `BLOOM_GPU_TOOLS_SPIRV_VAL_NAME`, and the
inventory name); no absolute build path is embedded. An unqualified mode, a CPU-stub build, a missing
prefix, or a prefix without the pinned tools clears the capability with a typed reason instead of
falling back to a host tool, and the CPU reference path remains the supported outcome. The staged
tools are the runtime consumer's only compiler: the OCIO program preparer invokes them through the
platform process supervisor with explicit source/SPIR-V/diagnostic byte ceilings, an address-space
and open-file limit, a deadline, and cancellation, never a shell or an ambient lookup. The lock
records `shippingRoles: ["executable", "license"]` for both components and the two license
reviews were extended from their earlier build-only form accordingly.

Arbitrary project-provided shader source is outside the initial scope. If scripting or shader nodes
are added later, validation, resource limits, and process isolation require a separate security
decision.

## ASWF Media Interop

OpenColorIO integrates by generating shader code and LUT data; it does not own Bloom's Vulkan
device. OpenImageIO and OpenEXR initially decode and encode through CPU memory followed by
asynchronous staging transfers. OpenEXRCore's thread-safe, non-blocking low-level API and support for
custom unpacking may later reduce copies, but direct GPU unpacking is a measured optimization rather
than an architectural prerequisite. [OpenEXR API](https://openexr.com/en/latest/API.html)

External GPU resource sharing is added only for a concrete integration and must specify ownership,
device identity, image layout/state, synchronization, lifetime, and fallback behavior. A raw native
handle alone is not an interop contract.

## Qt Presentation

Offscreen evaluation does not depend on Qt or a swapchain. A replaceable UI-side presentation
adapter consumes completed frame handles and presents them in a viewer.

The primary spike uses a custom Vulkan-capable `QWindow` embedded in the panel shell. Public Qt APIs
can associate a `QVulkanInstance` with a window, obtain a cross-platform `VkSurfaceKHR`, and query
presentation support without Bloom writing Win32, X11/Wayland, or Cocoa surface code. An advanced
renderer may manage its own device and swapchain instead of using `QVulkanWindow`.
[QVulkanInstance](https://doc.qt.io/qt-6/qvulkaninstance.html),
[Qt Vulkan integration](https://doc.qt.io/qt-6/qtgui-overview.html#vulkan-integration)

The native route targets a same-device GPU copy or sample into each viewer swapchain. Multiple
native panels share the render device where surface support permits and own independent swapchains.
Measured held resize and top-level shrink reject this embedded route as the current clipping repair:
197,225 neighbor pixels and 89,125 exterior pixels escaped their intended boundaries. Native-host
and geometry-admission experiments were rejected, not installed.

The [bounded ADR revisit](../decisions/0006-gpu-backend.md) therefore authorizes a final GPU RGBA8
transfer followed by ordinary QWidget painting with explicit GPU-prepared/CPU-painted provenance.
Stage 2 now adopts this route for ordinary application preview. Painting borrows immutable packed
storage and maps the true ROI data window inside the full display window, painting background outside.
An older coherent image, bounds and channel remain explicitly Stale until replacement; collapsed or
offscreen content advertises no displayed pixels. The cache retains only the sealed host product,
not a native image, lease or Float32 process frame. Cold preparation costs staging, host storage
and one final transfer; fitting warm reuse adds none of those preparation steps.

### Window, screen and pixel-ratio changes

The shipping application builds its preview service with native presentation disabled, so no
Viewer creates a `ViewerGpuPresenter`, a presentation coordinator, a swapchain or a presentation
target. A window resize, maximize, move to another screen or virtual desktop, device-pixel-ratio
change or unmap/remap therefore never allocates or retains native presentation state. It only
re-lays out widgets and repaints the retained packed frame through the window's ordinary backing
store. The preview identity carries no viewport size or device pixel ratio: under the Full, Half and
Quarter resolution settings a window change prepares nothing. Under Auto the displayed scale, which
includes the device pixel ratio, selects the divisor, so a move between a 1x and a 1.5x screen can
flip Half and Full. Each divisor's frame is prepared once on the GPU-prepared packed route and then
served from the preview cache; it never takes the CPU fallback. The only per-ratio cache is the icon
pixmap cache, bounded by the icon set times the ratios in use.

A frame from the packed route carries no Float32 process image, so the pixel probe answers the
hovered pixel with a one-pixel ROI evaluation through the CPU reference evaluator on a worker. That
evaluation is analysis for the probe readout, not the Viewer frame. After a window change the probe
samples the newly shown frame, usually at another pixel. Until issue #414 the operation cache keyed
every result of that evaluation by the ROI, including each layer's full-frame resampled footage, so
every probed pixel after a window change retained another full-frame image per layer, about 100 MiB
per monitor change in the reported 2048x858 project with 16-bit PNG sequence layers. ROI-independent
results now share the full-frame key and ROI-clipped results are not retained (see
[evaluation-primitives.md](evaluation-primitives.md), ROI). The first probe at a resolution retains
each layer's resampled content once; further window changes and pointer positions retain nothing.

The native presenter and coordinator remain the route of the required native fixtures. Their
retention paths (a silently replaced surface, an unproven retirement, a surface destroyed while a
target is live) keep at most one record per presenter and at most `kMaxQuarantinedTargets`
generations per process, and the quarantine fuse then refuses new native targets.

`bloom.ui.window_surface_memory_soak` qualifies this on the application-shaped workspace, the
GPU-prepared packed service and a 2048x858 composition of four 16-bit RGBA PNG sequence layers. Each
cycle moves the window to a 1.5x screen (Auto selects Full) and back to a 1x screen (Half), then
resizes, maximizes and hides it, with the pointer resting over the Viewer a few pixels from its
previous position after every step. After the first round trip it asserts: RSS within 48 MiB across
the measured cycles, the ledger's reusable pools and the preview and operation caches within 4 MiB,
no growth of GPU image allocation bytes, no new preview preparation of the parked frame, no Viewer
frame on the CPU fallback, a transient peak under 160 MiB per cycle, constant widget and window
counts, and zero retained presenter records or quarantined generations.

The Linux run on 2026-11-01 retained, before the fix, 107 MiB on every move to the 1.5x screen and
27 MiB on every move back (one full-frame resampled image per layer at Full and at Half), and about
350 MiB per cycle in all; the ledger grew from 0.7 GiB to 3.4 GiB over 8 measured cycles. Every move
missed the operation cache and added Operation entries, and anonymous RSS rose with the ledger's
host bytes, so the growth was accounted but never reused. After the fix the first round trip
retained 226 MiB once (both resolutions' GPU uploads, decoded footage and resampled layers), and
the 16 later moves retained 0 MiB with zero operation-cache misses: the ledger held 404 MiB,
anonymous RSS 481-519 MiB, RSS 883-896 MiB, GPU images 392 MiB, the cycle peak 24-39 MiB, with zero
preparations, zero Viewer CPU fallbacks and zero retained records.

The channel presentation scan runs in a cancellable bounded CPU worker, with explicit CPU channel
provenance. One completed derivative, one active task and one newest intent bound publication;
admission includes held source/derivative/active allocations and retained legacy process capacity.
Rebind and cancellation reject late old-owner results. This is a responsiveness repair, not required
GPU channel execution or an aggregate memory broker. Exposure/gamma/OCIO remain genuine GPU work.

Private Linux external pixels qualify held panel/top-level shrink, same-size ancestor movement,
ROI, eight handles and collapse without new image spill. Animated cold playback displays a coherent
older frame as Stale while the newest intent remains pending, then pause converges to Current.
Controlled Full/Half 100-frame EXRs retain 829,620,400/207,540,400 host bytes respectively under a
1 GiB host cache, with zero native retained products. Two hundred Current readiness observations
(199 changed-time seeks) add zero scene evaluation, dispatch, transfer or cache growth; they do not
prove 200 forced paints. The native route and final GPU routes remain required. Linux samples do
not establish equivalent macOS/Windows GPU execution or sustained performance; explicit CPU fallback
remains supported. Aggregate/process-limit accounting, remaining channel processing and broader
cache policy qualification stay separate work.

### Controlled mounted-footage A (bounded Full/Half evidence)

The 2026-10-10 private Linux check uses 100 strongly hashed contiguous EXR members, numbered
1001–1100, through the production compiler, GPU packed preparation, RAM controller/cache and Viewer.
The 1920x804 RGB HALF source is centered over authored opaque black in a 1920x1080 composition at
an explicit controlled 24 fps; the separately recorded source rate is 24000/1001. Full output is
1920x1080 with 138 black rows above and below; Half is 960x540 with 69. Every retained frame matches
an independent expected-member still-image CPU plan, including geometry, black margins and
nonuniform source pixels. EXR Auto assumes `lin_rec709_scene` because chromaticities are absent;
parity qualifies that application interpretation, not independent source-color intent.

Both runs retain 100 genuine GPU-prepared products: an initial foreground frame plus 99 RAM refill
products. Full owns 829,440,000 payload bytes plus 6,400 exact retirement-header bytes; Half owns
207,360,000 plus 6,400. Separate cache retention costs are 829,653,200 and 207,573,200 bytes and
overlap the owned payload, so these measures must not be added. Two hundred warm forward/reverse
Current observations add no preparation, graph work, dispatch, transfer or owned allocation.
Final phase and owned claims return to zero before actual retirement exit and collection; relevant
core/synchronization validation is clean. Fill intervals are 21.610 s Full and 21.533 s Half;
complete helper runs are 68.949 s and 65.022 s. These are bounded preparation/parity/reuse checks,
with largest-child peak RSS 1,945,460/1,203,524 KiB and separately reported cgroup peaks 2G/1.3G.
RSS, cgroup usage and rooted payload claims are distinct, not physical GPU-memory totals. These
checks do not establish painted FPS, sustained playback or SMB/server-cold throughput. Source read/decode bytes remain
unmeasured, and digest/header verification warms file caches.

Earlier attempts remain failed evidence: a missing timing utility prevented launch; malformed
external file URIs yielded CPU output with 40/100 retained frames rather than a GPU capacity result; incoherent
stage-only fixture sampling produced AwaitingCapacity despite ample bytes. Correct file URIs and
the existing production off-UI sampler repair the fixture, without changing production policy.
The successful Half run precedes only a fixture declaration-order correction for exceptional
fallback teardown; final Full and the clean one-TU strict check use that corrected order.
Repository quality passes with 1,459 format checks. Only two test files and owning evidence docs
change; production binaries are unchanged. Larger native-resolution footage, native allocation
ownership, media reuse, width two and sustained/platform qualification remain pending.

`QRhiWidget` is useful for a comparison spike because it embeds accelerated rendering naturally in
Qt Widgets and supports Vulkan, Metal, and Direct3D. It is not the core GPU backend. Accessing the
underlying QRhi requires `Qt::GuiPrivate`; Qt labels the QRhi family semi-public and provides no
source or binary compatibility guarantee across Qt minor versions. QRhi resources also cannot be
shared between QRhi instances. If Bloom later uses QRhi in a presentation adapter, that adapter must
be isolated, tested against an exactly pinned Qt minor version, and replaceable without changing
render semantics. [QRhi compatibility and threading](https://doc.qt.io/qt-6/qrhi.html),
[QRhiWidget](https://doc.qt.io/qt-6/qrhiwidget.html)

## Device Lifecycle, Failure, And Fallback

The device service has one explicit lifecycle:

```text
Initializing -----------------------------> Ready
      |                                       |
      +-----------------> Unavailable         v
                                         Lost -> Recovering -> Ready
                                           |         |
                                           +---------+-----> Unavailable

Initializing/Ready/Lost/Recovering/Unavailable -> ShuttingDown -> Stopped
```

Initialization failure publishes its diagnostic and enters `Unavailable`; CPU execution remains
available. Shutdown may begin from every non-terminal state, closes admission before draining, and
wins over initialization or recovery. Once `ShuttingDown`, the service cannot return to `Ready` or
start another recovery. `Stopped` owns no live device, queue, pipeline, resource, or completion that
could later be published.

Device-loss injection points cover device creation, allocation, shader/pipeline creation, submit,
completion/fence observation, readback, and presentation. On the first loss signal the service:

1. atomically changes the generation state to `Lost` and stops new GPU admission;
2. completes outstanding GPU task results with a `DeviceLost` failure diagnostic, using cancellation
   only for work whose cancellation was independently requested;
3. invalidates all resources, pipeline/cache entries, and prepared frames from that generation;
4. exposes a visible CPU-preview fallback without publishing a stale GPU result as current;
5. attempts at most one controlled asynchronous device recreation during the process lifetime; and
6. resumes with a new generation and freshly measured capability report, or remains `Unavailable`.

On a device-loss or unavailable fallback, final output continues on the CPU reference path only when
its immutable request and policy allow the CPU equivalent; otherwise it fails. It never resumes
partway through an output with mixed hidden semantics.

- No compatible GPU: start normally with the CPU renderer and show one persistent, actionable
  performance diagnostic.
- Unsupported node or format: schedule a CPU region when parity and transfer rules allow it; never
  substitute a different visual operation.
- Out of device memory: evict rebuildable GPU caches, retry within a bounded policy, then fall back
  or fail the request explicitly.
- Device loss: follow the bounded state machine above; repeated loss does not enter an unbounded
  recreate loop.
- Shader or pipeline failure: retain the compiler and driver diagnostic, identify the affected node,
  and use its CPU implementation when available.
- Preview fallback or reduced precision: expose it in viewer status. Strict final output does not
  degrade silently.

### Processing-phase admission by measured capacity and actual need

One neutral `GpuProcessingPhasePolicy` is explicitly injected by the application, export and
scripting composition roots. Owners sharing an allowance receive the same policy; independent
roots do not aggregate. Missing roots refuse before heavy work. Host preparation, native execution
and final transfer each hold a phase lease before entering the phase. Shared allowance never grants
access to another native owner, device generation or cache.

**Usable capacity.** Usable RAM is the measured available system memory (within an observed
process/cgroup limit, sampled off the UI thread and captured once per delivered sample) minus the
RAM headroom preference. Inside a cgroup v2 limit the domain's usage is `memory.current` minus its
reclaimable memory from `memory.stat` (`active_file + inactive_file + slab_reclaimable`, less
`file_dirty` and `file_writeback`, never more than `memory.current`), as the kernel's own
MemAvailable credits page cache: a limit filled by file cache from builds, tests or media reads is
not a full limit. An unreadable `memory.stat` grants no credit. Usable VRAM is the driver's live device budget (`VK_EXT_memory_budget`
heap budget minus heap usage, through VMA) with the root's own allocated native backing credited
back once, minus the VRAM headroom preference. Without a live budget the documented fallback is the
device-local heap size minus the headroom. The two headroom values (`memory/ramHeadroomBytes`,
`memory/vramHeadroomBytes`, default 2 GiB each, 0 to 256 GiB) are the only safety margin; there is
no fraction, quarter, half-of-remaining share or whole-device ceiling. A policy starts with zero
headroom until its composition root delivers the preference; the desktop applies the saved value
at startup and on every Preferences change, and a headless export root applies the 2 GiB default.

**Claims are needs.** A phase claims fixed output and metadata bytes, its source need and one
source's decode and conversion scratch on the host. Preparation decodes and converts its sources
one at a time, so the scratch is twice the largest single source the envelope states
(`largestSourceBytes`, #472), never twice the whole source need; when a source's extent is unknown
the scratch stays twice the source need. The source need is its minimum source
bytes until its envelope has been measured (see below). Its device need is its working
bytes plus its minimum executor bytes (the live-image peak; GPU scene preparation raises the
minimum to the preflight live-image peak for previews and final renders alike). GPU scene
preparation states the source need from the reachable decoded sources (one float frame per probed
image or video source, the preflight live-image peak when an extent is unknown). A channel read is
stated from its file sources' recorded extent too (#549): a Shuffle holds its one RGBA32F gather,
and a Cryptomatte type its rank layers, one RGBA32F frame per two ranks, counted once for every
matte of the type; the ranks are known only from the file, so the claim takes the Cryptomatte
default depth of six ranks (three layers), and a phase that retains more grows its claim when its
sources are prepared. An image-backed or nested channel source keeps the preflight peak. Requested source
and executor sizes are ceilings: a phase may grow toward them, but they are not claimed. Retained
products (RAM-preview frames, final products) stay charged while they live.

**Device claims follow device stages (#417).** The device need must fit the usable device capacity
and what is free of it at admission, but it is claimed only while the phase runs its device stage:
from its first native image or buffer allocation until it holds no native storage. A phase that is
decoding, waiting for the device, reading back or publishing claims no device memory, because it
holds none. Phases run their device stages one at a time per device, so claiming every admitted
phase's need at admission reserved the sum over all frames in flight: in the #416 runs 9.3 GB was
claimed at 2160p while the driver reported 1.0 GB for the process. At its first allocation a stage
claims its whole need when the live device room holds it, so the rest of the stage cannot be
crowded out; otherwise it claims nothing up front. Every allocation beyond the stage's remaining
claim is admitted against the live device room and refused (`BudgetRefused`, domain Device) when it
does not fit, so a real allocation never over-commits the device; the preview and final paths retry
such a refusal after a release. Storage freed within a stage refills its claim up to the need; the
stage's end releases the rest at once, quietly (the phase's own release is the wake-up, so a failed
attempt's cleanup cannot retry itself). Device claims at any time are therefore the root's own
native allocations plus the unallocated needs of the stages running.

**Claims follow the live set, and only while the scene executes (#539).** A scene's intermediates
that the executor publishes to the reusable scene cache leave the phase when they are published:
they stay charged to the root as retention that gives way on demand, and the phase's allocation,
its refill and its measured peak count only the storage it still holds. Before, every cached
intermediate of a Glow or blur chain stayed in the phase's allocation, so a 2K frame measured a
3.8 GB peak and claimed that for every later frame of its envelope. When the scene has executed, the phase's unallocated claim goes back to the root
at once (`endDeviceExecution`); from then on it may hold at most its stated working allowance
unallocated (the display, packed transfer or readback), and a frame that displays, transfers or
waits to be shown holds only the storage it actually has.

**Capacity never sends a frame to the CPU (#539).** A resident preview frame whose scene executor
refuses it for device capacity (its byte limit, or a native allocation the live room cannot hold)
waits for capacity instead of taking the CPU path. It asks the device caches to give way (parked
native storage, then the reusable scene cache) and completes with an admission retry, so it runs on
the GPU again once room returns (`capacityWaits` in the service counters). Preparation waits the same
way when a source does not fit a source ceiling that other admitted work had cut short, and when an
effect's GPU program could not be compiled because the shader compiler timed out, could not start or
crashed (`compile-unavailable`; many frames compiling at once on a loaded machine): neither says
anything about the scene, and both used to send a GPU-capable frame to the CPU, where a large Glow
took over a minute and stalled the RAM fill behind it. Device memory another program holds is
waited for too. Admission refuses only a device need above the device's ceiling (its device-local
heap minus the VRAM headroom, `deviceCeilingBytes`); a need above the measured free room but within
the ceiling, including a measured zero, waits for that room to come back. A frame whose executor
ran short (its byte limit, or a native image or buffer allocation the device or the live room
refused) waits the same way whenever the room can come back: the device caches can give way, other
admitted frames will release what they hold, or another program took device memory while it ran
(the measured capacity fell below what it was admitted with). When nothing fits now, RAM preview
and read-ahead keep one frame waiting so the run resumes when room returns. Before, a second program
filling the device made every frame `BudgetRefused`, and the fill ended with nothing cached. The
explicit CPU fallback remains only for a frame that already had every byte the device could give
with nothing left to evict, for an unsupported subset and for device loss.

**Idle means small (#539).** When the preview owner has had no preview work for
`idleTrimDelay` (one second), it gives back retained device memory: the reusable scene cache and
all parked native storage. What is on screen and the finished frames the UI holds stay; the next
frame re-creates what it needs (`idleTrims`, `idleTrimmedBytes`). The final render evaluator does
the same (#544): after its `idleTrimDelay` (one second) with no frame queued, running or preparing,
it gives its scene cache back (`GpuProcessFrameEvaluator::retentionStats`), so an application that
exported once no longer keeps that memory until the next export.

**Retention gives way inside the allocation (#544).** A running scene publishes each finished
intermediate to the scene cache, so at 4K one frame of a heavy comp filled the device with its own
retention (about 17.8 GB of 107 entries, almost none pinned). The next native allocation was then
refused, the whole frame failed, released its storage, waited, prepared its sources again and
refilled the same cache: the Viewer's first frame took 40 to 99 s, a RAM fill cached nothing for
minutes, and on the final render route the failed frame fell back to the CPU reference path (a
9-minute 4K frame). Now, when the root refuses a phase's device-local native image or buffer for
room, the device asks its owner's reusable retention to give way inside that allocation
(`GpuDevice::setDeviceRoomReclaimer`) and tries once more. The preview owner frees parked native
storage, then the scene cache's least recently used unpinned entries (freeing what they park), and
measures the device again; the final render evaluator evicts its scene cache. Each gives back at
least the allocation plus however far the claims already exceed the capacity, and at least half of
what it retains (`deviceRoomGiveWay`), so the scene's next steps fit too while the most recently
used half stays for reuse (`deviceRoomReclaims`, `deviceRoomReclaimedBytes`). Published
device-local retention is counted (`retainedNativeImageBytes`): it still counts as claims, so it
still gates admission, but it no longer cuts a phase's executor ceiling, which before shrank to the
room the cache left (a final render frame failed its own 1.3 GB byte limit and went to the CPU).
When the retry is still refused (the device is full of other work, or of another application's
memory), the final render evaluator returns `AwaitingCapacity` instead of an over-budget failure,
so the export waits at the frame boundary and runs the frame again on the GPU; before, the frame
went to the CPU reference path. An OCIO effect's (or display's) output image refused that way is
the same typed over-budget refusal, not an allocation failure: before, that one refusal reported
the device unavailable and sent a CG comp export frame to the CPU reference path.

**Cached audio holds no request slot (#586).** A decoded audio buffer (a video's audio stream or an
audio asset) is adopted into the cache with its processing phase. Once the adoption succeeds the
phase calls `GpuProcessingPhaseLease::retireToRetention()`: it stops counting in `admittedPhases`
against the request capacity (32), returns its unspent host reservation, and keeps only the adopted
backing claimed, reported as `retainedPhases` / `retainedProcessingHostBytes` and included in
`UsableCapacity::hostRetentionBytes`. Freeing the buffer releases the lease. Before, every cached
stream kept one of the 32 slots for its life, so enough audio filled them, exports sat in Compiling
with no frame and the Viewer's preparation was refused. The export capacity wait counts only
admitted phases, so with retention alone it stalls after its limit and the job fails with a plain
error instead of hanging. A refused audio backing or decode-scratch admission in an export is
reported as that cause (a resource refusal), never as a full destination.

**Purge returns device memory (#544).** Purge preview cache (and the media purge, which clears the
same store) clears the resident scene cache on the service owner, then frees all parked native
storage, which the cleared entries had moved into, measures the device again and reports the bytes
that actually went back (for example "Preview cache purged; 11.8 GB of graphics memory
returned"). Before, the clear moved the images into parked storage and the memory stayed allocated
until an idle trim, which never came while a fill waited for capacity. Purging six frames into a `cg-comp-4k` fill, the
instance's device memory (`nvidia-smi`) fell from 12.0 GiB to 0.17 GiB within 0.3 s.

**Measured (#544).** The scenario suite ([results](../development/benchmark-results-2026-11-05-scenarios.md#after-544))
on the RTX 4090 profile, one run each, before on main `8626d3918` and after on the #544 change:

| | Before | After |
|---|---|---|
| `cg-comp-4k` first Viewer frame (fill / playback / scrub) | 47.6 / 62.4 / 98.9 s | 5.9 / 6.1 / 5.9 s |
| `cg-comp-4k` fill | stalled, 1 of 12 in 120 s, 349 capacity waits | 12 of 12 in 9.9 s (1.21 fps), 5 waits |
| `cg-comp-4k` GPU busy during the fill | 28.2% | 75.2% |
| `cg-comp-4k` export | 141.2 s/frame, 1 of 4 frames on the CPU | 3.15 s/frame, 4 of 4 on the GPU |
| `cg-comp-2k` fill | 24 in 55.4 s (0.433 fps), 538 capacity waits | 24 in 5.4 s (4.48 fps), 0 waits |
| `two-layer-glow-2k` fill | 48 in 4.40 s (10.9 fps), 42 capacity waits | 48 in 3.09 s (15.5 fps), 0 waits |

Where a 4K export frame's time goes (`cg-comp-4k`, 12 frames, 2.37 s per frame over the export,
medians of per-frame timers in a diagnostic build): preparing the scene on a CPU worker (source
read, decode and conversion) 1.88 s, up to four frames at once; the GPU run on the owner thread
0.74 s (0.41 s of it waiting on the processing fence, 46 ms readback, 40 ms host publication);
semantic identity 76 ms; the EXR write 0.92 s, reopen verification 0.36 s and hash 62 ms, one frame
at a time with the one-thread codec. Before, a CPU-side profile of the 4K export spent about 531 s
of 543 s on one thread in the CPU reference evaluator (separable blur 42%, float image reads 17%)
for the one frame the GPU route refused; the three GPU frames took about 10 s together.

**Measured (#539, #472).** Cold RAM fills of 120 frames on the RTX 4090 profile
([benchmark results](../development/benchmark-results-2026-10-31.md)), two footage layers with a
Glow of size 64 or 244, or several large Gaussian blurs, one run each before and after #539
(fill rates vary by about a tenth between runs):

| Scene | Peak device storage per frame, before / after | Fill fps, before / after | CPU-fallback frames after |
|---|---|---|---|
| 2048x858, Glow 64 | 3.66 GB / 0.55 GB | 17.6 / 17.4 | 0 |
| 2048x858, Glow 244 | 3.84 GB / 0.65 GB | 9.5 / 9.6 | 0 |
| 4K, Glow 64 | 3.76 GB / 0.76 GB | 17.3 / 15.4 | 0 |
| 4K, Glow 244 | 3.95 GB / 0.84 GB | 8.1 / 9.2 | 0 |
| 4K, Gaussian blurs | 1.76 GB / 0.79 GB | 10.2 / 10.2 | 0 |

The scene cache still retains up to about 5 GB while a fill runs; it is retention that gives way on
demand, not a claim of the frames. A Viewer frame requested during a 4K Glow 244 fill took 0.64 s
(median of six, at most 1.09 s) while fill frames each spent about 1.7 s in flight. One second after
the fill, device memory Bloom holds fell from 5.7 GB to 0.27 GB. With a second program allocating
14.5 GiB of the 24 GiB device a few seconds into the 4K Glow 244 fill, the fill ran one or two
frames at a time in the room left and completed all 120 frames in 43 s with no CPU fallback; before,
the same run sent 163 frames to the CPU, cached 11 and took 240 s. With 18 GiB taken before the fill
started, the fill waited for the room and then completed (before: every frame was refused and the
fill ended with nothing cached). With L25 and L50 stacks of 1080p EXR half layers (the `layers`
suite), host claims follow one source's scratch, so the fill keeps its frames: L25 cached 194 of 240
frames with no evictions (before 155 with 84) at 8.1 fps (before 8.9), and L50 cached 213 with 27
evictions (before 38 with 192) at 4.6 fps (before 4.9).

**A CPU frame always says why (#553).** A final-render frame that takes the CPU reference path
records the reason in its GPU provenance (no provider, no usable device, outside the GPU subset, or
the failed status, code and message) and the export log entry `export.device.cpu-reference` carries
it, as does the first CPU frame of a sequence export. The Viewer's resident CPU fallback task is
named with its counted reason. The intermittent fallback came from the process-wide coverage
reservations (see above): the export's frame and the Viewer's frame each reach a coverage pass on
their own owner thread, so one could be refused while the other held the reservation. The refused
step now waits instead of failing the scene.

**Glow runs its octave blurs at full resolution (#539).** A reduced-resolution octave pyramid was
the remaining item of the fix. A 2x downsampled Gaussian blur differs from the full-resolution CPU
reference by about 2.3e-4 at its largest at radius 244, and by more at smaller radii. The CPU/GPU
parity tolerance stays at 2e-6 and is not loosened for Glow, so the pyramid is not used: Classic,
Physical and Streak Glow keep their full-resolution blurs, and the device peak of a large Glow is
bounded by the live-set claim and the in-flight bound above instead.

**Device needs follow measured peaks (#417).** The stated device need is a fixed estimate: the
desktop's preview working allowance is 512 MiB of room for one general OCIO display job (the arm's
former per-program ceiling, which #474 removed) plus the 320 MiB packed-transfer allowance whatever
the frame size. Each released phase records the most native
storage it held at one time under its stated envelope (working bytes, minimum executor bytes,
fixed host bytes and minimum source bytes: equal for the frames of one composition, resolution and
plan). A later phase of the same envelope needs the largest recorded peak plus a quarter, never
more than its stated need; the first phase of an envelope uses the stated need, and a phase that
ended early never lowers it. The root keeps the 16 most recently used envelopes. Only a stated need
larger than the whole usable device is refused; waiting and the stage claim use the measured need,
and allocations beyond it are admitted against the live room like any other growth.

Measured on the benchmark cells (RTX 4090, RAM fill at Full or an EXR export, peaks over the
run; claims are `deviceClaimBytes`, own allocation is the root's native image and buffer storage,
driver is the process's device memory as `nvidia-smi` reports it; the per-phase columns are means
over the released phases):

| Cell | Claims before | Claims after | Own allocation | Driver | Need / measured peak per phase |
|---|---|---|---|---|---|
| 2160p png16 A fill | 9.67 GB | 1.71 GB | 1.24 GB | 1.16 GB | 1.14 / 0.94 GB |
| 2160p png16 B fill | 15.08 GB | 2.41 GB | 1.43 GB | 1.39–1.52 GB | 1.70 / 1.30 GB |
| 1080p H.264 A fill | not measured | 1.59 GB | 0.82 GB | 0.84 GB | 0.33 / 0.25 GB |
| 720p H.264 A fill | not measured | 1.46 GB | 0.71 GB | 0.83 GB | 0.16 / 0.11 GB |
| 2160p png16 B EXR export | 3.90 GB | 3.70 GB | 3.27 GB | 3.27–3.34 GB | 1.20 / 1.77 GB |

Stated bounds: device claims stay within the root's own allocation plus two phases' needs (the
executing stage and one finishing); measured, the peak claims were 1.1 to 2.1 times the peak own
allocation, the high end being the first frame of an envelope, which still uses the stated need.
A preview phase's need averaged 1.2 to 1.5 times what it actually allocated. Before the change a
final render stated one float frame as its executor minimum, so its need (0.40 GB in the export
row) was a quarter of what a phase allocated; it now states the preflight live-image peak like a
preview, and the rest is admitted against the live room as it is allocated. Fills and the export
kept the same frames, fps and zero CPU fallback.

**Host source needs follow measured retention (#472).** The stated source need counts every
reachable source reference as one float frame, but preparation decodes each distinct source once
(layers that show the same media at the same time share one decoded backing). On the many-layer
benchmark stacks (1080p, L25 and L50) the stated need was 2.8 and 6.3 times what a frame retained;
the frames admitted claimed nearly all usable RAM while the process's anonymous memory was an
eighth of it or less, so the preview cache was given no budget and kept no frames. With the
measured need the same fills kept 225 and 240 of 240 frames (see the
[benchmark results](../development/benchmark-results-2026-10-31.md)). Each phase that finishes source preparation
records the deduplicated source bytes it retained under its stated envelope (the same key and the
same 16 most recently used envelopes as the device peaks, kept separately). A later phase of the
same envelope needs the largest recorded retention plus a quarter, never more than its stated
minimum; its host claim is its fixed bytes plus three times that need while it prepares, and
what it actually retains afterwards. The first phase of an envelope uses the stated need, and a
phase that retained less never lowers it. The safety rules are unchanged: retention beyond the
claim is charged as it is when preparation finishes, a phase's retained sources stay within the
live room at its admission (its source ceiling), uploads and processing storage after the handoff
are admitted against live capacity, and only a need larger than the whole usable capacity is
refused. The Viewer's reserved need and the eviction shortfall use the same measured need.

**Waiting, not refusing.** A need that fits the usable capacity but not what other work holds right
now returns `AwaitingCapacity` with its domain (Host, Device or RequestCount). Before answering, the
policy asks registered retention owners to evict reusable cache entries. Waiters resume on a phase
release, a capacity change or a fresh sample (`waitForAdmissionChange`). Only a need larger than the
whole usable capacity of an otherwise empty root is `BudgetRefused`. On the host that is the
ceiling (the tightest finite memory limit, or physical memory, minus headroom), so a host need that
fits the ceiling waits even while measured free memory is zero, for example while anonymous memory
of other processes fills the same cgroup. Each wait or refusal is logged
once per second per root with the status and domain names and the capacity, claim, retention and
admitted-phase counters (`describeAdmission`). The final-export attempt runner waits in place,
cancellably, and reports `Waiting for capacity`; a refused frame names the counters.

**Latch recovery.** A frame whose native work could not be proven retired latches its phase on the
device generation. Before the next phase the owner waits for the device to go idle (a device-wide
fence proof), proves the latched phase retired and releases it; a lost device is reported, never
released. The generation no longer refuses every later frame.

GPU scene preparation uses CPU preflight for semantic validation without charging hypothetical CPU
intermediate images against the host source allowance. Once the root scene finishes preparation,
the phase keeps its actual deduplicated retained source bytes (charging growth beyond its stated
need) and releases the rest of its preparation claim. Nested builders do not finish a borrowed phase
while sibling sources can still decode. Uploads after the handoff, and earlier uploads larger than
the phase's own remaining claim, acquire their actual nonlocal host backing against live capacity.
Retirement and unproven-work rules are unchanged.
A cancelled scene also releases unpublished outputs in retained OCIO effect programs after native
retirement is proved, before dropping its phase. The program cache keeps compiled shaders for reuse;
pending submissions and unproven allocations remain held.
Rooted resident preview queue admission charges no hypothetical pixel backing: the processing
phase owns that admission before allocation. Scheduler queue/count bounds remain active.

Before CPU evaluation, a CPU-worker planner supplies the preflight live-image minimum, including
nested compositions, to phase admission. That minimum is the phase's claimed need; the caller's
source limit is an unclaimed ceiling. It is not an exact allocation forecast: transformed bounds,
decoded media, and operation scratch remain checked during execution.

#### Preview default propagation

Preview distinguishes omitted limits from explicit finite ceilings.
`GpuPreviewDisplayServiceOptions::residentExecutorBudgets` is optional: an explicit value preserves
its byte and structural limits verbatim. An omitted rooted producer uses the configured request
ceiling, or the device-owner ceiling when the request default is delivered live, rather than a cold
reusable-cache snapshot. Actual execution still clamps each request to its admitted phase, unique
live pins and native resource limits. Omitted unrooted producer budgets retain legacy behavior.

The application explicitly shares one neutral `TaskBudgetView` between Tasks and its processing
policy. The existing off-UI sampler delivers host/process state; the root resolves configured
preview/operation shares against the delivered live processing allowance, then publishes an
immutable numeric snapshot after quota locks are released. Reusable-cache credit is not processing
headroom. Omitted scheduler and controller host-pixel admission read delivered host defaults instead of
latching startup values. The rooted service request and preparation executor use the device-owner
ceiling independently; a host source ceiling does not cap GPU intermediates. Each request remains subject to real phase and device admission; a snapshot is neither
allocation ownership nor a new independent allowance.

Layer masks and mattes (ADR 0028). A `LayerMaskKernel` effect prepares as one fixed
neighborhood program (`GpuOcioTransformKind::LayerMask`, semantics `bloom.render.layer-mask.v2`)
through the same preparer, compile cache and executor as the separable blur. The host samples and
flattens the masks once (`sampleLayerMasks`, `layerMaskProgramData`) and packs segments, reciprocal
factors and per-mask controls into a read-only std430 storage buffer at set 0 binding 0
(`OcioGpuProgramDesc::uniformBufferStorage`). The bytes are in the prepare key and the command
identity; the retained native program is keyed by `PreparedGpuOcioCommand::nativeProgramKey()`,
which omits them, and each dispatch uploads its own command's bytes. A keyed path therefore reuses
one compiled SPIR-V module (the shader compiler's source cache) and one pipeline per buffer
capacity.
The per-pixel math uses no division and a multiply-only square root, so coverage is bit-identical to
the CPU reference. More than 8192 segments fail GPU preparation closed. Image Mix's Mask is binding 4
of `ImageCombineV1` (push mask extent, offset and mode; an absent mask is zero coverage), carried as
a third resident input on the blend command and planned, retained and released like its other two.
A track matte is the existing Multiply Alpha dispatch over two Layer Output commands. Required
axes `feature.mask.layer`, `feature.mask.value` and `feature.matte.track` and the effect and Mask
node ids are fixtured through the production builder with native parity.

A requested executor size is a ceiling, not a need: the phase claims its working bytes plus its
stated minimum (the plan's preflight live-image peak, at least one float frame of the resolved
output), and concurrent phases such as a Viewer request beside a RAM-preview fill are all admitted
while their needs fit the usable device capacity.

The resident service keeps construction ceilings separate from live free space. For an omitted
rooted preview limit, the executor's per-image ceiling uses finite device-local size (or the
bounded unknown-device fallback), while lease, scene, and Viewer retention limits use the
device-local heap as their pool. A zero free-space sample at startup therefore does not permanently disable
those owners. The service samples live device availability on its GPU owner and refreshes the
shared phase report; measured free bytes already exclude VMA allocations. With one reported owner,
or several owners whose reports carry the same driver device UUID
(`VkPhysicalDeviceIDProperties::deviceUUID`, for example the preview service and the export
evaluator on one GPU), the phase quota adds back only exact root-owned local native backing that
remains in its claim ledger, so it is subtracted once. Equal heap sizes are not treated as the same
device. Unallocated phase reservations and other allocations receive no credit; owners on different
or unidentified devices retain a conservative common minimum. Nominal estimates
subtract live and retiring image ownership outside the root's existing claims. Every native
allocation remains subject to the refreshed phase quota and allocator result. Explicit ceilings
are clamped to the device pool independently; none is additive.

Cold, failed, future-dated or stale default delivery closes new default admission with typed
AwaitingCapacity. A fresh sample epoch can retry the newest eligible request through existing
completion/sample notifications. Its sample serial is distinct from the phase release revision;
own cleanup cannot manufacture a new sample or create a retry loop. Explicit finite limits remain
explicit and are never enlarged by default delivery. Owners without a view retain legacy defaults.
Already admitted bytes and live pins remain held when capacity falls; new work must fit the reduced
capacity. Saved settings, requested shares and effective constraints remain distinct.

The packed transfer ceiling remains 320 MiB for concurrent staging plus host capacity. Actual native
source allocation is deducted separately from request headroom before transfer admission; the
transfer ceiling is not a Float32 source/executor limit. This follow-on adds no native allocation,
UMA, whole-process or OOM guarantee, batching or additional frame context.

A terminal executor `Failure` with `OverBudget` may now supply positive retirement proof after an
earlier native prefix finishes: no device loss, unretired submission or required owner drain may
remain. Preview additionally checks actual Neutral/general display and packed transfer submissions;
final rendering checks output-color work. Only this proved path releases the touched phase before
fallback/recovery. Unknown work and the device's independent unproven latch remain charged; budget
refusal, cancellation or logical terminality alone does not clear them.
An independent legacy Neutral service post-submit fault confirms that one phase and one unproven
claim remain held until allocator-generation teardown. It does not execute the new resident
OverBudget branch; the explicit-cap refusal/recovery witness and reviewed downstream guards cover
that proved-retirement path separately.

Configured prior 23-member B Full/Half witnesses and explicit-cap refusal/recovery are recorded in
the [owning plan](../plans/gpu-reliability-and-preview.md). They use explicit fixture allowances and
do not prove ordinary omitted defaults. Separate ordinary-default Full/Half witnesses pass through
the application's saved-settings helper and sampled numeric delivery without request/scheduler/
producer overrides, retaining explicit 2 GiB cache and 1 GiB oracle allowances. Affected owning,
246-unit strict, desktop build, 20 Linux no-Vulkan fallback and final repository-quality gates pass.
This qualifies the bounded default-propagation and proved-retirement companion; native allocation
ownership, full-GUI/sustained performance and broader platform execution remain open. The accepted
first phase checkpoint and mounted-footage A evidence are unchanged.

Native work keeps its lease until exact retirement, including cancellation, publication failure
and retained/quarantined submission ownership. A logical terminal result or timeout is not fence
proof. A final executor's terminal `Failure`/`OverBudget` may release its phase only when no device
loss, unretired executor submission, owner drain or output-color submission remains. The device's
independent unproven-submission latch still prevents release; an unknown failure supplies no such
proof. Capacity updates preserve held leases/counts; measured zero refuses new admission. Bounded
readiness and external-capacity-release retries retain one active/newest intent, avoid retrying on
own disposal, cancel safely on shutdown, and preserve warm exact-cache reuse when no new phase is
needed. They do not create a polling loop or silently move GPU work to CPU.

The sampled host policy may release a previously observed constraint only using fresh complete
paired data in the same verified visible view and explicit removal for every remembered finite
domain. It does not prove hidden limits absent; the exact proof and reusable-retention recovery
remain owned by [evaluation primitives](evaluation-primitives.md).

Independent source/lifetime reviews and integrated validation accept this bounded first slice.
Required native coverage passes 56/56 with no missing fixtures and six production route proofs.
The final native retry witness, full 6000x4000 output parity, explicit-cap refusal/retirement and
sticky unknown-retirement checks pass. Affected strict checks close 195 translation units through
193 initial passes and two repaired fixture reruns; application quality and Linux CPU-only builds
and tests also pass. The reliability plan distinguishes initial failures from corrected evidence.
Published
products, cache aliases, external pins, later presentation, UMA backing, opaque metadata and driver
residuals still need allocation-owned accounting. This slice establishes no aggregate OOM guarantee,
width-two execution, sustained throughput, or native macOS/Windows qualification. The
[GPU reliability plan](../plans/gpu-reliability-and-preview.md) owns the remaining gates.

### Scenes of many layers (#343)

An imported SVG is hundreds or thousands of Shape layers in nested Merges. Before #343 such a scene
was refused or fell to the CPU at real frame sizes for reasons that were not memory the executor
held: the phase claimed the CPU reference's peak, retained coverage had a fixed allowance, the
command and coverage ceilings counted the whole scene, and the scene cache filled the device's image
slots. Each is now counted as what it is; no budget was raised.

- **The phase claims the executor's live set.** The CPU reference holds every entry of a Merge at
  once (its `peakPixelStorageBytes`). The executor folds a Merge into its accumulator one entry at a
  time and frees each entry after its fold, so a Merge holds the accumulator, one entry and the new
  output. `detail::gpuExecutorLivePeakBytes` walks the plan in executor order (a vector layer adds
  three frames of scratch; Shape and Text leaves hold nothing until their layer rasterises them;
  image and video sources are held at their own size) and the phase's minimum executor bytes is the
  smaller of that and the CPU peak, never less than one frame. A non-Normal fold plans its
  destination first (`GpuSceneBlendCommand::planDestinationFirst`) so the accumulator, not the
  entry, is the image kept across the fold.
- **Retained scene storage grows through the host root.** A phase whose envelope sets
  `growsRetainedSceneStorage` (preview and final render) claims coverage past
  `retainedCoverageByteLimit`, and the command storage past `metadataBytes`, from its host lease in
  16 MiB steps (`GpuProcessingPhaseLease::claimRetainedHostBytes`). The root admits, waits or refuses
  that claim like any other host claim, and it returns on release. A phase without the flag refuses
  as before.
- **Ceilings guard what they protect.** `GpuSceneExecutorBudgets::maxCoverageBytes` (256 MiB) bounds
  one coverage mask, not their sum; the sum is the host claim above. `maxCommands` is a
  runaway-plan guard at 2^20: a command holds no pixels, its storage is charged to the host claim,
  and the importer stops at 20,000 elements, so the former 4,096 refused real files for no memory
  reason.
- **The scene cache keeps stacks, not every layer.** A device has a fixed number of image slots
  (4096 per family, whatever their size). `GpuSceneCacheBudgets::maxRetainedImages` (2048) evicts by
  count as it evicts by bytes, and a scene of more than
  `GpuSceneExecutorBudgets::maxCommandsOfferingEveryImage` (1024) commands offers only its output and
  its Merges of three or more entries to the cache, so an edit re-runs one stack and a warm frame
  dispatches nothing.
- **Leaf shapes flatten like the CPU reference.** A shape whose path is already on the layer's pixel
  grid is flattened by `PathRaster::create` at the plain scale, as the CPU leaf arm does, instead of
  through the transformed path at the matrix norm, so curved outlines cover the same pixels.
- **A layer draws what can be seen.** Each layer's window is its transformed support intersected
  with what its readers can show of the frame (`detail::visibleSupportWindows`), and with its track
  matte's window when the matte is a plain alpha or luma multiply (`detail::matteBoundedLayers`).
  The CPU reference's layer arm takes the same windows, so the pixels are the same on both routes.
  A reader the demand pass does not understand asks for everything, so nothing it reads is clipped.
- **The phase claims images at their real sizes.** The admission floor takes each image as one
  frame. Once the scene is prepared, `detail::gpuExecutorLivePeakBytes` walks the plan again with
  every prepared window and the phase raises its claim to that live set when it is more
  (`GpuProcessingPhaseLease::raiseExecutorNeed`): a gradient node drawn in its own window larger
  than the frame is counted at its size. The need grows by the live set's excess over the executor
  bytes it already counts, so a measured envelope (#417) keeps its measured need plus that excess
  and never claims the unused stated working allowance again. The device admits, waits for or
  refuses the larger need; an executor ceiling below it refuses the scene. Images are counted at what the device allocates,
  not at their requested extent: the device reports `render::GpuAllocationSizes` (carried in
  `GpuAllocationBudget::allocation_sizes`, probed with `vkGetImageMemoryRequirements` on the image
  formats and memoised per size), `GpuProcessingPhasePolicy` rounds each claimed image with it, and
  the per-operation metadata buffers (axis tables, staging, status words) join the claim. A device
  that reports no sizes is claimed at the requested extent, as before.
- **The CPU reference folds Merges too.** When plan order would hold more than eight images and
  folding holds less, the CPU reference composites each Merge entry as it is ready and releases it
  (`detail::mergeFoldSteps`), so a CPU-only export of a few hundred layers holds a few frames.

The viewer route is proved by `bloom.ui.viewer_export_svg_artwork`: an imported compound shape with
a gradient fill, a clip matte, two nested opacity groups and a keyed corner and counter point is
shown in the interactive Viewer from `GpuPreparedPacked` with no CPU fallback, and the GPU route's
Viewer frames equal the CPU route's (RGB within one 8-bit code, exact alpha) at the first, a middle
and the last frame, as do RAM preview (cold and warm) and still, range and movie export.

Native batching still keeps up to eight kernels' outputs until their fence, bounded by
`maxNativeBatchOutputBytes`; the live set itself is asserted with batching off.
`bloom.runtime.gpu_svg_artwork_native` asserts each point as image sizes, never times. Results per
file and size are in [Vector Artwork](vector-artwork.md#rendering-and-memory).

### Rooted native image suballocation ownership (bounded two-family slice)

The implemented native follow-on covers explicitly rooted backing created by `createResidentImage`
and `createDisplayImage`: native RGBA32F process images and RGBA8 display images. Each opaque backing
implementation contains one move-only `NativeImageStorageToken`; aliases share that token. Legacy
unrooted creation remains unowned by this ledger. No new image/frame cap or reclamation queue is
introduced; existing owner-slot and native resource bounds remain in force.

Creation reserves its existing owner slot and creates an unbound image, then queries that image's
requirements and dedicated-allocation flags. VMA ranks compatible types using the existing
AUTO_PREFER_DEVICE preferences. Each admitted allocation uses UNKNOWN with one pinned type bit;
failed candidate allocation clears that bit and retries the remaining ranking, stopping at mask
zero or the physical memory-type count. Size/type/heap must reconcile downward to the actual
allocation before bind/view; there is no allocate-first top-up. Bind/view/reconciliation failure
destroys actual backing before claim undo rather than silently trying another type.
Allocation errors alone prove neither device health nor retirement of other submitted work.

Under one neutral root lock, the token transfers requirements bytes from the phase's remaining
native debit. A heap without DEVICE_LOCAL additionally acquires a separately bounded host backing
claim against delivered host capacity and the immutable native envelope, without borrowing the
source/packed-host grant. Both domains admit before a VMA backing attempt. HOST_VISIBLE on a
DEVICE_LOCAL BAR heap alone does not imply this host claim; UMA physical overlap remains unknown.
The runtime supplies the original sample's validity deadline. Expired delivery closes new
supplemental host admission without erasing held backing, and fresh same-size delivery can reopen
readiness. A new timestamp cannot manufacture freshness for an old sample.

`ownedNativeImageBytes`/`ownedNativeImages` count covered suballocations once;
`ownedNonlocalNativeHostBytes` is an overlapping host subset. `totalDeviceClaimBytes` and
`totalHostClaimBytes` include their respective phase remainders and covered owners. These are
admission constraints, not additive physical totals. Actual `VmaAllocationInfo::size` describes a
suballocation, not committed VMA blocks, driver/RSS, opaque metadata or allocator overhead. OCIO
texture arrays, WSI images and other allocation families remain outside this ownership slice.

Cache/output retention promotes only the retained image. Promotion drops its strong phase alias
outside the lock and keeps a weak origin for turnover: actual backing destruction may replenish
that still-live preadmitted envelope, while an expired origin releases credit normally. Aliases do
not pin the request indefinitely. Failed-prefix foldback requires proof of destroyed backing,
exact origin and the last internal alias with no external/cache holder. It cannot fabricate an
independent release revision or hide a genuine independent release. Ordinary published disposal
retains its release revision; this does not add an immediate UI wake channel. Typed refusals preserve
exact phase/domain and current refusal watermark. The genuine general-controller witness preserves
last-good output after an actual native allocation refusal, ignores unchanged-revision pulses and
recovers the newest intent once after an independent native cache release. Its ordinary retained-frame
return adds no preparation, graph, dispatch or native owned bytes/count. Final transfer/identity and
rooted general-display/Viewer precision checks also pass: cached return preserves exact identity
and adds no packed transfer, the general display matrix matches its CPU pixel oracle, and actual
Viewer/cache ownership retires covered native backing to zero.

Tokens survive existing foreign orphan and unknown quarantine paths with their exact allocator
generation. Actual native destruction precedes release; owner close or enqueue is not destruction.
The host reclaimer is not a native owner. The real UnknownFence witness retains covered token
bytes/count through actual fence/owner retirement while holding its admitted phase lease.
Independently, existing unproven phase protections stay sticky where engaged; this witness does not
assert those latch counters. A private unproved-flag control is distinct from submitted-work evidence.

Incremental neutral, genuine two-family allocation/nonlocal/fault/alias, submitted-fence and OCIO
pixel/cache witnesses pass, as detailed in the
[owning plan](../plans/gpu-reliability-and-preview.md). Corrected whole general-display and packed
helpers also pass with explicit root/retirer/phase-lease prerequisites; their earlier fixture
failures remain separate from production behavior. Required-route and all 323 owned desktop strict
checks are covered with their split-run history. Linux CPU-stub fallback covers 16 distinct strict
consumers and 17 passing owning checks plus one honest unavailable-tools skip; actual GPU routes
are qualified separately. Final application/CLI/media identity and dependency checks and repository
quality pass. Verified packaging and normal-app replacement follow this checkpoint. This does not
qualify all native allocations, block commitment, whole-process/UMA/OOM accounting, sustained
performance or native macOS/Windows execution.

### Rooted OCIO sampled-LUT image ownership (bounded LUT slice)

The OCIO follow-on extends the existing native image token to explicitly rooted sampled 1D, 2D and
3D LUT images. Program creation receives the exact phase lease already retained by the device,
before job begin; an equal numeric phase id from another root is insufficient. Immutable neutral
root identity is recorded even for zero-LUT programs without pinning a phase or native owner.
Both runtime program caches check that root, device epoch and native allocator affinity before
cold creation or warm reuse. Unproven authority is refused, with no second phase or quota grant.

Each move-only sampled resource carries the same token from temporary image ownership through
program retention or upload quarantine. Actual same-image requirements and dedicated flags feed
the existing ranked, pinned VMA allocation path. Native bytes transfer from the admitted phase;
actual nonlocal heap backing also claims the bounded, fresh same-root host subset before allocation.
HOST_VISIBLE device-local BAR memory is not classified as nonlocal. Actual VMA size/type/heap
reconcile before bind/view, and destruction precedes credit. These LUT images contribute to the
existing native-image counters; staging, uniform/status/packed buffers, sample vectors, samplers,
views and driver/allocator-block overhead remain outside allocation-owned image bytes.

Both runtime caches promote sampled storage only after successful cache insertion and bookkeeping.
Failed insertion or controlled partial promotion destroys the exact backing and folds the attempt's
own prefix back without fabricating an independent release revision. Warm reuse under a new phase
retains the same published image charge, rather than pinning the original phase. Rootless legacy
programs remain outside this accounting.

An intrusive program registration precedes native resources. Foreign final release marks its
preowned node without allocating or calling the driver. The exact allocator generation and native
owner drain after actual fence proof, outside the registry lock; native destruction precedes
unregistration. Live aliases and exact-generation upload quarantine keep preview/final exit
incomplete. Closing or timing out an owner is not retirement. The packed-host worker is not this
native owner, and no autonomous cleanup tail is introduced.

The authority and registry checkpoints pass separately from actual LUT backing evidence. Genuine
native fixtures now cover dimensional/heap identity, pre-backing refusal and faults, compatible
nonlocal freshness, forced RGBA format compatibility with unchanged HDR/3D CPU pixel oracles,
required two-texture prefix cleanup, aliases/foreign drain and both textured runtime caches.
Submitted display work with a fault-overridden unknown fence observation retains charges until
actual owner fence retirement; this does not demonstrate an actual GPU hang. Submitted upload
cancellation is a separate quarantine witness. Their exact attribution is recorded in the
[owning plan](../plans/gpu-reliability-and-preview.md). Nine remaining selected owners pass
separately from those direct native/cache proofs. All six required production writers and their
fourteen-check route gate pass; all 324 desktop owned-TU strict checks close through a split prefix
and disjoint remainder. The fresh Linux CPU-stub build and selected fallback tests pass with two
explicit unavailable checks skipped. All fifteen distinct fallback strict consumers pass without
diagnostics. Final repository quality passes after the six-file formatting-only repair, and the
source-aligned application/CLI/media identity readback has zero missing dependencies. Exact
integrated review, selective commit and verified packaging follow this bounded checkpoint.
No buffer/WSI/external,
whole-native, block/physical/UMA/OOM, sustained-performance or other-platform qualification is
adopted by this incremental checkpoint.

### Rooted display-transfer staging buffer ownership (bounded staging slice)

The first buffer follow-on adopts only the transient staging backing of `GpuDisplayTransfer`.
`NativeBufferStorageToken` moves with that backing into the existing native transfer slot and
quarantine. `ownedNativeBufferBytes`, `ownedNativeBuffers` and the overlapping
`ownedNonlocalNativeBufferHostBytes` distinguish these suballocations from owned images and packed
host payloads. They do not measure committed VMA blocks, driver overhead or process physical memory.

The rooted path creates one actual unbound buffer, queries its requirements and dedicated flags,
and preserves the original compatible-type restrictions while VMA ranks a shrinking candidate mask.
A pinned allocation retains the original random-access/mapped flags and explicitly requires
HOST_VISIBLE. Actual allocation size, type, heap, allocator epoch and mapped pointer are checked
before submission. HOST_VISIBLE on a DEVICE_LOCAL heap is not classified as nonlocal host backing;
physical overlap on UMA remains unknown. Unrooted buffers retain their legacy unowned path.

Native bytes and, for a nonlocal heap, already-planned host staging bytes transfer atomically from
the exact retained phase. Both total claims remain unchanged. This is conversion of an admitted
working grant, not the image token's additive nonlocal host claim or a new sample-dependent grant.
The declared staging-plus-output transfer allowance remains independent of the source image grant.
Insufficient paired debit refuses before a VMA backing attempt, although the unbound buffer may
already exist. Actual backing destruction precedes token foldback; its final phase alias drops
outside the ledger mutex. No cache promotion, extra queue or allocation/frame-count cap is added.

Foreign final release of a Pending transfer hands its preowned resources, token and pixels to the same slot
without allocation or driver work. A live Reserved slot is not reclaimable. Quarantined and Retiring
states remain pending for the exact generation through actual fence polling and destruction outside
the handoff lock; wrong-owner ticks do nothing. A closed owner's late handoff remains incomplete
rather than being reported as reclaimed. Preview and final owner exit now check this pending
transfer before acknowledging device/report retirement. Proven buffer refusal metadata uses the
existing typed phase-release retry boundary; an error code alone is not retirement proof.

Actual native identity, fault, nonlocal conversion, foreign handoff and same-generation recovery
checks pass. The production submitted-sticky witness separately retains buffer, phase and unproven
charges through an incomplete shutdown wait, then reaches covered zero through actual owner and
generation retirement. These privately overridden observations are not driver hangs, and the
independent unknown-report latch is not cleared. Neutral/General Full/Half/ROI CPU pixel parity,
retained aliases and cancellation/detach checks also pass. The actual device helper, three UI
preparation/control owners and genuine packed QWidget Viewer pass separately; UI controls alone
are not native pixel proof. All six required production writers pass the fresh fourteen-check
route gate. All 325 desktop owned-TU strict checks pass without diagnostics or repairs through
five prefix units and 320 disjoint consumers. The fresh Linux CPU-stub application/CLI/media and
eight selected fallback owners build and pass with explicit native-unavailable provenance. All
sixteen distinct fallback strict checks pass without diagnostics. The first final-quality run
passes all repository checks, and the source-aligned application/CLI/media identities pass readback
with zero missing dependencies. Exact integrated review, selective commit and verified packaging
follow the bounded qualification recorded in the
[owning plan](../plans/gpu-reliability-and-preview.md). Other upload, process, final/readback and
staging buffer creators remain unowned by this slice. No whole-buffer/native, physical/UMA/OOM,
performance or other-platform execution qualification is adopted here.

### Rooted resident-display processing buffers (bounded increment)

The 2026-10-13 continuation from issue #304 adopts the transient input, packed-output and status
buffers of `GpuResidentDisplay`. Each embeds one move-only `NativeBufferStorageToken`; the same
existing job owner retains it through submission, cancellation, failed-job cleanup and quarantine.
Native destruction precedes token foldback. Taking a completed display image destroys the retired
working buffers; keeping their pipeline alive does not pin the phase through these buffers.

Processing tokens transfer existing native working debit. A nonlocal processing allocation also
requires fresh additional host headroom, checked atomically before backing allocation. It cannot
spend the protected output or source/scratch/upload host partitions. Its host charge contributes
to `totalHostClaimBytes` and the overlapping `ownedNonlocalNativeBufferHostBytes` subset; it does
not reduce `hostReservedBytes`. Downward reconciliation and backing destruction release only that
additional host charge. Local/BAR backing consumes native debit without claiming physical UMA
separation. Output and upload tokens keep their existing conversion rules.

The native helper preserves AUTO memory-type ranking, compatible masks and original access/mapping
preferences. Processing input/output buffers may remain unmapped; mapped status backing must be
HOST_VISIBLE and have an actual mapped pointer. Actual VMA size/type/heap/allocator generation
must reconcile before bind/use. Typed device-budget, host-budget and awaiting-headroom refusals
retain exact phase/release metadata. A failed allocator, bind, reconciliation or map path destroys
backing and returns its token without fabricating capacity retry provenance.

Focused native checks exercise actual unmapped/mapped identity, one-byte-short refusal before a
backing attempt, forced nonlocal host admission and staleness, allocation/reconcile/bind/map faults,
partial production buffer-prefix refusal/cleanup with an external phase alias and exact own-release
revision, CPU pixel parity and ten alternating completion/cancellation cycles. Each settled cycle returns
covered claims and actual VMA allocation count/bytes to baseline at request capacity one. These
are small resident-display iterations, not the sustained fill/purge, 60-second playback or RSS
qualification required by issue #304. Neutral tests separately cover partition isolation, moves,
unproven-phase lifetime, freshness and integer boundaries.

The six required production preview/still/range/video/headless routes and native coverage gate pass
on Linux. Private-compositor packed animation/drag and native capacity-recovery checks also pass.
Independent focused regressions pass thirteen native checks and ten CPU-stub checks, with one
additional explicit native-unavailable outcome. Neutral ASan/UBSan/LeakSanitizer checks are clean.
These results do not establish the unresolved native-host LeakSanitizer allocation ownership.

Cached OCIO resources use the existing separate buffer factory until their own publication and
retirement adoption is implemented; attaching a transient token there would keep its whole origin
phase alive across cache hits. OCIO packed/UBO/status/LUT staging/scratch, coverage/palette,
composite/axis/map/affine/blend buffers, presentation storage and legacy/direct/oracle allocations
remain outside this increment. No operation or render-route GPU coverage exemption is introduced.
Committed VMA blocks, allocator/driver/RSS residuals, physical/UMA/OOM guarantees and macOS/Windows
native qualification remain open.

### Independently rooted presentation backing

Bloom-owned presentation uniform and overlay-staging buffers and overlay images use the device's
explicit processing root. Preview startup binds that stable root before native qualification;
the bound device rejects a different root and cannot retain another root's processing phase.
Presentation admission never borrows an arbitrary active frame, consumes a phase count slot or
pins the whole phase. New backing requires fresh host readiness, device readiness and sufficient
remaining device capacity, plus additional host capacity for an actual nonlocal heap. The paired
claims are committed before the VMA backing call, with exact generation/type/heap/size reconciliation
before binding. Original AUTO memory preferences and mapping requirements are preserved.

Presentation bytes remain included in the general owned native-image/buffer counters and are also
reported as separate presentation subsets. Downward provisional reconciliation and actual native
destruction return only their own claims. Native release exposes an independent admission revision;
there is no originating phase-release watermark to suppress. A failed allocator/bind/map path
destroys backing before releasing credit and reports no fabricated capacity-refusal provenance.

The existing presenter and target own these tokens through their render and presentation retirement
proofs. Overlay replacement and resize destroy old backing only after the previous render retires;
target quarantine retains the whole native owner and its charges. Vulkan WSI swapchain images are
explicitly reported as `PresentationEngineUnowned`: their physical allocation size is unknown and
Bloom does not count them as VMA-owned backing. Offscreen oracle attachments retain separate
caller-owned provenance. Driver allocations, committed VMA blocks, WSI memory and a physical/UMA
or whole-process OOM guarantee remain outside these requested-suballocation counters.

Focused neutral checks qualify phase isolation, provisional reconciliation, independent release
revisions, staleness and integer limits. Actual Linux native tests qualify uniform/image/staging
identity, pre-backing refusal, allocator/bind/map cleanup, ten overlay resize cycles and exact VMA
allocation-count/bytes return. Rooted offscreen production draws still pass the independent
QPainter oracle, including nonuniform 1280×720 images, with zero presentation claims after owner
retirement. The private Wayland lifecycle passes using the production coordinator's bounded
acquire mode through clear/resident presents, resize and acquired-unpresented close. Zero-timeout
poll-only acquire does not progress in that private fixture and is not qualified by these results.

### Rooted combined final-readback staging (bounded final-output slice)

The final-output increment adopts the transient staging buffer used by
`GpuOutputColorReadback` for the process payload and, optionally, one encoded RGBA32F or RGBA8
payload. One submission still transfers the composited output at the final host boundary. The
existing `FinalProcessingPhase` lease now travels explicitly through the output-color stage and
readback begin; it must match the exact proven phase retained by the device, root and generation.
An omitted lease is not authority to allocate against an unrelated rooted device phase.

The same buffer helper and `NativeBufferStorageToken` convert remaining admitted native and
already-planned nonlocal host staging debit once. Source images, CPU vectors and builder overlap
remain separately reserved. Actual requirements, mapped type/heap/size and the staging-plus-host
transfer ceiling still constrain allocation; insufficient paired debit refuses before a VMA backing
attempt, without adding a grant. The token follows staging through the existing readback slot;
actual backing destruction precedes foldback. These bytes use the existing buffer counters, not
CPU output ownership or a second native-image count.

Foreign final release of a Pending readback hands its existing resources to the native owner without
allocating or calling the driver. Reserved remains live; exact-generation Retiring remains pending
through fence proof and destruction outside the handoff mutex. Preview/final exit includes this
readback pending state. Proven budget refusal drops attempt-owned aliases before phase retirement,
allowing a second request at capacity one; loss or unknown work is not that proof. No timeout or
closed-owner state is reported as reclaimed success.

A host handoff may fail after the actual readback fence has completed. That failure releases its
attempt aliases only after checking the owner thread, device epoch, exact retained phase and every
independent readback, transfer and OCIO retirement witness. It remains a failed render, but does not
poison the next capacity-one admission. Unknown completion and device loss cannot satisfy this
proof; cached OCIO buffers remain independently charged until their actual destruction.

Focused native helper and complete evaluator evidence qualifies this behavior independently.
The evaluator's real submitted work with a privately overridden unknown observation retains buffer,
phase and unproven charges through an incomplete shutdown wait; actual native/generation teardown
reaches covered zero while the independent unknown device-report latch remains untouched. This is
not an actual GPU hang. The six-unit focused strict closure includes one fixture-reporting repair.
The [owning plan](../plans/gpu-reliability-and-preview.md) records exact attribution. The focused
readback checkpoint did not own CPU output allocations; the final-host follow-on below adopts them
separately. Upload staging remains excluded. These increments share one final-output union
qualification, commit and package; neither is a separately deployed milestone.
Other buffers, legacy/unrooted storage, committed blocks/driver/process/UMA accounting, OOM,
performance and other-platform qualification remain outside this scope.

### Rooted final CPU output ownership (bounded final-output slice)

`FinalPixelStorage<Rgba32f>` and `FinalPixelStorage<Rgba8>` distinguish explicitly rooted final
payloads from legacy vector storage. Process readback, the distinct process-image builder copy,
immutable image freeze and encoded output moves preserve their exact allocation authority.
`ownedFinalProcessHostBytes`, `ownedFinalEncodedHostBytes`, `ownedFinalHostMetadataBytes` and
`ownedFinalHostAllocations` record allocator-requested capacity and the admitted intrusive header,
not malloc/driver overhead. Old CPU builders and decoded/cache storage remain explicitly unowned;
a rooted allocation refusal never switches silently to legacy storage.

The existing final phase's conservative `48 * pixels + 2 * encodedBytes` overlap and metadata
reservation fund these allocations without a second allowance. Readback staging, CPU vectors and
builder copies have separate live charges while they overlap. Staging admission also leaves room
for the host headers. Freezing moves the same builder backing; internal takes remain provisional.
Checked direct publication or the complete evaluator's process-plus-encoded terminal-success
handoff validates both authorities before promoting either. Promotion precedes mailbox acceptance;
later cancellation or abandoned delivery may still discard the complete published product.
Changing a domain/root during moves or publishing only one half of a failed product is refused.

Published process and encoded storage detach from the originating phase and outlive native device
teardown. Their final release uses the existing explicit per-root host worker: allocation-free
handoff retains the payload/header charges until actual worker free, without a new queue, frame cap
or detached cleanup tail. Provisional cleanup remains synchronous on the supported worker/owner
path; foreign Pending native handoff is separate from provisional Ready disposal. Host shutdown
remains incomplete while final output pins survive and requires actual worker exit then collection.

Neutral allocation and actual production consumers pass their focused checks. A real encoded GPU
output witness preserves every-pixel process/encoded oracles, retains the published host products
with phase count zero through native generation teardown, then reaches covered zero after foreign
last-owner release and actual host-worker free. Exact gates and earlier failed consumer builds are
recorded in the [owning plan](../plans/gpu-reliability-and-preview.md). Twenty remaining owning tests
and a fresh fourteen-test required-route chain pass; the earlier still/video fixture shutdown
failures remain separately recorded. The 385 affected desktop strict checks close through reusable
prefixes, clean disjoint checks and one exact fixture arithmetic repair; original failures are
preserved. Fresh Linux CPU-stub tests and sixteen distinct fallback strict checks pass. Final
repository quality and source-aligned application/CLI/media artifact checks pass. Exact independent
source/doc freeze review precedes selective commit and immutable packaging.
This covers rooted final process/encoded payload and header
ownership only; upload, legacy CPU/decode/cache storage, other buffers/WSI, allocator
blocks/driver/process/UMA accounting, OOM,
performance and other-platform execution qualification remain separate.

### Rooted image-upload staging (bounded staging slice)

The next buffer increment covers only `GpuImageUpload` transient mapped staging. The existing
phase estimate stays `F + 3L`: fixed output overlap F, protected retained-source estimate L,
scratch estimate L and separately occupied upload allowance U=L. Ordinary owned host output uses
F only. `NativeBufferStorageToken::reserveUpload` consumes remaining U and native debit atomically
on every heap, including device-local/BAR staging. Nonlocal backing converts the same already-
planned host subset once; local backing leaves that conservative host estimate reserved. Actual
requirements beyond U refuse before a VMA backing attempt. Reconciliation and actual destruction
return the exact originating occupancy without adding a grant or manufacturing a release wake.
The source/scratch terms remain estimates, not ownership of decoded/prepared legacy cache images.

The token moves with `UploadStagingBuffer`, preserving sequential-write, mapped, HOST_VISIBLE
requirements and actual allocation/type/heap identity. Native destruction precedes token credit.
The existing native owner slot retains foreign or unproven work, rather than passing it to the
host-pixel retirement worker or publishing transient staging into a cache. Native pending-generation
exit wiring is part of this increment. The complete production evaluator submits a real upload,
then a controlled status observation retains 96 staging bytes, one allocation, the same nonlocal
subset and one phase/unproven charge through a 50 ms incomplete shutdown. Removing only that
observation override permits actual owner fence/backing/generation retirement and covered zero.
The independent unknown device-report latch is neither cleared nor required to reach zero.

Focused neutral and primitive checks pass. Real mapped upload, flush, paired refusal, controlled
faults, foreign owner drain and same-generation reuse are qualified independently from the whole
executor's cache/phase-detachment and capacity-one recovery checks. A manually unproven submitted
primitive control is not production evaluator shutdown evidence or a driver hang. The owning plan
records exact attribution and earlier fixture failures. The held 28-source checkpoint adds only
a Viewer readiness fixture correction to the accepted upload increment. Fresh required routes pass.
The first private Viewer whole run failed gesture/mapping/document assertions while shutdown
assertions passed; a corrected whole run preserves those assertions and passes after an actual
mapping-ready setup wait. This qualifies post-readiness functionality, not rapid selection-to-drag
latency. The 372 affected desktop strict checks close without diagnostics through eight representative
checks, 363 unchanged consumers and the final exact Viewer check. Fresh Linux CPU-stub tests
pass 10/10 with explicit unavailable provenance, and sixteen distinct fallback strict checks pass
without diagnostics. Final repository quality passes on its first attempt, including 1,477 format
checks and all unchanged owning checks. Source-aligned application, CLI and media-worker hashes/
BuildIDs are held with no missing dependencies or linked Vulkan. The 28 source paths and two
owning docs form this bounded checkpoint; exact independent freeze review precedes selective
commit and immutable packaging. Other buffers,
legacy storage, WSI, committed blocks/driver/process/UMA accounting, OOM, sustained performance
and other-platform execution are outside this proof.

### Prepared HOST retention (removed)

[ADR 0037](../decisions/0037-memory-follows-the-working-set.md) removed `GpuPreparedUploadCache`
and the operation cache's prepared-upload kind. Staging is scratch: the full-resolution RGBA32F host
buffer a video or image leaf converts for upload lives only while the prepared scene's upload
command holds it, on every route (interactive preview, RAM Preview, export, headless). A warm
rebuild converts again from the compact decoded frame (`DecodedVideoCache`) or the retained encoded
source revision. Measured on 720p H.264, one 10 s playback pass kept 3.52 GB of these copies that
linear playback never reused.

What in-flight correctness needs stays: `OperationCache::bindHostRetirement` roots the shared pool
on the explicit packed-host retirement owner, so evicted operation, decoded-media and source-revision
entries free on its worker after the GPU work that may read them, and `OperationCache::close()`
irreversibly refuses later stores and begins video-source shutdown before the host owner exits. GUI
preview and final contexts share the application pool; owned headless providers root a private pool
on their own owner. The bounded-source-reuse qualification this section used to record is history
in the repository log.

### Native loader LeakSanitizer attribution

The 2026-10-13 attribution follow-on uses the standalone Linux
[`native_loader_lsan_probe.cpp`](../../tools/quality/native_loader_lsan_probe.cpp). It links no Bloom,
Qt, media or project libraries. Loader-only admission is a clean unsuppressed control; creating a
Vulkan instance already reproduces dependency leaks, before any Bloom device, image or buffer
allocation. Required enumeration/device modes fail if the native API is unavailable. Each cycle
destroys its actual Vulkan device and instance before closing the loader, and records loaded-module
ranges before unloading so subsequent LeakSanitizer addresses retain module provenance.

With Vulkan and EGL vendor discovery both pinned to the qualified NVIDIA vendor, the two-cycle
zero-Bloom baseline and the refreshed scoped native-host witness both report 3,308 bytes in six
allocations. The original 2,468-byte/six-allocation receipt remains historical evidence; the current
received-message buffers differ in size. Matching DBus debug information names
`dbus_message_new_signal`, `_dbus_message_loader_queue_messages`, `internal_bus_get`,
`dbus_bus_register` and `dbus_connection_send_with_reply_and_block`. Same-process loader/module
records place their previously unnamed native callers in `libnvidia-eglcore.so`. Its stripped
internal functions are recorded by module-relative offsets rather than assigned invented names.
The same six allocation shapes and DBus paths occur with no Bloom code linked. This resolves the
previously unattributed DBus stack family to the native dependency path.

Default EGL discovery is a separate unsuppressed control. It also initializes `libEGL_mesa.so`
through `libEGL.so` and `libGLX_nvidia.so`; additional retained allocations reach `libdrm.so` through
`drmGetDevices2`, `drmProcessPciDevice` and `drmDeviceAlloc`.
That refreshed two-cycle baseline reports 4,300 bytes in fourteen allocations. Vendor selection
isolates dependencies; it is not a leak suppression or proof that default discovery is clean.
Neither result changes application ownership, performs global DBus shutdown, keeps a loader handle
alive to hide leaked reachability, or claims whole-native-host LeakSanitizer cleanliness. The
historical eleven-body instrumentation scope still excludes uninstrumented linked archives.

The probe is compiled separately from production. Set `icd_manifest` and `egl_manifest` to the
qualified vendor manifests; keep resulting addresses, local paths and raw loader logs private.

```sh
mkdir -p .cache
clang++ -std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
  -I build/dependency-prefix/include tools/quality/native_loader_lsan_probe.cpp \
  -ldl -o .cache/native-loader-lsan-probe
env -u DISPLAY -u WAYLAND_DISPLAY -u LSAN_OPTIONS \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  .cache/native-loader-lsan-probe --loader build/dependency-prefix/lib/libvulkan.so.1 \
  --mode loader
env -u DISPLAY -u WAYLAND_DISPLAY -u LSAN_OPTIONS \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  VK_DRIVER_FILES="$icd_manifest" __EGL_VENDOR_LIBRARY_FILENAMES="$egl_manifest" \
  .cache/native-loader-lsan-probe --loader build/dependency-prefix/lib/libvulkan.so.1 \
  --mode device --cycles 2 --snapshot
```

### Rooted packed-host payload ownership (bounded ownership slice)

This narrow G6B slice tracks allocator-requested RGBA8 pixel payloads for explicitly rooted
packed preview storage. It does not replace the accepted phase-reservation contract above.
`PackedRgba8Storage` distinguishes its legacy `std::vector` arm from a bounded allocator arm;
`PackedRgba8Ownership` reports `LegacyUnowned`, `RootedProvisional` or `RootedPublished`.
`PreparedReferenceDisplayBuffer::pixelOwnership()` exposes that distinction. Legacy storage remains
unowned by this accounting even when its cache retention cost is known.

The rooted allocator admits each actual allocator request before allocating the payload. During
construction, the allocation keeps its originating phase alive and moves covered bytes from the
phase estimate into the owned-payload count under the same budget lock. Failed or discarded
provisional storage folds that debit back atomically, without manufacturing a capacity-release
revision or a preview retry. The complete product's terminal-success handoff promotes its payload's
lifetime ownership independently of the origin phase, before sink or mailbox acceptance. Later
cancellation, forced terminal delivery or abandonment can still discard that promoted storage.
Aliases, mailbox delivery, cache retention and later external pins retain the same allocation
charge until actual backing storage is destroyed;
dropping one alias is not release proof. The locked ownership handoff leaves the total claim and
admission revision unchanged. Dropping the retained origin outside that lock may release the last
phase alias; that releases its remaining bytes/count and advances the revision normally.

`hostReservedBytes` continues to mean the remaining phase estimate. `ownedPackedHostBytes` and
`ownedPackedHostAllocations` describe only explicitly rooted allocator-requested pixel payloads.
`totalHostClaimBytes` combines that remaining estimate with owned payload bytes for admission,
without counting the covered bytes twice. These counters exclude allocation headers, debug proxies,
malloc bookkeeping, geometry/proof/envelope metadata, legacy vectors and native/driver allocations.
They are neither RSS nor committed allocator blocks, VRAM or UMA accounting. Cache-held product cost
remains a separate retention measure and may include metadata.

Independent review accepts ownership, conservation and off-UI production allocation within this
scope. Neutral core tests and sanitizers, final native Neutral/general Full/Half/ROI parity,
200 warm aliases, allocation/diagnostic double-fault terminal cleanup and cancellation retirement
pass. Actual Viewer cached return adds no GPU work, transfer or allocation; purge and external pins
preserve one charge until the last backing owner releases it. Native service-owner teardown does
not prove policy-root destruction because that fixture retains root aliases; neutral core tests
cover root destruction separately. Controller no-spin/recovery evidence is CPU/control evidence
using existing notification/sample pulses, with five-second sampling as the late-release backstop.
It does not establish a new immediate final-alias release wake.

The 51 affected strict translation units close through 47 initial passes and four corrected owning
reruns, plus a final RAM fixture rerun already in that map. Owning tests initially pass 15/16;
the repaired tiny RAM fixture asserts CPU OCIO/CpuReference and zero native dispatch. Linux
no-Vulkan tests initially pass 11/13; the two Qt-theme startup failures pass isolated reruns.
Application and repository quality pass. These corrected batches are distinct evidence, recorded
in the reliability plan. Existing required final-render coverage is unchanged; no final-render
host-payload route, unrooted legacy route, native allocation accounting, whole-process guarantee,
width-two execution or sustained/platform qualification is adopted here.

At the accepted `6acad4d4` ownership checkpoint, published backing still freed synchronously on
the final owner's thread, potentially Qt UI. Its Viewer witness proved ordering and conservation,
not off-UI free or large-free latency. The follow-on below implements that required retirement
repair; its affected qualification gates pass within the bounded scope below. The historical checkpoint is not an engineering-rule
waiver or a claim that legacy storage retires off UI.

### Off-UI rooted published-host retirement (bounded retirement slice)

The follow-on covers explicitly rooted published RGBA8 backing and its exact intrusive allocation
header/node. Admission transfers payload and header bytes from the phase estimate before allocation.
`ownedPackedHostBytes` remains payload-only; `ownedPackedHostMetadataBytes` counts the header/node;
`ownedPackedHostAllocations` counts backings. `totalHostClaimBytes` now combines remaining phase,
payload and header claims without duplication. Queue metadata is bounded by admitted bytes, with no
new fixed frame/allocation-count cap. Cache product retention cost remains separate. Debug/authority
proxies, malloc/stdlib/thread/stack overhead, other product metadata, legacy vectors, final RGBA32F
and native/driver/UMA backing remain outside this claim; the counters are not RSS or committed blocks.

Provisional cleanup remains synchronous on the admitted attempt worker, folding payload and header
back together before the final origin-phase release. The complete product's terminal-success
handoff still precedes sink/mailbox acceptance. Dropping the last RootedPublished alias performs
an O(1) intrusive enqueue/notify, without new allocation, backing free or ledger credit. The worker
frees backing outside the queue mutex and only then releases payload/header claims and registration.
Live, queued and in-progress backing stays charged until actual free. This does not move legacy
vector or every host-buffer release off UI, or establish a hard real-time/sustained latency bound.

An explicit joinable per-root `PackedHostRetirementOwner` is created inert; worker-only preparation
registers actual at-thread-exit acknowledgement before admitting backing. Tokens retain neutral
producer state, not the lifecycle owner or device. The frontend retains its owner through producer
stop, pin release, close, actual exit and collection. `beginShutdown()` closes registration/notifies;
live aliases keep shutdown incomplete. `exitAcknowledged()` polls without joining; `collect()` joins
only off an active UI loop or after that loop returns, outside the polling mutex. Destruction neither
joins nor detaches; normal exception/unwind requires the explicit lifecycle guard.

GUI shutdown first reaches task/native/export prerequisites, stops preview producers, releases live
Viewer/current/cache pins and closes the owner. Already-posted outgoing-editor deleteLater pins may
remain; their originating loop destroys them while charge/exit remain pending. Post-loop or unwind
teardown drains only already-posted safe deletion before borrowed dependencies die. Active shutdown
polls actual exit; collection follows event processing. A timeout is not quiescence.

Owned headless providers preserve worker-sampled roots; injected application roots remain borrowed.
Python uses module/interpreter-owned light records, shared-provider root identity and separate
per-Host schedulers. Early GC retains closing provider/native state before Host release; records
retain no Host, Session or Python object. Close rejects new calls, cancels native admissions, joins
Python producers and drops only runtime-owned references while Python is alive. Native/host waits
and collection release the GIL and lifecycle locks. Incomplete shutdown retains provider/owner/
scheduler and forbids finalize or dependency unload. CLI guards borrowed evaluator/scheduler
lifetimes through normal return and unwind. The owning scripting/output documents detail these
boundaries. One interpreter-owned cancellation-aware gate serializes owned native bootstrap only;
borrowed app providers and frame execution remain independent.

Core and GUI increments, actual held-free Viewer/GIL witnesses and the unchanged ordinary mixed-ICD
Python API with three repeats pass their focused gates. The repaired module boundary forwards the
same neutral owner through runtime's public surface, without duplicate state or checker exception.
Final owning tests close via an initial 19/20 plus a separate corrected controller prerequisite;
that controller witness is CPU/control evidence. Linux CPU-only/Python-enabled app, CLI and module builds pass, with all twenty affected
root/frontend suites passing in 9.93 s. Native branches there are unavailable, never GPU passes.
All 103 changed-header production/owning translation units pass the final strict sweep without
diagnostics or repair reruns. The reliability plan preserves exact split-run history.
No native allocation accounting, width-two, aggregate OOM or sustained/platform qualification is
adopted here.

### Measured host headroom and rooted backing

The processing policy samples machine availability and each visible finite process domain off the
UI thread. Those free-space readings already reflect completed rooted packed-preview and final
process/encoded pixel allocations. The root still charges those exact payloads and intrusive
headers until their backing is actually freed. Subtracting the full root claim from raw measured
free space would therefore charge retained frames a second time and could stop RAM-preview refill
while the OS still reports enough room for another frame.

The allocator keeps admission promises separate from completed backing. Only after the typed vector
finishes constructing and touching its pixel allocation does its token mark exact payload plus
header bytes as materialized. A worker captures that amount before the machine/process probe and
the cumulative materialized retirement amount after it. At most the initial amount less every
retirement during the probe is credited; allocations made during the probe receive no credit.
The sample credit belongs to that policy root and exact sample timestamp. Only a fresh matching
delivery can add it to measured machine and finite-process headroom, with saturation and the
physical-capacity ceiling. Ordinary admission continues to subtract the entire live root claim,
including unallocated phase estimates, host promises and held phases.
An older finite-process headroom ceiling remains uncredited when a later sample cannot verify that
same finite process reading; new backing may not have existed when that ceiling was measured.

Aliases and cache-held byte totals provide no credit. Provisional backed vectors are eligible
after construction; failed allocation, reserve-before-allocation, legacy vectors, source/coverage
estimates, transient processing claims and opaque native host backing are not. Published storage
queued for off-UI retirement remains materialized and charged until its worker starts the actual
free. At that boundary, the root revokes the sample's remaining credit before deallocation while
the charge stays held through the completed free. Retirement between sampling and delivery is
deducted atomically when the capacity is applied; a fresh OS sample is needed to restore capacity
afterward because allocator page retention need not raise measured free space. Duplicate and stale
delivery tighten capacity under the same root lock as retirement, so they cannot restore revoked
credit or reopen readiness. Measured zero, stale or incomplete delivery, cancellation and unproven
retirement retain their existing fail-closed behavior. This reconciliation does not measure
allocator overhead, whole-process RSS or GPU driver commitment.

### Resident request cancellation and failure retirement (G2E)

Independently requested cancellation or supersession discards publication, while the admitted stage,
its completion and native pins remain owned until exact submitted-work retirement. Expected
executor/display cancellation does not increment the resident backend-failure counter or replace
its error detail. A concurrent unexpected poll result or native failure still counts and reports
both the actual poll result and the underlying diagnostic. Actual native device loss takes
precedence over cancellation or an expired deadline. Scene, Neutral display and General OCIO
display loss participate in the same terminal-route decision; cancellation reaches all three arms.

A display's logical Failure is not fence proof. The resident service checks its exact outstanding
submission before clearing the active stage or consuming completion. The General OCIO native
program and runtime wrapper continue owner-thread fence polling after logical failure solely to
retire that submission. A successful later fence preserves the original failure and publishes no
pixels; actual driver loss preserves explicit loss truth. Healthy reuse is admitted only after the
outstanding submission clears. Repeated retirement polls do not count another job failure.

Current owner-drain truth is per executor and exact submission. PathCoverage's static historical
teardown fuse remains latched for audit and is not the service's current outstanding-work test.
The unchanged resident terminal policy permits 512 owner pumps before pipeline destruction invokes
its native bounded drain/quarantine. Exact native family ownership can remain retained afterward,
but the G2E checkpoint alone did not transfer its request charge into an aggregate quarantine
reservation. The first G6B phase slice above extends reservation lifetime without claiming unique
allocation-owned retained-byte accounting. Native stores and residual lifetime inventory remain G6
work; historical teardown counters do not establish hard physical reservation ceilings.

EditorArea replacement and workspace mutation continue requiring the existing WSI safe-to-mutate
proof. An unsafe result leaves the outgoing owners and mapped native surface intact. Compute fence
retirement is never substituted for that presentation proof.

### Oracle readback retirement (G2D2)

The full process/display and sparse display pixel oracles share exactly two process-wide native
submission reservations, with at most one reservation per exact allocator owner. Admission precedes
native allocation and refuses recoverably when occupied. This preserves the former aggregate
quarantine ceiling; it adds no production preview readback. Requested transfer bytes and actual VMA
staging allocation bytes must both fit the caller's byte budget, including sparse transfers whose
native allocation can exceed their payload. Private diagnostics charge actual staging bytes until
native destruction; this budget does not describe the subsequently allocated host pixel vector.

An unknown completion retains command resources, staging, allocator ownership, and the exact source
implementation's pre-reserved image slot. A synchronized source pin and a monotonic oracle token
prevent source destruction and stale proof reuse. Only the original allocator and thread lifetime
may query retirement. A real signalled fence clears that exact pin after native command children,
pool, fence and staging destruction; actual driver device loss permits cleanup and invalidates the
source, without publishing pixels. A timeout never proves loss or successful pixels.

Readback retry polls retirement before checking the source pin. The normal image-owner pump also
polls pending oracles before reclaiming released images, including a caller's foreign final release.
An allocator with zero pending oracles avoids scanning or locking the oracle store. Facade teardown
performs this drain before closing its owner lifecycle. Closed owners never reopen or allocate;
unproved submissions from an exited or closed owner remain charged within the same two-slot ceiling.
The asynchronous production final-output transfers keep their separate ownership and cancellation
contracts.

### Image owner reclamation (G2C)

Each native RGBA32F process image and RGBA8 display image reserves one owner-reclamation slot
before its first Vulkan allocation. Each image family has a process-wide ceiling of 4096 slots,
chosen from the existing cache/lease entry ceilings; it does not guarantee that every maximum-size
scene fits and does not increase any byte budget. Exhausted admission refuses image creation
without a native allocation and recovers when the exact owner destroys retiring images.

A foreign-thread final release or move-assignment replacement marks that image's already-owned
slot retiring without allocating. The slot preserves the image implementation, co-owned allocator,
and actual VMA allocation byte count through native destruction. Owner drains use the exact
allocator state and a monotonic thread-lifetime token together with the thread ID, so neither a
second device nor a recycled thread ID can reclaim the image. Preview pumps and teardown, offline
scene polling, and device facade teardown drain these slots; an exact allocator generation with zero pending image releases takes an atomic
fast path, even when unrelated closed generations remain quarantined. The per-image pending marker
is published before its slot becomes retiring and decremented only after native destruction. These image drains require final shared ownership release and retain any explicitly
unproved submission. They do not replace presentation's WSI retirement proof.

Destroying or replacing a device facade closes its reclaim-owner lifecycle after a final drain.
Existing images remain valid and co-own their allocator; their original owner may still release
them normally. New allocation through that closed generation is refused. A subsequent foreign
final release stays in a closed-owner quarantine: it never tears native resources down on another
thread. This is observable through process-wide retiring image counts and native allocation bytes,
even after the facade is gone. Repeated closed generations consume the same fixed stores and
therefore eventually refuse admission instead of growing an unbounded hidden retained set. There
is no automatic recovery once the original owner has stopped pumping. These diagnostics describe
image native allocations only; aggregate resource brokerage remains G6 work.

The synchronous oracle readback global quarantine/fuse and its source-image unproved flag remain
subsequent G2 work. This image slice deliberately preserves such images, charged and bounded,
without clearing an unproved flag from unrelated compute or presentation evidence.

Local native evidence (2026-10-08): both real VMA image families passed moved final ownership,
capacity refusal before allocation and recovery, move replacement, exact owner/device isolation,
thread-lifetime mismatch retention, retained admission/bytes through a deterministic pre-destroy
barrier, production preview pumping, and late closed-generation retention. Unproved-flag checks
assert retention only; source-fence recovery remains the following oracle slice. Composite and
resident-display CPU parity also passed with core and synchronization validation enabled and no
reported errors. These are local native results; cross-platform GPU qualification is not claimed.
Per-device image counts and native allocation bytes are exact; capacity, refusal, and reclaim
counters are process-wide. A bounded capacity refusal retains the existing AllocationFailed
operation result and its distinct cause is observable through the typed admission-refusal counter.

## Alternatives Evaluated

| Option | Strengths | Why it is not the working choice |
| --- | --- | --- |
| Direct Vulkan + MoltenVK | One explicit compute/render model, SPIR-V toolchain, strong diagnostics, direct control of queues, memory, and synchronization | High implementation cost; macOS is a portability subset and must pass real parity, performance, and presentation tests |
| Qt QRhi / QRhiWidget | Natural Qt Widgets integration; runtime Vulkan, Metal, D3D11/12, and OpenGL backends | Core QRhi requires `Qt::GuiPrivate` and has no cross-minor source or binary compatibility guarantee; its resource/threading model would couple the headless renderer to Qt |
| Dawn / WebGPU | Permissive native implementation over D3D12, Metal, and Vulkan; standardized capability and validation model | WGSL and WebGPU limits add another shader contract; native shared-resource facilities are implementation extensions and are not uniformly implemented, making pro-app interop and zero-copy presentation a risk today |
| Diligent Engine | Consistent C/C++ API, HLSL-oriented shader path, explicit compute and modern API backends | The open distribution's native Metal backend is commercially licensed; its free macOS route still uses a Vulkan portability layer, so it adds an abstraction without removing the main Apple risk |
| Native D3D12 + Metal + Vulkan | Maximum native control, tooling, and interop on each OS | Three production backends and shader paths multiply implementation and parity work before Bloom proves its first compositor |

Dawn is BSD-3-Clause licensed. Diligent Engine is Apache-2.0 overall, but its official support table
marks the native Metal backend as commercially licensed and notes independent third-party licenses.
These facts do not replace a distribution-time dependency audit.
[Dawn overview and backends](https://dawn.googlesource.com/dawn),
[Dawn shared texture memory status](https://dawn.googlesource.com/dawn/+/HEAD/docs/dawn/features/shared_texture_memory.md),
[Dawn license](https://dawn.googlesource.com/dawn/+/HEAD/LICENSE),
[Diligent Engine support and licensing notes](https://github.com/DiligentGraphics/DiligentEngine)

## Cross-Platform Spike Gate

This direction becomes accepted only after the same bounded prototype passes on Linux, Windows, and
Apple Silicon macOS:

1. Run the exact closed operation vocabulary offscreen with `RGBA32F` and apply the numeric gates in
   this document. Fixtures include an empty stack; one and two solids with reorder; integer and
   fractional translation; opacity and all alpha endpoints; negative/HDR, signed-zero, and subnormal
   values; odd, proxy, 4K, and 8K extents; and matrix, 1D-LUT, and 3D-LUT OCIO display processors.
2. Run the same SPIR-V assets and backend code on native Linux/Windows Vulkan and Apple Silicon
   MoltenVK. Record the immutable capability report, exact fixture digest, comparison summary, and
   every failure. No platform-specific tolerance adjustment is allowed.
3. Present qualified output in the Qt Widgets Viewer, exercise resize, panel replacement,
   detach/reattach, rapid request supersession, cancellation barriers, cache pressure, and resource
   generation invalidation. The normal path performs no full-frame CPU readback.
4. Trace cold OCIO shader compile, pipeline creation, frame evaluation, queue/fence delay, and image
   upload while UI thread sentinels run. Predeclare latency and memory budgets for the test profile;
   report measurements rather than substituting subjective visual review.
5. Run Vulkan validation cleanly; capture GPU timings and a platform-native frame trace; serialize
   and reject incompatible pipeline caches. Inject allocation, shader, pipeline, submit, completion,
   readback, presentation, and device-loss failures and verify the lifecycle contract.
6. Stress bounded queue admission, supersession, staged shutdown, repeated loss, and CPU fallback.
   The Viewer stays responsive, last-good frames remain marked stale, and no cancelled or
   old-generation frame becomes current.
7. Qualify and lock the Vulkan loader/headers, Vulkan-Hpp, VMA, shader compiler, MoltenVK, and every
   bundled transitive component through [`dependency-intake.md`](dependency-intake.md), including
   offline rebuild, SBOM, shipped-file verification, and notices.

Before production acceptance, expand parity CI or lab coverage beyond one GPU per operating system
to include at least AMD, Intel, NVIDIA, and Apple GPU families that Bloom claims to support. Results
are machine-readable capability evidence; screenshots or “looks correct” approval do not replace
the numeric fixtures.

If the prototype fails because of MoltenVK correctness, sustained performance, or presentation
interop, do not hide the gap behind reduced quality. Time-box an A/B spike of Dawn's native Metal
backend and a small native Metal backend behind the same Bloom interface, then replace or narrow this
working decision with evidence. If only the Qt embedding path fails, retain the offscreen Vulkan
result and compare the custom `QWindow` and isolated QRhiWidget presentation adapters.

## Dependency Licenses To Track

- Vulkan-Hpp and MoltenVK: Apache-2.0.
- Vulkan Memory Allocator: MIT.
- Glslang: multiple permissive components, led by BSD-3-Clause for glslang proper; audit the exact
  compiled target and optional dependencies.
- Qt: community packages dynamically link the pinned LGPLv3 Qt distribution under accepted
  [ADR 0014](../decisions/0014-apache-license-and-qt-distribution.md). Depending on a private Qt
  target does not provide an ABI promise and increases upgrade/build coupling.

[Vulkan-Hpp project](https://github.com/KhronosGroup/Vulkan-Hpp),
[Vulkan Memory Allocator project](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[MoltenVK licensing](https://github.com/KhronosGroup/MoltenVK#licensing)
