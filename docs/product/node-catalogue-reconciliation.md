# Node Catalogue Reconciliation

Status: working inventory; planned and not-planned rows are not implementation commitments

Updated: 2026-11-15

## Authority And Reading Guide

This is the detailed companion to [Node Catalogue](node-catalogue.md). It reconciles the June 2026
Node Implementation Matrix research with the repository's node catalogue. It preserves every
historical node row and name, including repeated names, and maps each row to what Bloom has now:
a shipped node under its current display name, a planned node tracked by an open issue, or no plan.
The node catalogue lists what is shipped and what is planned; this document explains where each
historical idea went.

A mapping means conceptual coverage, not verified implementation, exact algorithm equivalence,
or a serialized type-ID alias. Historical slash-separated names are preserved as written; they are
not automatically synonyms. No new node IDs or compatibility migrations are defined here.

The original research remains useful for rationale. Its library and scripting suggestions (a
different implementation language, graphics API, embedded scripting language and add-on package
format) were not adopted. Follow the repository's C++20, GPU backend, scripting, standards and
dependency-intake contracts.

Implementation evidence lives in registered definitions, evaluators and tests, with exact value-node
behavior documented in [Value Node Library](../architecture/value-node-library.md).

## Status Column

| Status | Meaning |
| --- | --- |
| Shipped | A node in the Add menu (or the Vector Artwork node made by SVG import) covers the idea. The mapping names the current display names; several rows map to more than one node, or to one node's mode. Where only part of a row is covered, the note says what is not. |
| Planned | An open issue tracks the node or family. The mapping names the planned node and its issue. A row that maps partly to shipped nodes says so in the note. |
| Not planned | No shipped node covers it and no open issue tracks it. The row stays as a record of the idea; it gains a plan only when an issue is opened. The idea may still be reachable through an existing node, as the note says. |

Current priorities and grouping are tracked in backlog issue [#384](https://github.com/afterlab/bloom/issues/384).

## Reconciliation Rules

- Normalize names for discovery, while retaining distinct data types and artist intents.
- Reuse primitives; a useful artist-facing recipe can still deserve its own controls or node.
- Keep pure evaluation, inspection/UI and side-effecting tasks as separate execution classes.
- Keep image time mapping, value animation and stateful simulation distinct.
- Preserve CPU reference behavior, exact time, channel/color/alpha meaning and explicit diagnostics.
- Require document, command, persistence, evaluation and three-platform behavior before calling a
  workflow complete; a primitive or source mode alone does not establish an independent node.

## Source / Input Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Read` | Shipped | Image | Stable asset identity and source interpretation; not a raw-path document contract. |
| `ReadSequence` | Shipped | Image | An Image source reads image-sequence assets, with explicit frame timing; the frame window and missing-frame handling follow the sequence asset. |
| `ReadVideo` | Shipped | Video | Worker-only decode with image and audio outputs, exact frame timing and preserved alpha. |
| `ReadAudio` / `AudioRead` | Shipped | Audio | Shared audio decode and timing model; effects follow it (Audio Equalizer, Audio Compressor, Audio Auto Level, Audio Mono, Audio Hiss). |
| `Solid` / `SolidColor` | Shipped | Solid | Two historical labels for the same generator family. |
| `Gradient` / `Ramp` | Shipped | Linear Gradient, Radial Gradient, Four-Color Gradient | Conic and angular ramps are modes of Gradient Field; a single unified Gradient node is planned in [#551](https://github.com/afterlab/bloom/issues/551). A color ramp value is a different type (Ramp Sample). |
| `Checkerboard` | Shipped | Checker | Image generator, distinct from a point grid. |
| `Noise` / `NoiseTexture` | Shipped | Noise | Fields family: Noise has image and mask outputs, and Voronoi, Pattern and Random Field are generators beside it. |
| `ColorBars` | Shipped | Test Card | Color-bar cards are among the Test Card patterns; no separate Color Bars node. |
| `Slate` | Not planned | None | A slate is a Text Source plus String Format and time readouts; no dedicated node is planned. |
| `Text` | Shipped | Text Source | Advanced shaping and layout are planned separately under [#345](https://github.com/afterlab/bloom/issues/345). |
| `Shape` | Shipped | Shape | Parametric and path authoring with shared fill and stroke; the Add menu offers Rectangle, Ellipse, Polygon, Star, Line, Triangle and Path presets. |
| `ImageFromChannels` | Shipped | Combine Channels | Image channels are not a single Color value; see Combine Color (Value) for that. |
| `MetadataRead` | Not planned | None | No metadata query node; evaluation does no filesystem reads. |

## Output / Viewing Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Viewer` | Shipped | Image Preview | Session viewing never changes Composition Output. |
| `Write` | Shipped | Output | Encoding and publication are cancellable export tasks, not a separate Write node. |
| `Output` | Shipped | Output | One evaluation endpoint per composition; named group outputs are Group Output. |
| `PreviewOutput` | Not planned | None | Preview and proxy targets are runtime capabilities (RAM preview, proxy work in [#372](https://github.com/afterlab/bloom/issues/372)), never a second source of project truth. |
| `Scope` / `Histogram` / `Waveform` / `Vectorscope` | Shipped | Histogram, Image Statistics | Image scopes are also a Scopes panel; audio waveform is a separate family (Audio Waveform). |
| `Compare` / `A/B` / `Difference` / `ErrorHeatmap` | Shipped | Image Compare | Difference, Split and Checkerboard modes with an explicit connected image output. |
| `MetadataView` | Not planned | None | No metadata inspection node. |

## Composite / Merge Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Merge` | Shipped | Merge | Distinguishes the compositing operator from the blend mode and states the alpha equations. |
| `Mix` / `Dissolve` | Shipped | Image Mix | Dissolve is an artist-facing use of an animated mix; not scalar or color Mix. |
| `KeyMix` | Shipped | Image Mix | The mask input gives the masked-mix (key mix) behavior. |
| `LayerMerge` | Shipped | Layer, Merge | Layer Output and Layer Stack bridge; no independent layer-render truth. |
| `MatteMerge` | Shipped | Mask Combine | Adds, subtracts, intersects or takes the difference of two masks. |
| `ContactSheet` | Not planned | None | Contact-sheet layout is not planned as a node. |
| `Switch` / `MultiSwitch` | Planned | Image Switch ([#531](https://github.com/afterlab/bloom/issues/531)) | Typed value switches (Switch Scalar, Switch Color and the others) are shipped; image branch selection is the planned node. |
| `BlendMode` | Shipped | Merge, Layer | Per-layer blend mode on the layer stack; no separate Blend Mode node. |

## Alpha / Matte Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Premult` | Shipped | Alpha Association | Premultiply mode with defined zero-alpha behavior. |
| `Unpremult` | Shipped | Alpha Association | Unpremultiply mode with defined zero-alpha behavior. |
| `SetAlpha` / `CopyAlpha` / `ShuffleAlpha` | Shipped | Set Alpha, Channel Copy, Shuffle | Replacing alpha is kept distinct from rearranging channels. |
| `InvertAlpha` / `ClampAlpha` | Shipped | Image Math, Image Clamp | Built from per-pixel math and clamping; there is no dedicated alpha-invert node, and Invert is a color mode. |
| `MatteDilate` / `MatteErode` / `Choke` | Shipped | Matte Dilate, Matte Erode | Coverage-domain morphology over a Mask. |
| `EdgeBlur` | Shipped | Matte Blur | Mask-domain Gaussian filtering. |
| `EdgeExtend` | Shipped | Edge Extend | Transparent-edge RGB reconstruction. |
| `GarbageMatte` / `HoldoutMatte` / `MaskMix` | Shipped | Mask, Mask Combine | Garbage, holdout and mask-mix recipes are built from mask nodes and Mask Combine. |
| `LuminanceMatte` | Shipped | Luma Key, Image to Mask | Declared luma coefficients and thresholds; Image to Mask also converts alpha or channels. |
| `ColorMatte` | Shipped | HSL Qualifier | Selection by hue, saturation and luma ranges; Chroma Key is planned separately ([#485](https://github.com/afterlab/bloom/issues/485)). |

## Channel / AOV Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Shuffle` | Shipped | Shuffle | Preserves arbitrary channel identities. |
| `CopyChannel` | Shipped | Channel Copy | Explicit source and destination channels. |
| `ChannelSplit` / `ChannelMerge` | Shipped | Separate Channels, Combine Channels | Multi-channel images, not vector component packing. |
| `RemoveChannel` / `RenameChannel` | Shipped | Select Channels | Keeps or drops channels by pattern; there is no channel-rename node. |
| `AOVSelect` / `AOVMerge` | Shipped | Pass, Image to Channel Set, Combine Channels | Pass extracts one named layer; there is no automatic beauty-reconstruction recipe. |
| `Cryptomatte` | Shipped | Cryptomatte | Identifier channels and metadata give a mask and an optional matted image. |
| `DepthExtract` | Shipped | Pass | Extracts a depth layer as a flat image; Z Defocus and Depth Fog read depth passes. |
| `NormalExtract` | Shipped | Pass | Extracts a normal layer as a flat image; no dedicated normal-aware node. |
| `MotionVectorExtract` | Shipped | Pass | Extracts a motion-vector layer; Vector Motion Blur reads it. |

## Color Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `OCIOColorSpace` | Shipped | Color Space Transform | Exact project configuration with explicit source and destination spaces (display name since [#609](https://github.com/afterlab/bloom/issues/609)). |
| `OCIODisplay` | Not planned | None | Display and view transforms are Viewer and project color settings, not process-pixel nodes. |
| `LUT1D` / `LUT3D` | Shipped | Apply LUT | LUT assets with interpolation, direction and look bypass (display name since [#609](https://github.com/afterlab/bloom/issues/609)). |
| `CDL` | Shipped | Grade | The ASC CDL form is the order of Grade's controls. |
| `Grade` | Shipped | Grade | Convenience controls over explicit grading operations. |
| `ColorCorrect` | Shipped | Grade | Lift, gamma and gain cover shadows, midtones and highlights. |
| `Exposure` | Shipped | Exposure | Scene-linear scaling by stops. |
| `Levels` | Shipped | Levels | Explicit black and white points, gamma and optional clamp. |
| `Curves` / `RemapCurve` | Shipped | Curves | Remap curves on scalar values use Field Remap or Map Range. |
| `HueSaturation` | Shipped | Hue/Saturation | Declared perceptual model. |
| `WhiteBalance` | Shipped | Grade | Temperature and tint controls; Auto Exposure also pulls toward neutral. |
| `ColorMatrix` | Shipped | Color Matrix | Declared color domain, coefficient layout and alpha behavior. |
| `Invert` | Shipped | Invert | Scene-linear color mode; separate alpha and mask modes are not built. |
| `Clamp` / `SoftClip` | Shipped | Tone Map, Image Clamp | Soft highlight compression and hard value limits; distinct from scalar Clamp. |
| `Posterize` / `Threshold` | Shipped | Quantize | Threshold and Posterize modes. |
| `FalseColor` / `GradientMap` / `Colorize` | Shipped | Colorize | Fill, Tint, Tritone, Gradient Map and Black and White modes; no separate False Color node. |
| `ColorSplit` / `ColorCombine` | Shipped | Separate Color (Value), Combine Color (Value), Separate Channels, Combine Channels | Single Color values and image channels have separate typed forms. |

## Transform / 2D Geometry Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Transform2D` | Shipped | Transform 2D | One shared coordinate and sampling contract. |
| `Translate` / `Rotate` / `Scale` | Shipped | Transform 2D | Convenience forms of the same operation. |
| `Reformat` / `Resize` | Shipped | Resize, Fit | Output format and resampling are related but distinct operations. |
| `Crop` / `Pad` | Shipped | Crop, Pad | Explicit display and data windows. |
| `CornerPin` / `PerspectiveWarp` | Shipped | Corner Pin | Projective mapping with invalid-transform diagnostics. |
| `GridWarp` / `SplineWarp` | Planned | 2D rig and puppet deformation ([#606](https://github.com/afterlab/bloom/issues/606)) | Editable grid and spline deformation domains are not yet specified; [#606](https://github.com/afterlab/bloom/issues/606) tracks the deformation gap. |
| `STMap` / `UVRemap` / `IDistort` | Shipped | ST Map, Displace | Absolute coordinates and relative displacement are not aliases. |
| `LensDistortion` / `Undistort` | Shipped | Lens Distortion | Forward and inverse direction with an ST Map output. |
| `Mirror` / `Tile` / `PolarTransform` | Shipped | Tile & Mirror, Procedural Warp | Tile and Mirror in Tile & Mirror; polar mapping is a Procedural Warp mode. |

## Filter / Image Processing Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Blur` / `GaussianBlur` / `BoxBlur` | Shipped | Gaussian Blur, Box Blur | Blur is a family label, not one unspecified kernel. |
| `DirectionalBlur` / `RadialBlur` | Shipped | Directional Blur, Spin Blur, Zoom Blur | Distinct sampling geometries. |
| `Defocus` / `BokehBlur` / `LensBlur` | Shipped | Z Defocus | Depth-driven defocus; performance audit in [#556](https://github.com/afterlab/bloom/issues/556). |
| `Sharpen` / `UnsharpMask` | Shipped | Sharpen | Unsharp mask with defined HDR behavior. |
| `Median` / `Despeckle` | Shipped | Denoise | Median and Dust and Scratches modes. |
| `Denoise` / `Degrain` | Shipped | Denoise | Bilateral and Temporal modes. |
| `EdgeDetect` | Not planned | None | No color-image edge-detect node is planned. |
| `Emboss` | Not planned | None | No emboss node is planned. |
| `DilateFilter` / `ErodeFilter` | Not planned | None | Color-image dilate and erode are not planned; Matte Dilate and Matte Erode work on masks. |
| `Glow` / `InnerGlow` | Shipped | Glow, Inner Glow | Shared blur and composite primitives with separate artist controls. |
| `LightWrap` | Shipped | Light Wrap | Foreground and background edge integration. |
| `DropShadow` / `InnerShadow` | Shipped | Drop Shadow, Inner Shadow | Shared matte, blur, offset and composite recipe. |
| `Grain` / `MatchGrain` | Shipped | Film Grain | Seeded synthesis; grain matching by analysis is not planned. |
| `ChromaticAberration` | Shipped | Chromatic Aberration | Channel-dependent coordinate mapping. |
| `Mosaic` / `Dither` / `Vignette` / `Roughen` | Shipped | Dither, Vignette | Mosaic and Roughen are not planned. |

## Keying Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `LumaKey` | Shipped | Luma Key | Explicit color space and matte output. |
| `ChromaKey` / `ColorKey` | Planned | Chroma Key ([#485](https://github.com/afterlab/bloom/issues/485)) | Color Key is a discoverability term for the same planned node. |
| `DifferenceKey` | Not planned | None | No difference-key node is planned; Image Compare in Difference mode is an inspection tool, not a key. |
| `AdvancedKeyer` (the historical row named a third-party keyer) | Not planned | None | Historical label for a third-party advanced keyer; no compatibility is promised. Chroma Key is planned in [#485](https://github.com/afterlab/bloom/issues/485). |
| `Despill` | Planned | Despill ([#485](https://github.com/afterlab/bloom/issues/485)) | RGB reconstruction separate from matte generation. |
| `KeyCleaner` / `CoreMatte` / `EdgeMatte` / `MatteCombine` | Planned | Matte Refine ([#515](https://github.com/afterlab/bloom/issues/515)) | Key cleanup, core and edge matte recipes; combining uses Mask Combine. |

## Time Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Time` / `TimeRemapValue` | Shipped | Time | A Time value node with seconds, frame and timecode readouts; reading context never resamples an image. |
| `TimeOffset` / `TimeShift` | Planned | Time Offset ([#346](https://github.com/afterlab/bloom/issues/346)) | Time Shift is historical terminology. |
| `FrameHold` / `FreezeFrame` | Planned | Frame Hold ([#346](https://github.com/afterlab/bloom/issues/346)) | Freeze Frame is historical terminology; Frame Rate Hold is a different, shipped node. |
| `TimeWarp` / `Retimer` | Planned | Time Remap ([#346](https://github.com/afterlab/bloom/issues/346)), Flow Retime ([#522](https://github.com/afterlab/bloom/issues/522)) | Interpolation and optical-flow reconstruction are separate capabilities. |
| `Loop` / `PingPong` | Shipped | Composition, Loop Value, Ping-pong | Composition has Hold, Loop and PingPong time modes; the value nodes loop scalars. No generic image loop node. |
| `PosterizeTime` | Shipped | Frame Rate Hold, Echo | Frame Rate Hold shows the input at a lower rate; Echo includes a posterize-time mode. |
| `FrameBlend` / `TemporalBlur` / `Echo` / `Slitscan` / `Trail` | Shipped | Echo | Bounded temporal window with explicit weights; Temporal Blur and Slitscan as separate nodes are not planned. |
| `MotionBlur2D` / `VectorMotionBlur` | Shipped | Motion Blur, Vector Motion Blur | Transform-derived versus flow- or vector-derived inputs with separate quality contracts. |

## Vector Shape Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Rectangle` / `Ellipse` / `Circle` / `Polygon` / `Star` / `Line` | Shipped | Shape | Presets in the Add menu: Rectangle, Ellipse, Polygon, Star, Line, Triangle. Circle is a constrained Ellipse. |
| `Path` / `EditableShape` / `FreehandPath` / `QuickShape` | Shipped | Path, Shape | Path value node plus Shape; authoring tools are viewer features. |
| `TextToShape` | Shipped | Text Outline | Glyph outlines as paths; a mask form and keyed term groups are planned ([#529](https://github.com/afterlab/bloom/issues/529), [#592](https://github.com/afterlab/bloom/issues/592)). |
| `ImageToShape` / `ContourExtract` | Not planned | None | Vectorize Image and contour extraction are not planned. |
| `MergeShapes` | Shipped | Merge Paths | Concatenation, distinct from boolean geometry. |
| `BooleanShapes` / `ShapeBoolean` | Shipped | Boolean Paths | Union, subtract, intersect and exclude with failure diagnostics. |
| `TrimPath` | Shipped | Trim Path | Path-domain length and segment semantics. |
| `OffsetPath` / `Thicken` | Shipped | Offset Path, Path Stroke | Thicken is the stroke-to-outline form in Path Stroke. |
| `RoundCorners` | Shipped | Round Corners | Non-destructive path modifier. |
| `ResamplePath` / `ResampleShape` / `SubdivideShape` | Shipped | Resample Path | Explicit sample spacing and point count. |
| `PathMorph` / `ShapeMorph` | Shipped | Shape Morph | Interpolates between two paths with explicit handling of incompatible topology. |
| `ShapeRepeater` / `ShapeAlongPath` / `ShapeDeform` / `ShapeSmooth` | Shipped | Warp Path, Resample Path | Deform and smooth are shipped; a path repeater and shape-along-path are part of the planned instancer ([#526](https://github.com/afterlab/bloom/issues/526)). |
| `Stroke` / `Fill` / `Dash` / `RasterizeShape` / `DrawShape` | Shipped | Path Stroke, Shape | Fill and stroke on Shape, dashes and brush stamps on Path Stroke; Shape is also the raster boundary. |
| `ShapeAttributes` / `ShapeExploder` / `PathDelete` | Not planned | None | Shape attributes, exploder and path delete are not planned. |

## Text Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Text` | Shipped | Text Source | Same source family, not another node. |
| `String` / `StringFormat` / `Counter` | Shipped | String nodes (Format, Concatenate and the others) | Counter is a recipe of Frame Number and Format. |
| `TextStyle` / `TextLayout` | Planned | Shaped Text Layout ([#345](https://github.com/afterlab/bloom/issues/345)) | Structured text differs from plain String; text style authoring is part of the layout work. |
| `TextOnPath` | Planned | Text On Path ([#345](https://github.com/afterlab/bloom/issues/345)) | Shaping precedes path positioning. |
| `TextAnimator` / `TextAnimate` | Planned | Text Animator ([#345](https://github.com/afterlab/bloom/issues/345)) | Selectors over stable glyph, word and line layout results. |
| `GlyphExplode` | Planned | Text as paths with keyed term groups ([#592](https://github.com/afterlab/bloom/issues/592)) | Element identity comes from shaped glyph layout. |
| `TextToMask` / `DrawText` | Planned | Text To Shape / Mask ([#529](https://github.com/afterlab/bloom/issues/529)) | Coverage differs from colored text imagery. |
| `TextToPoints` | Planned | Points generators ([#526](https://github.com/afterlab/bloom/issues/526)) | Text To Points is listed under the planned point generators. |
| `MergeText` | Not planned | None | Structured text merging is not planned. |

## Generative / Procedural Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `FractalNoise` / `Turbulence` / `WorleyNoise` / `Voronoi` | Shipped | Noise, Voronoi | Seeded and versioned; fractal sums are Noise controls and Worley cells are Voronoi. |
| `Plasma` / `Stripes` / `Rings` / `Dots` / `Scanlines` | Shipped | Pattern | Stripes, rings, dots, grid lines, hex and wave; Plasma and Scanlines are not planned. |
| `Halftone` | Not planned | None | Halftone is not planned. |
| `Kaleidoscope` | Shipped | Tile & Mirror, Field Transform | Kaleidoscope mode. |
| `Mosaic` | Not planned | None | Mosaic is not planned. |
| `Vignette` | Shipped | Vignette | Image effect in Stylize. |
| `LightRays` | Shipped | Light Rays | Sampling effect, not a light simulation. |
| `LensFlareBasic` | Shipped | Lens Flare | Artistic generator, not a physical lens simulation. |
| `CausticsPattern` | Not planned | None | Caustics pattern is not planned. |
| `Starfield` | Not planned | None | Starfield is not planned. |
| `Particle2D` | Planned | Particle System ([#521](https://github.com/afterlab/bloom/issues/521)) | Stateful seeded emitter with cached simulation; stateless scattering is part of [#526](https://github.com/afterlab/bloom/issues/526). |

## Field Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `ConstantField` | Not planned | None | No standalone constant field; an unconnected Field Math operand uses its value parameter. |
| `GradientField` / `RadialField` | Shipped | Gradient Field | Linear, Radial, Angular and Diamond. |
| `NoiseField` / `TurbulenceField` | Shipped | Noise | Value, vector, gradient and curl outputs; Field Derivative gives curl of any field. |
| `VortexField` / `RadialForce` | Shipped | Force Field | Vortex, Radial and Uniform forces; field evaluation is separate from simulation integration. |
| `DistanceField` | Shipped | Shape Distance | Signed distance and falloff from a shape or mask. |
| `ImageSampleField` / `ImageSample` | Shipped | Image Field, Field Sample | A sampled value and a spatial function are distinct outputs. |
| `VectorFieldFromImage` / `OpticalFlow` | Shipped | Optical Flow, Image Field | Optical Flow solves motion; Image Field reads an image as a vector field. |
| `PointsField` / `AttributeToField` | Not planned | None | Points as fields are not planned until point sets exist ([#526](https://github.com/afterlab/bloom/issues/526)). |
| `FieldCombine` / `FieldMath` / `FieldRemap` / `Remap` | Shipped | Combine Field, Field Math, Field Remap | Typed field algebra, not implicit image evaluation. |
| `FieldToMask` / `FieldToDisplace` / `DrawField` / `RasterizeField` | Shipped | Rasterize Field | Explicit extent and resolution at the domain boundary. |
| `Curl` | Shipped | Field Derivative | Curl, gradient and divergence by central differences. |

## Points / 2D Particle Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `PointGrid` / `Grid` | Planned | Points generators ([#526](https://github.com/afterlab/bloom/issues/526)) | Point Grid is distinct from the Checker image generator and the Test Card grid. |
| `PointScatter` | Planned | Points generators ([#526](https://github.com/afterlab/bloom/issues/526)) | Scatter with a stable seed. |
| `PointsAlongPath` | Planned | Points generators ([#526](https://github.com/afterlab/bloom/issues/526)) | Points Along Path. |
| `ShapeToPoints` | Planned | Points generators ([#526](https://github.com/afterlab/bloom/issues/526)) | Shape To Points. |
| `TextToPoints` | Planned | Points generators ([#526](https://github.com/afterlab/bloom/issues/526)) | Text To Points. |
| `PointAttributes` | Planned | Instancer ([#526](https://github.com/afterlab/bloom/issues/526)) | Per-instance attributes: position, rotation, scale, color, time offset, index and random. |
| `PointJitter` | Not planned | None | Point jitter is not planned apart from the seeded attributes in [#526](https://github.com/afterlab/bloom/issues/526). |
| `PointAdvect` | Planned | Particle System ([#521](https://github.com/afterlab/bloom/issues/521)) | Advection needs integration, history and seek/reset. |
| `PointRelax` | Not planned | None | Point relaxation is not planned. |
| `PointFilter` / `PointDelete` | Not planned | None | Point filter and delete are not planned. |
| `PointSort` | Not planned | None | Point sort is not planned. |
| `PointConnect` | Not planned | None | Point connect is not planned. |
| `MergePoints` | Not planned | None | Merge Points is not planned. |
| `CloneToPoints` | Planned | Clone To Points ([#526](https://github.com/afterlab/bloom/issues/526)) | Image, shape and text instancing. |
| `RenderPoints` / `DrawPoints` / `RenderTrails` / `PointTrail` | Planned | Draw Instances ([#526](https://github.com/afterlab/bloom/issues/526)) | Trails as accumulated history are part of [#521](https://github.com/afterlab/bloom/issues/521). |
| `PointDeform` | Not planned | None | Point deformation is not planned. |

## Math / Value Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Constant` / `ValueSource` | Shipped | Scalar, Integer, Boolean, Vector 2, Vector 3, Color, String | Typed value kinds are separate. |
| `Add` / `Subtract` / `Multiply` / `Divide` / `Modulo` | Shipped | Math, Integer Math, Vector 2 Math, Vector 3 Math | Shared checked primitives, no implicit color-vector equivalence. |
| `Power` / `Sqrt` / `Abs` / `Sign` | Shipped | Math, Sign | Domain and fallback behavior per operation. |
| `Min` / `Max` / `ClampValue` / `Saturate` | Shipped | Math, Clamp | Saturate is clamp to the unit range. |
| `MapRange` / `Fit` / `NormalizeValue` | Shipped | Map Range | Scalar Fit and Normalize are not image Fit or vector normalization. |
| `Floor` / `Ceil` / `Round` / `Fract` | Shipped | Rounding | Floor, ceiling, round, truncate and fraction. |
| `Sin` / `Cos` / `Tan` / `Atan2` / `RadiansDegrees` | Shipped | Math, Degrees To Radians, Radians To Degrees | Trigonometry with explicit angle units. |
| `Smoothstep` / `Step` / `MixValue` / `Lerp` | Shipped | Smoothstep, Math, Mix | Lerp is scalar and vector interpolation, not image compositing. |
| `Random` / `Seed` | Shipped | Random | Explicit seed. |
| `NoiseValue` | Shipped | Field Sample, Wiggle | A sampled scalar from Noise, or seeded motion; not an image generator. |
| `MakeVector` / `BreakVector` / `CombineVec2/3/4` / `SeparateVec2/3/4` | Shipped | Combine XY, Combine XYZ, Separate XY, Separate XYZ | Vector 2 and Vector 3; no Vector 4 schema. |
| `Math` / `VectorMath` | Shipped | Math, Vector 2 Math, Vector 3 Math | Operation selectors; menu aliases create no separate durable types. |
| `DotProduct` / `CrossProduct` / `Length` / `Distance` / `NormalizeVector` / `ScaleVector` | Shipped | Vector 2 Measure, Vector 3 Measure, Vector 3 Math | Dimension-specific operation contracts. |
| `RotateVector2D` / `AngleBetween` / `VectorToPolar` / `PolarToVector` / `ReflectVector` / `RefractVector` | Shipped | Rotate 2D, Polar To Cartesian, Cartesian To Polar | Angle Between, Reflect and Refract are not planned. |

## Logic / Control Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `If` / `Select` | Shipped | Switch Scalar, Switch Integer, Switch Boolean, Switch Vector 2, Switch Vector 3, Switch Color, Switch String | Value selection; image branch selection is Image Switch ([#531](https://github.com/afterlab/bloom/issues/531)). |
| `CompareValue` | Shipped | Compare | Explicit tolerance for floating comparisons. |
| `And` / `Or` / `Not` | Shipped | Boolean Logic, Boolean Not | Explicit conversion nodes handle numeric-to-Boolean. |
| `IsValidFrame` | Not planned | None | Source-range queries are not planned. |
| `IsNan` / `ReplaceNan` | Not planned | None | Non-finite handling is explicit at evaluation, not a node. |
| `ThresholdValue` | Shipped | Compare, In Range | Typed Boolean result. |

## Animation / Modifier Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `KeyframeCurve` | Shipped | Data Sample | Curves are durable parameter state; Data Sample reads a stored curve. |
| `Ease` | Shipped | Smoothstep | Keyframe easing is a parameter feature, not a node. |
| `Wiggle` | Shipped | Wiggle | Deterministic noise over exact time. |
| `Oscillator` / `Oscillate` | Shipped | Oscillator | Sine, square, triangle and saw functions with phase and units. |
| `Spring` | Not planned | None | A spring needs state or a seekable solve; see the Simulation node ([#594](https://github.com/afterlab/bloom/issues/594)) for stateful motion. |
| `Delay` / `Smooth` | Not planned | None | Value delay and smooth need history and are not planned apart from [#594](https://github.com/afterlab/bloom/issues/594). |
| `Quantize` | Shipped | Snap | Scalar step quantization; distinct from Quantize on images and Frame Rate Hold. |
| `LoopValue` | Shipped | Loop Value | Animation repetition separate from looping media frames. |
| `Envelope` | Not planned | None | Envelope is not planned; Audio Amplitude gives an audio envelope. |
| `AudioAmplitude` | Shipped | Audio Amplitude | Usable as a parameter driver. |
| `LinkParameter` | Not planned | None | Every parameter has a linkable socket, which is the driver link; there is no separate node. |
| `Expression` | Not planned | None | A general expression runtime has no adopted contract. Function evaluates a typed formula. |

## Audio Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `AudioRead` | Shipped | Audio | Same source as the Source row. |
| `AudioWaveform` | Shipped | Audio Waveform | Analysis data as a number list. |
| `AudioSpectrum` | Shipped | Audio Spectrum | Deterministic windowing and FFT analysis. |
| `AudioAmplitude` | Shipped | Audio Amplitude | Prepared analysis data and smoothing; further audio analysis nodes are Audio Bands, Audio Sample, Audio Features, Audio Beat and Audio Pitch. |
| `AudioSyncMarker` | Not planned | None | Durable audio sync markers are not planned; Audio Beat gives beat timing. |
| `AudioMixPreview` | Not planned | None | Audio preview and mixing are services, not nodes. |

## Analysis / Utility Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `PixelProbe` | Shipped | Image Statistics | Region-based measurement; the Viewer has pixel readouts. |
| `Bounds` / `FormatInfo` / `CompDetails` | Shipped | Layer Bounds, Composition Size, Composition Duration, Frame Rate | Image bounds differ from composition settings; measured bounds before the image pass are planned ([#601](https://github.com/afterlab/bloom/issues/601)). |
| `MetadataWrite` | Not planned | None | Metadata authoring is not planned. |
| `Checksum` | Not planned | None | Checksum is not planned as a node. |
| `Debug` | Shipped | Image Preview | Diagnostics never become render truth. |

## Graph Organization Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `Dot` / `Reroute` | Shipped | Reroute | Dot is a historical name for graph routing. |
| `Backdrop` / `Note` | Not planned | None | Named frames around cards are editor layout, not node types; there is no note node. |
| `Group` / `Precomp` | Shipped | Node Group, Composition | Group encapsulation and time-mapped composition instances are not aliases. |
| `Cache` / `FileCache` | Planned | Particle System ([#521](https://github.com/afterlab/bloom/issues/521)) | File Cache disk bake is part of the planned job. |
| `Bypass` / `Disable` | Not planned | None | A node can be muted to pass through; that is a node state, not a node type. |
| `PublishInput` / `PublishOutput` / `ParameterPanel` | Shipped | Group Input, Group Output | Parameter panels are a UI projection over command-backed parameters. |

## Plugin / Scripting Nodes

| Historical names | Status | Current mapping | Notes |
| --- | --- | --- | --- |
| `OpenFX` | Not planned | None | OpenFX hosting is not planned. |
| `PythonScript` | Not planned | None | Python is an automation and task surface, not a node. |
| `Expression` | Not planned | None | A general expression runtime has no adopted contract. |
| `CustomKernel` | Not planned | None | A custom kernel boundary is not planned. |
| `ExternalProcess` / `CommandHook` | Not planned | None | External-process nodes are not planned. |
| `BloomAddon` / `NativeAddonNode` | Not planned | None | Add-on node contributions use the module and add-on registries; external native ABI and a package format are not planned. |
