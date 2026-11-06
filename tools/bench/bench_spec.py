"""The benchmark media specification shared by the generator, manifest, project builder and runner.

Plain data only (no bpy, no numpy), so Blender's Python, the system Python and Bloom's embedded
Python can all import it.
"""

MEDIA_ROOT_ENV = "BLOOM_BENCH_MEDIA"
DEFAULT_MEDIA_ROOT = "bench-media"  # relative to the current directory
SEED = 1729
FPS = 24
BLENDER_SERIES = "4.5"  # LTS series the stored generator is pinned to

IMAGE_FORMATS = ("png8", "png16", "exr_half_piz", "exr_float_zip", "exr_multilayer")
# ADR 0026: an image layer starts on the first part's root layer. Blender's multilayer EXR has no
# root color channels, so its projects select the composited color layer explicitly; without it the
# footage has no Image output ("No color layer - use Channels") and the cell measures nothing.
COLOR_LAYER = {"exr_multilayer": "Composite.Combined"}
VIDEO_FORMATS = ("h264", "hevc10", "prores422hq", "prores4444", "dnxhr_hqx", "av1", "vp9")
# tier: (width, height, frames, image formats, video formats)
TIERS = {
    "720p": (1280, 720, 240, IMAGE_FORMATS, VIDEO_FORMATS),
    "1080p": (1920, 1080, 240, IMAGE_FORMATS, VIDEO_FORMATS),
    "2160p": (3840, 2160, 240, IMAGE_FORMATS, VIDEO_FORMATS),
    # Reduced 8K tier: streaming/memory invariant and per-image GPU ceiling stress.
    "4320p": (7680, 4320, 48, ("png16", "exr_half_piz", "exr_multilayer"),
              ("prores4444", "hevc10")),
}
TIER_ORDER = ("720p", "1080p", "2160p", "4320p")

# Pinned in the manifest. Change them only together with regenerated media and manifest.
RENDER_SETTINGS = {
    "engine": "CYCLES",
    "device": "GPU (OptiX)",
    "samples": 128,
    "adaptiveThreshold": 0.02,
    "denoiser": "OPENIMAGEDENOISE (GPU)",
    "maxBounces": 6,
    "seed": SEED,
    "animatedSeed": False,
    "viewTransform": "Standard",
    "filmTransparent": True,
    "passes": ["Combined", "Depth", "Normal", "CryptoObject (4 levels)", "AOV bench_color",
               "AOV bench_id", "Noisy Image"],
    "multilayer": "OPEN_EXR_MULTILAYER half ZIP (depth, normal, Cryptomatte, value AOV float)",
    "flat": {"png8": "PNG RGBA 8-bit, compression 15", "png16": "PNG RGBA 16-bit, compression 15",
             "exr_half_piz": "OpenEXR RGBA half PIZ", "exr_float_zip": "OpenEXR RGBA float ZIP"},
    "frameNumbering": "1-based, 4 digits",
}

# Sequence file name inside <root>/<tier>/<format>/ ; '####' is the frame number.
SEQUENCE_PATTERN = {
    "png8": "bench_{tier}_png8.####.png",
    "png16": "bench_{tier}_png16.####.png",
    "exr_half_piz": "bench_{tier}_exr_half_piz.####.exr",
    "exr_float_zip": "bench_{tier}_exr_float_zip.####.exr",
    "exr_multilayer": "bench_{tier}_multilayer.####.exr",
    "png16_mist": "bench_{tier}_mist.####.png",
}

# #483: passes derived from the rendered media rather than rendered themselves (bench_media.py
# derive; run_scenarios.py writes the frames a scenario needs on first use). "png16_mist" is a
# 16-bit PNG mist pass of the multilayer EXR's camera depth: mist = clamp((Z - start) / depth,
# 0, 1), 1 where the render is empty (the common mist encoding, 0 at the camera's near start), in
# all of R, G and B with opaque alpha, untagged (no sRGB chunk): data, not a picture.
DERIVED_PASSES = {
    "png16_mist": {"source": "exr_multilayer", "channel": "ViewLayer.Depth.Z", "start": 5.0,
                   "depth": 35.0},
}

# Video encodes from the 16-bit PNG master. Non-alpha codecs flatten over black (premultiply).
# Footage video is tagged BT.709 primaries/transfer/matrix, limited range: the common delivery
# tagging. Colour accuracy is measured with the chart, not with this footage.
_TAGS = ["-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709",
         "-color_range", "tv"]
VIDEO_ENCODES = {
    # format: (file suffix, alpha, pix_fmt, encoder args)
    "h264": ("h264.mp4", False, "yuv420p",
             ["-c:v", "libx264", "-preset", "medium", "-crf", "18", "-profile:v", "high"]),
    "hevc10": ("hevc10.mp4", False, "yuv420p10le",
               ["-c:v", "libx265", "-preset", "medium", "-crf", "20", "-profile:v", "main10",
                "-tag:v", "hvc1"]),
    "prores422hq": ("prores422hq.mov", False, "yuv422p10le",
                    ["-c:v", "prores_ks", "-profile:v", "3", "-vendor", "apl0"]),
    "prores4444": ("prores4444.mov", True, "yuva444p10le",
                   ["-c:v", "prores_ks", "-profile:v", "4", "-alpha_bits", "16",
                    "-vendor", "apl0"]),
    "dnxhr_hqx": ("dnxhr_hqx.mov", False, "yuv422p10le",
                  ["-c:v", "dnxhd", "-profile:v", "dnxhr_hqx"]),
    "av1": ("av1.mp4", False, "yuv420p10le",
            ["-c:v", "libsvtav1", "-preset", "8", "-crf", "30"]),
    "vp9": ("vp9.webm", False, "yuv420p",
            ["-c:v", "libvpx-vp9", "-crf", "31", "-b:v", "0", "-row-mt", "1",
             "-deadline", "good", "-cpu-used", "4"]),
}
VIDEO_TAGS = _TAGS


def tier_size(tier):
    width, height, frames, _, _ = TIERS[tier]
    return width, height, frames


def sequence_path(tier, image_format):
    """Media-root relative pattern with '####' for the frame number."""
    return f"{tier}/{image_format}/" + SEQUENCE_PATTERN[image_format].format(tier=tier)


def video_path(tier, video_format):
    return f"{tier}/video/bench_{tier}_" + VIDEO_ENCODES[video_format][0]


def footage_assets():
    """Every footage asset id in tier order: '<tier>/<format>'."""
    for tier in TIER_ORDER:
        _, _, _, images, videos = TIERS[tier]
        for image_format in images:
            yield tier, image_format, "sequence"
        for video_format in videos:
            yield tier, video_format, "video"

# Per-node GPU cost (`run_bench.py nodes`, benchmarks.md "Node cost"): the native benches (build
# targets relative to the build directory), the tiers they run at, and the budgets for each new
# node case's ms per frame against its cheap reference (a Grade) and its closest existing
# comparable at the same resolution. A case may carry its own budget; "default" covers the rest.
NODE_BENCHES = {"matte": "src/runtime/tests/bloom_runtime_gpu_matte_node_bench",
                "depth": "src/runtime/tests/bloom_runtime_gpu_depth_node_bench",
                "lens-flare": "src/runtime/tests/bloom_runtime_gpu_lens_flare_node_bench",
                "film-grain": "src/runtime/tests/bloom_runtime_gpu_film_grain_node_bench",
                "motion": "src/runtime/tests/bloom_runtime_gpu_motion_node_bench"}
NODE_COST_TIERS = ("1080p", "2160p")
NODE_COST_BUDGETS = {
    "default": {"reference": 4.0, "comparable": 2.0},
    # Mask Combine reads two Mask nodes where Luma Key is one node.
    "mask-combine-mix": {"reference": 4.0, "comparable": 2.75},
    # N node Masks: N Mask nodes plus one combine each, held to N Image Mix nodes by the same Masks;
    # against one Grade the count is the cost (about 0.6 of a Grade per Mask).
    "matte-layer-masks-4": {"reference": 5.0, "comparable": 1.5},
    "matte-layer-masks-16": {"reference": 14.0, "comparable": 1.5},
    # A Cryptomatte route costs what the #364 node itself costs, so it is held to that node's
    # existing Image output. #549: the node uploads its two rank layers each frame (66 MB at 1080p,
    # 265 MB at 2160p, against the EXR Grade's half-float beauty), about 3 EXR Grades at 1080p
    # and 6 at 2160p.
    "cryptomatte-layer-mask-1": {"reference": 7.0, "comparable": 1.5},
    "cryptomatte-layer-mask-16": {"reference": 7.0, "comparable": 1.5},
    "cryptomatte-layer-mask-64": {"reference": 7.0, "comparable": 1.5},
    "cryptomatte-layer-mask-256": {"reference": 7.0, "comparable": 1.5},
    "cryptomatte-effect-mask-16": {"reference": 7.0, "comparable": 1.5},
    # #483 Z Defocus is a two-dimensional gather (1 + 4K(K + 1) samples a pixel at K rings: 289 at
    # Normal) where its comparable Gaussian blur is separable (2 (2r + 1) taps), plus its circle of
    # confusion map, tile maximum and prefilter; its cost follows the rings and the share of the
    # frame out of focus. Held about 25% over the 2026-11-06 measurement at either tier. Depth Fog
    # keeps the default budgets.
    "z-defocus-8": {"reference": 10.5, "comparable": 6.5},
    "z-defocus-32": {"reference": 17.0, "comparable": 8.5},
    "z-defocus-128": {"reference": 22.0, "comparable": 4.5},
    "z-defocus-32-quality-0": {"reference": 11.5, "comparable": 5.5},
    "z-defocus-32-quality-2": {"reference": 21.5, "comparable": 10.5},
    "z-defocus-32-quality-3": {"reference": 21.5, "comparable": 10.5},
    "z-defocus-uniform-32": {"reference": 12.0, "comparable": 5.75},
    "z-defocus-bokeh-32": {"reference": 20.0, "comparable": 9.75},
    # #507 Lens Flare is one fixed pass whatever its counts, held about 25% over the 2026-11-06
    # measurement; a reading of the source or a matte adds its probe and its slot in the atlas.
    "lens-flare": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-only-glow": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-only-streak": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-only-ghost": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-only-ring": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-only-iris": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-only-hoop": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-only-anamorphic": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-largest": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-largest-size-4": {"reference": 1.75, "comparable": 0.35},
    "lens-flare-source": {"reference": 2.25, "comparable": 0.45},
    "lens-flare-occlusion": {"reference": 2.25, "comparable": 0.45},
    # #486 Film Grain is one fixed pass whatever its size, held about 25% over the 2026-11-06
    # measurement against a Grade and a procedural Noise.
    "film-grain": {"reference": 1.25, "comparable": 1.25},
    "film-grain-size-0.25": {"reference": 1.25, "comparable": 1.25},
    "film-grain-size-4": {"reference": 1.25, "comparable": 1.25},
    "film-grain-size-32": {"reference": 1.25, "comparable": 1.25},
    "film-grain-held": {"reference": 1.25, "comparable": 1.25},
    "film-grain-transparent": {"reference": 1.25, "comparable": 1.25},
    # #484, every motion-blur node, held about 25% over the 2026-11-06 run on the preview route
    # (benchmarks.md, "Motion blur"). A heavy mode is relaxed for what it computes, not for how it
    # waits: its breakdown there separates the two.
    # Directional, Spin and Zoom: one pass of up to 64 taps, against two separable Gaussian passes.
    "directional-8": {"reference": 2.0, "comparable": 1.25},
    "directional-32": {"reference": 2.75, "comparable": 1.75},
    "directional-128": {"reference": 3.25, "comparable": 1.0},
    "directional-32-draft": {"reference": 2.75, "comparable": 1.75},
    "directional-32-standard": {"reference": 2.75, "comparable": 1.75},
    "directional-32-high": {"reference": 3.25, "comparable": 2.25},
    "spin-10": {"reference": 3.25, "comparable": 2.25},
    "spin-45": {"reference": 4.5, "comparable": 3.0},
    "zoom-10": {"reference": 3.25, "comparable": 2.25},
    "zoom-50": {"reference": 4.0, "comparable": 2.75},
    # One resample per sample, against one resample.
    "transform-blur-draft": {"reference": 2.0, "comparable": 2.25},
    "transform-blur-standard": {"reference": 2.0, "comparable": 2.25},
    "transform-blur-high": {"reference": 2.5, "comparable": 3.0},
    "transform-blur-final": {"reference": 1.75, "comparable": 2.0},
    # The upstream once per sample and an accumulator atlas of two moves, a blend and a pass per
    # sample: 64 samples at High, a real cost the card states.
    "motion-blur-draft": {"reference": 5.25, "comparable": 3.75},
    "motion-blur-standard": {"reference": 13.0, "comparable": 8.5},
    "motion-blur-high": {"reference": 34, "comparable": 24},
    "motion-blur-final": {"reference": 34, "comparable": 36},
    # As Motion Blur, per echo.
    "echo-4": {"reference": 5.25, "comparable": 5.75},
    "echo-16": {"reference": 8.25, "comparable": 9.75},
    "frame-blend-4": {"reference": 7.0, "comparable": 7.0},
    # Tiles, NeighborMax and a 16-sample gather, against one Directional pass.
    "vector-gather-8": {"reference": 5.25, "comparable": 3.5},
    "vector-gather-32": {"reference": 5.75, "comparable": 3.0},
    "vector-gather-128": {"reference": 3.75, "comparable": 1.75},
    "vector-gather-limit-32": {"reference": 3.75, "comparable": 2.0},
    "vector-gather-limit-128": {"reference": 3.75, "comparable": 2.0},
    "vector-gather-limit-512": {"reference": 4.0, "comparable": 2.0},
    # Real compute, the fence holds the frame: a soft splat per shutter sample, done as a gather because
    # the portable pipeline has no atomics, against the 16-sample gather.
    "vector-splat-8": {"reference": 39, "comparable": 13.0},
    "vector-splat-32": {"reference": 39, "comparable": 13.0},
    "vector-splat-128": {"reference": 39, "comparable": 17.0},
    # Two splats and an outside-in fill, against one splat.
    "vector-layered-32": {"reference": 82, "comparable": 2.75},
    # Real compute, the fence holds the frame: hundreds of passes of a solver, and nothing existing
    # solves anything, so the yardstick is the widest Gaussian. Final solves at full resolution.
    "optical-flow-dis-draft": {"reference": 7.5, "comparable": 2.25},
    "optical-flow-dis-standard": {"reference": 12.5, "comparable": 3.75},
    "optical-flow-dis-final": {"reference": 32, "comparable": 8.0},
    "optical-flow-tvl1-draft": {"reference": 6.5, "comparable": 2.0},
    "optical-flow-tvl1-standard": {"reference": 14.75, "comparable": 4.0},
    "optical-flow-tvl1-high": {"reference": 32, "comparable": 9.25},
    "optical-flow-tvl1-final": {"reference": 120, "comparable": 31},
    # Real compute: four solves, their consistency and the splat, against the splat of known vectors.
    "flow-blur-draft": {"reference": 66, "comparable": 2.25},
    "flow-blur-standard": {"reference": 150, "comparable": 5.0},
    "flow-blur-high": {"reference": 305, "comparable": 10.0},
    "flow-blur-matte-standard": {"reference": 320, "comparable": 5.0},
}
# Cases a known limitation keeps from running: none (#549 put the 2160p Cryptomatte cases back on
# the GPU route).
NODE_COST_KNOWN_FAILURES = frozenset()
