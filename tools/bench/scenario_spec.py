"""The real-world scenario suite's shots (issue #540), as plain data.

Each scenario is a generic composite of CG render passes built only from the synthetic benchmark
footage (see benchmarks.md, "Media"): the multilayer EXR stands in for a CG beauty render, the
EXR half and float sequences for further passes and elements, and the PNG sequences for a plate.
run_scenarios.py builds each one through MCP `transact` in a separate offscreen Bloom.

A scenario names its media tier (the composition takes that tier's size), the frames of its work
area, and its layers from the bottom up. A layer is one of:

- footage: {"name", "media": <format>, "transform": {...}, "blend": <mode>, "effects": [...],
  "lightWrap": <name of a lower layer whose footage is the wrap's background>};
- a nested Merge: {"name", "merge": [footage layers, bottom up], "transform", "blend",
  "effects": [...] (applied to the nested Merge's picture)};
- an adjustment layer: {"name", "adjustment": [effects]} (filters the stack below it);
- an audio layer (#569): {"name", "audio": {"seconds", "effects": [...]}}, a generated stereo tone
  with seeded noise, imported as an audio asset and fed through the audio effects in order before
  the layer (a scenario with "audioExport" also exports its sound as WAV, optionally normalized to
  {"loudnessLufs", "truePeakDbtp"}, #570);
- a procedural layer: {"name", "generator": [node type, {role: value}], "port": <its raster
  output, "image" by default>, "keys": {role: [[frame, value], ...]}, "transform", "blend",
  "effects": [...], "pathNode": {"keys": [[frame, path value], ...]}} (a generator node drawn as a
  layer, with no media; "pathNode" drives its `path` from a keyed Path node, #589).

A footage layer may also carry mattes (#383): "cryptomattes": {name: matte list} adds a
Cryptomatte node per name reading the layer's channel set (the matte list in the common syntax),
"maskCombine": {name: [mode, first, second]} a Mask Combine of two of them, "matteMasks":
[[name, mode], ...] layer masks shaped by those Masks (mode a layer-mask mode name), and
"trackMatte": name a node track matte read from one (any earlier layer's). An effect may take a
third element {"mask": name, "maskChannel": value} connecting that Mask to its mask input.

A footage layer may also carry depth passes (#483): "passes": {name: {"media": format,
"colorSpace": label}} adds a source node per name reading that footage (a derived pass such as
"png16_mist" included; "Raw" reads a depth pass as data), and every footage layer's own channel set
is the pass "<layer name>:channels". An effect's third element may then hold {"depth": pass name,
"depthChannel": value} connecting that pass to its depth input, and {"keys": {role: [[frame,
value], ...]}} keys its parameters (a rack focus).

An effect is [node type, {role: value}]. Values of the roles in SCALED are pixel sizes at 1920
wide and scale with the composition width (clamped to the parameter's range by the builder).
A transform's "position" is a fraction of the frame. When a new heavy node lands, add a scenario
that uses it (and keep the existing ones unchanged, so their baselines stay comparable).

"knownFailures" maps a check to the open issue that makes it fail: the check is then reported as
`known-failure` with that reference and does not fail the run. Remove an entry when its issue is
fixed (the runner says when a listed check passes).
"""

SCALED = ("radius", "size", "length", "distance", "softness")

GRADE_WARM = ["bloom.grade", {"gainRed": 1.08, "gainBlue": 0.94, "gammaMaster": 0.06,
                              "contrast": 1.12, "saturation": 1.1}]
GRADE_COOL = ["bloom.grade", {"liftBlue": 0.01, "gainRed": 0.95, "gainBlue": 1.05,
                              "exposure": -0.25}]
CURVES_S = ["bloom.curves", {"master": {"closed": False, "anchors": [
    {"point": [0, 0], "inHandle": None, "outHandle": None, "mode": "corner"},
    {"point": [0.25, 0.2], "inHandle": None, "outHandle": None, "mode": "corner"},
    {"point": [0.75, 0.82], "inHandle": None, "outHandle": None, "mode": "corner"},
    {"point": [1, 1], "inHandle": None, "outHandle": None, "mode": "corner"}]}}]
LEVELS = ["bloom.levels", {"masterInputBlack": 0.02, "masterInputWhite": 0.95,
                           "masterGamma": 1.1}]

# The open issues behind the failures measured on 2026-11-05 (benchmark-results-2026-11-05-
# scenarios.md). After the #544 fixes the final render no longer falls back to the CPU, the fills
# no longer starve on capacity waits and land in frame order, and image sequences publish frames
# side by side. The GPU-busy check moved to #386, whose batched submissions and fence waits made
# the light 2K shot pass it (64% busy on 2026-11-06); what is left of it is listed here (the
# Cryptomatte shot's under #549, which tripled its fill and then read each frame's EXR once).
MATTE_FILL_BUSY = ("#549: the Cryptomatte shot fills at 12 to 13 fps with the GPU 24 to 40% busy "
                   "(2026-11-06, 7 to 8 fps and 25 to 46% when it decoded the EXR twice): its "
                   "24 frames fill in about 2 s, ten GPU utilisation samples, and each frame's "
                   "host work is still the one ZIP decode of the multilayer EXR (beauty and "
                   "Cryptomatte ranks together) and the PNG plate")
NOISE_FILL_BUSY = ("#386: the Noise shot fills its 24 frames in about 0.4 s (59 fps on "
                   "2026-11-06), two or three GPU utilisation samples: too short to judge the "
                   "average busy fairly (31%, peak 50%)")
LENS_FLARE_FILL_BUSY = ("#386: the Lens Flare shot fills its 24 frames in 0.35 s (69 fps on "
                        "2026-11-06), too few GPU utilisation samples to judge (13 to 15%); over 240 "
                        "frames it filled at 88 fps with the GPU 48% busy: one pass a frame, the rest "
                        "host decode")
FILM_GRAIN_FILL_BUSY = ("#386: the Film Grain shot fills its 24 frames in 0.2 to 0.3 s (83 to 116 fps on "
                        "2026-11-06), too few GPU utilisation samples to judge (0 to 4%); over 240 "
                        "frames it filled at 106 fps with the GPU 28% busy: one pass a frame, the "
                        "rest host decode")
SHARPEN_FILL_BUSY = ("#405: the Sharpen shot fills its 24 frames in 0.3 s (80 fps on 2026-11-06), too "
                     "few GPU utilisation samples to judge (1 to 6%); over 240 frames it filled at "
                     "79 fps with the GPU 21% busy (peak 100%) and 11 CPU cores busy: a blur and one "
                     "pass a frame, the rest host decode")
DEPTH_FILL_BUSY = ("#386: the depth effects shot fills at 16 to 20 fps with the GPU 31 to 58% busy "
                   "(2026-11-06, after #549): its 24 frames fill in a little over a second, a few "
                   "GPU utilisation samples, and every frame first decodes three sequences (the "
                   "plate, its mist pass and the multilayer EXR) on the host")
SCENARIOS = {
    # The smallest shot that needs the whole chain admitted at once: a plate and one CG pass
    # with one large Glow (the effect-chain admission case of #539).
    "two-layer-glow-2k": {
        "description": "A PNG plate under a CG beauty pass (multilayer EXR) with one large "
                       "Physical Glow.",
        "tier": "1080p",
        "frames": 48,
        "layers": [
            {"name": "Plate", "media": "png16"},
            {"name": "CG beauty", "media": "exr_multilayer", "blend": "screen",
             "transform": {"scale": [0.8, 0.8], "position": [0.55, 0.5]},
             "effects": [["bloom.glow", {"mode": "Physical", "size": 200, "intensity": 1.5,
                                         "threshold": 0.6}]]},
        ],
    },
    # A full comp at 2K: plate, three CG passes, a nested Merge, a heavy chain of Glows
    # (Classic, Physical, Streak), Drop Shadow, Light Wrap, large blurs and grading.
    "cg-comp-2k": {
        "description": "A blurred, graded PNG plate; a CG beauty pass (multilayer EXR) with "
                       "Light Wrap, Classic Glow, Drop Shadow, Curves and Levels; a CG element "
                       "(EXR half) with Physical Glow; a highlights pass (EXR float) with Streak "
                       "Glow; a nested Merge of two passes with grading and a large blur; and an "
                       "adjustment layer grading the whole stack.",
        "tier": "1080p",
        "frames": 24,
        "layers": [
            {"name": "Plate", "media": "png16",
             "transform": {"scale": [1.1, 1.1]},
             "effects": [["bloom.gaussian-blur", {"radius": 48}], GRADE_COOL]},
            {"name": "CG beauty", "media": "exr_multilayer", "lightWrap": "Plate",
             "transform": {"scale": [0.85, 0.85], "position": [0.5, 0.52]},
             "effects": [["bloom.light-wrap", {"radius": 64, "amount": 0.8}],
                         ["bloom.glow", {"mode": "Classic", "size": 128, "intensity": 1.2,
                                         "threshold": 0.7}],
                         ["bloom.drop-shadow", {"distance": 24, "softness": 32,
                                                "opacity": 0.6}],
                         CURVES_S, LEVELS]},
            {"name": "CG element", "media": "exr_half_piz", "blend": "screen",
             "transform": {"scale": [0.5, 0.5], "rotation": 8.0, "position": [0.3, 0.35],
                           "opacity": 0.9},
             "effects": [["bloom.glow", {"mode": "Physical", "size": 160, "intensity": 1.4,
                                         "threshold": 0.5}],
                         ["bloom.drop-shadow", {"distance": 16, "softness": 24}]]},
            {"name": "Highlights", "media": "exr_float_zip", "blend": "add",
             "transform": {"scale": [0.6, 0.6], "position": [0.7, 0.4], "opacity": 0.7},
             "effects": [["bloom.glow", {"mode": "Streak", "length": 400, "points": 6,
                                         "intensity": 1.5, "threshold": 0.8, "glowOnly": True}]]},
            {"name": "Passes", "blend": "normal",
             "transform": {"scale": [0.4, 0.4], "position": [0.78, 0.72], "rotation": -6.0},
             "merge": [
                 {"name": "Pass A", "media": "exr_half_piz",
                  "effects": [GRADE_WARM]},
                 {"name": "Pass B", "media": "png8", "blend": "overlay",
                  "transform": {"scale": [0.7, 0.7], "opacity": 0.8}},
             ],
             "effects": [["bloom.gaussian-blur", {"radius": 96}],
                         ["bloom.glow", {"mode": "Classic", "size": 96, "threshold": 0.6}]]},
            {"name": "Grade", "adjustment": [GRADE_WARM, CURVES_S]},
        ],
    },
}

# #525: the Fields heavy node. A fractal Noise animated through its evolution (so no frame reuses
# another's raster), screened over the plate and lit by a Physical Glow, the glow of the noise's
# bright crests. The issue's Noise-driven Displace is not built yet (#348), so the Noise drives a
# Glow; add a Displace layer to a new scenario when it lands, keeping this one unchanged.
SCENARIOS["noise-glow-2k"] = {
    "description": "A PNG plate under a full-frame procedural Noise (Spline, Dynamic Twist, 8 "
                   "octaves, evolving every frame) screened on top with a large Physical Glow.",
    "tier": "1080p",
    "frames": 24,
    "knownFailures": {"gpu-busy-during-fill": NOISE_FILL_BUSY},
    "layers": [
        {"name": "Plate", "media": "png16"},
        {"name": "Noise", "blend": "screen",
         "generator": ["bloom.field-noise", {"type": "Spline", "fractal": "Dynamic Twist",
                                             "size": [320, 240], "octaves": 8.0,
                                             "octaveRotation": 25.0, "contrast": 1.6,
                                             "overflow": "Soft Clamp"}],
         "keys": {"evolution": [[0, 0.0], [24, 2.0]]},
         "effects": [["bloom.glow", {"mode": "Physical", "size": 160, "intensity": 1.3,
                                     "threshold": 0.55}]]},
    ],
}

# The same comp at 4K (the 2160p tier), with a shorter work area.
SCENARIOS["cg-comp-4k"] = dict(SCENARIOS["cg-comp-2k"], tier="2160p", frames=12)

# Alpha keyed from tolerance-qualified colour (#545). The reference-parity gate holds alpha exact,
# except alpha an effect derives from colour that a device route matches within the 2e-6 gate
# rather than bit for bit (gpu-backend.md, "reference-parity profile"): that alpha is held to the
# same 2e-6 tolerance. Glow's Luma Key derives its coverage from its input's colour. The device
# matches the CPU within the tolerance, not exactly, for colour through a Gaussian blur (and the
# Light Wrap, Glow and Drop Shadow recipes built on it), a Grade's gamma or contrast and a Levels
# gamma (the device `pow`, image-filters.md).
COLOUR_KEYED_ALPHA = ("bloom.glow",)
BLURRED_COLOUR = ("bloom.gaussian-blur", "bloom.light-wrap", "bloom.glow", "bloom.drop-shadow",
                  "bloom.sharpen")


def tolerance_qualified_colour(effect):
    kind, values = effect[0], effect[1]
    if kind in BLURRED_COLOUR:
        return True
    if kind == "bloom.grade":
        return values.get("contrast", 1.0) != 1.0 or any(
            value != 0.0 for role, value in values.items() if role.startswith("gamma"))
    if kind == "bloom.levels":
        return any(value != 1.0 for role, value in values.items() if role.endswith("Gamma"))
    return False


def _keyed_chain(name, effects, qualified, found):
    """Walks one effect chain whose input is tolerance-qualified colour when `qualified` names the
    effect that made it so; records each colour key over such colour and returns the source."""
    for effect in effects:
        if qualified and effect[0] in COLOUR_KEYED_ALPHA:
            found.append(f"{name}: {effect[0]} keys colour after {qualified}")
        if not qualified and tolerance_qualified_colour(effect):
            qualified = effect[0]
    return qualified


def keyed_alpha(scenario):
    """The colour keys in a scenario whose alpha comes from tolerance-qualified colour, as
    "<layer>: <key> keys colour after <effect>". Empty: every alpha sample is held exact."""
    found = []
    below = None  # what makes the stack below tolerance-qualified, for an adjustment layer
    for layer in scenario["layers"]:
        if "adjustment" in layer:
            qualified = _keyed_chain(layer["name"], layer["adjustment"], below, found)
        else:
            qualified = None
            for entry in layer.get("merge") or ():
                qualified = _keyed_chain(entry["name"], entry.get("effects") or (), None,
                                         found) or qualified
            qualified = _keyed_chain(layer["name"], layer.get("effects") or (), qualified, found)
        below = below or qualified
    return found

# The synthetic scene's object names (blender_scene.py).
CARDS = [f"Card{i}" for i in range(5)]

# #383: the matte workflow heavy node. The CG beauty pass's Cryptomatte selects the pillars and the
# cards; that selection minus the veil (Mask Combine Subtract) is the beauty layer's matte mask and
# limits its warm grade through the grade's mask input, and the cards alone matte a blurred, glowing
# highlights pass as a node track matte.
SCENARIOS["cryptomatte-mattes-2k"] = {
    "description": "A PNG plate under a CG beauty pass (multilayer EXR) shaped by its Cryptomatte: "
                   "the pillars and cards minus the veil (Mask Combine) as a matte layer mask and "
                   "as the mask input of a Grade, and the cards as the node track matte of a "
                   "blurred, glowing highlights pass (EXR float).",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16"},
        {"name": "CG beauty", "media": "exr_multilayer",
         "cryptomattes": {"Objects": ", ".join([f"Pillar{i}" for i in range(12)] + CARDS +
                                               ["Veil"]),
                          "Veil": "Veil",
                          "Cards": ", ".join(CARDS)},
         "maskCombine": {"Solid objects": ["Subtract", "Objects", "Veil"]},
         "matteMasks": [["Solid objects", "Add"]],
         "effects": [[GRADE_WARM[0], GRADE_WARM[1], {"mask": "Solid objects"}], CURVES_S]},
        {"name": "Highlights", "media": "exr_float_zip", "blend": "add",
         "trackMatte": "Cards",
         "effects": [["bloom.gaussian-blur", {"radius": 32}],
                     ["bloom.glow", {"mode": "Physical", "size": 128, "intensity": 1.2,
                                     "threshold": 0.6}]]},
    ],
    "knownFailures": {"gpu-busy-during-fill": MATTE_FILL_BUSY},
}

# #507: the Lens Flare over a plate, its intensity keyed so every frame is new, every element on at
# the largest counts and its brightness following the plate under the light (the probes' atlas path).
SCENARIOS["lens-flare-2k"] = {
    "description": "A PNG16 plate under a Lens Flare with every element on at its largest counts, "
                   "its intensity keyed and its brightness read from the plate.",
    "tier": "1080p",
    "frames": 24,
    "knownFailures": {"gpu-busy-during-fill": LENS_FLARE_FILL_BUSY},
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.lens-flare",
                      {"ghostCount": 16, "irisCount": 12, "irisBlades": 12, "streakCount": 8,
                       "anamorphicAmount": 0.5, "sourceBrightness": 0.5, "sampleRadius": 16.0},
                      {"keys": {"intensity": [[0, 0.6], [23, 1.4]]}}]]},
    ],
}

# #486: Film Grain over a plate, strong and in every channel: its frame's own pattern makes every
# frame new with no key.
SCENARIOS["film-grain-2k"] = {
    "description": "A PNG16 plate under strong Film Grain of 1.5 pixels, drawn anew for every frame.",
    "tier": "1080p",
    "frames": 24,
    "knownFailures": {"gpu-busy-during-fill": FILM_GRAIN_FILL_BUSY},
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.film-grain", {"intensity": 0.05, "size": 1.5, "blueAmount": 1.5}]]},
    ],
}

# #486: Sharpen over a plate, its amount keyed so every frame is new: each frame runs the plate's blur
# and the one pass over the two.
SCENARIOS["sharpen-2k"] = {
    "description": "A PNG16 plate under Sharpen with a radius of 8 pixels, its amount keyed.",
    "tier": "1080p",
    "frames": 24,
    "knownFailures": {"gpu-busy-during-fill": SHARPEN_FILL_BUSY},
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.sharpen", {"radius": 8.0},
                      {"keys": {"amount": [[0, 0.5], [23, 2.0]]}}]]},
    ],
}

# #564: Frame Rate Hold over a moving plate with a seeded irregular cadence. The plate moves, so every
# held picture is new work upstream of the hold, and the cadence repeats some of them.
SCENARIOS["frame-rate-hold-2k"] = {
    "description": "A moving PNG16 plate held to 12 fps with a seeded irregular cadence.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.transform-2d", {},
                      {"keys": {"translate": [[0, [-96.0, 0.0]], [23, [96.0, 24.0]]]}}],
                     ["bloom.frame-rate-hold",
                      {"rate": 12.0, "irregular": True, "amount": 0.4, "seed": 7}]]},
    ],
}

# #565: Film Grain's Sensor Noise mode over a plate: shadow-weighted grain drawn anew for every frame
# (no key needed), chroma blotches, and a fixed pattern that is the same on every frame.
SCENARIOS["film-grain-sensor-noise-2k"] = {
    "description": "A PNG16 plate under Film Grain in Sensor Noise mode, with a fixed pattern.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.film-grain",
                      {"mode": 1, "intensity": 0.05, "size": 1.5, "shadowWeighting": 1.5,
                       "chromaBlotches": 0.6, "blotchSize": 24.0, "fixedPattern": 0.4}]]},
    ],
}

# #512: Camera Shake over a plate with its motion blur on: every frame is new (the shake is a function
# of the frame), and each frame resamples the plate once per shutter sample.
SCENARIOS["camera-shake-2k"] = {
    "description": "A PNG16 plate under a Handheld Camera Shake with three octaves, auto overscan "
                   "and its motion blur on.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.camera-shake",
                      {"mode": "Handheld", "amplitudeX": 24.0, "amplitudeY": 16.0,
                       "rollAmount": 1.0, "octaves": 3, "seed": 7, "motionBlur": True}]]},
    ],
}

# #563: Codec Degrade over a plate with the default GOP, coarse blocks and its quality keyed: each frame
# renders its chain from the last keyframe (the optical flow of every step included), so this is the
# node's heavy case.
SCENARIOS["codec-degrade-2k"] = {
    "description": "A PNG16 plate under Codec Degrade with 16 px blocks and a 12-frame GOP, its "
                   "quality keyed.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.codec-degrade",
                      {"blockSize": "16 px", "gopLength": "12 frames", "persistence": 0.75},
                      {"keys": {"quality": [[0, 60.0], [23, 15.0]]}}]]},
    ],
}

# #566: Auto Exposure over a plate whose brightness flickers, so every frame measures a window of
# changed frames. Its reductions are shared between frames, so the cost per frame is one new frame's
# measurement plus the filter and the gain; a cold frame measures the whole 48-frame window.
SCENARIOS["auto-exposure-2k"] = {
    "description": "A PNG16 plate under an Exposure driven by a seeded Light Flicker, then Auto "
                   "Exposure over a 48-frame look-back.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.exposure", {},
                      {"drives": {"stops": ["bloom.light-flicker",
                                            {"rate": 6.0, "depth": 1.5, "dropoutRate": 1.0,
                                             "seed": 7}, "stops"]}}],
                     ["bloom.auto-exposure", {"lookBack": 48}, {}]]},
    ],
}

# #481: the three pixel arithmetic nodes in a chain over a plate, every frame new through a keyed
# Map Range, so the cost is the plain per-pixel passes (one grade program each) and no more.
SCENARIOS["image-math-2k"] = {
    "description": "A PNG16 plate through Image Math (Multiply), Image Map Range (keyed input range) "
                   "and Image Clamp.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.image-math", {"operation": "Multiply", "value": 1.5}, {}],
                     ["bloom.image-map-range", {"outputMax": 2.0},
                      {"keys": {"inputMax": [[0, 1.0], [23, 2.0]]}}],
                     ["bloom.image-clamp", {}, {}]]},
    ],
}

# #569, #570: the audio effect chain over ten seconds of sound, then the WAV export with and without
# loudness normalization. Written, not run: it has no recorded numbers and no budget. The sound is
# generated by the builder (a stereo tone with seeded noise), so it needs no media tier.
SCENARIOS["audio-effects-wav"] = {
    "description": "Ten seconds of generated stereo sound through Equalizer, Compressor, Auto Level, "
                   "Mono and Hiss, exported as WAV, then again normalized to -16 LUFS.",
    "tier": "720p",
    "frames": 240,
    "audioExport": {"loudnessLufs": -16.0, "truePeakDbtp": -1.0},
    "layers": [
        {"name": "Card", "generator": ["bloom.test-card", {"card": "Zone Plate"}]},
        {"name": "Voice",
         "audio": {"seconds": 10,
                   "effects": [["bloom.audio-equalizer", {"highPass": 80.0, "midGain": 4.0,
                                                          "midFrequency": 2500.0}, {}],
                               ["bloom.audio-compressor", {"threshold": -24.0, "ratio": 4.0}, {}],
                               ["bloom.audio-auto-level", {"target": -18.0}, {}],
                               ["bloom.audio-mono", {"amount": 1.0}, {}],
                               ["bloom.audio-hiss", {"level": -48.0, "seed": 7},
                                {"keys": {"level": [[0, -60.0], [239, -40.0]]}}]]}},
    ],
}

# #486: Lens Distortion in Redistort mode (the inverse solved per pixel) over a plate, its radial
# coefficient keyed so every frame is new. Only the warped image is read, so the ST map is not built.
SCENARIOS["lens-distortion-2k"] = {
    "description": "A PNG16 plate under Lens Distortion in Redistort mode, its radial k1 keyed.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.lens-distortion",
                      {"mode": "Redistort", "k2": 0.05},
                      {"keys": {"k1": [[0, -0.1], [23, -0.3]]}}]]},
    ],
}

# #486: Chromatic Aberration over a plate, its amount keyed so every frame is new.
SCENARIOS["chromatic-aberration-2k"] = {
    "description": "A PNG16 plate under Chromatic Aberration, its amount keyed.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.chromatic-aberration",
                      {},
                      {"keys": {"amount": [[0, 0.01], [23, 0.04]]}}]]},
    ],
}

# #568: Rolling Shutter over a plate shaken by a Camera Shake, whose Offset and Roll drive the shutter's
# camera sockets, so the shutter reads the shake exactly at three moments of the readout. The shake is
# a function of the frame, so every frame is new.
SCENARIOS["rolling-shutter-2k"] = {
    "description": "A PNG16 plate shaken by a Camera Shake and skewed by a Rolling Shutter that reads it.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.camera-shake", {"amplitudeX": 24.0, "amplitudeY": 16.0, "rollAmount": 1.0,
                                             "seed": 3}],
                     ["bloom.rolling-shutter", {"readout": 0.8},
                      {"drives": {"cameraOffset": ["@previous", {}, "offset"],
                                  "cameraRoll": ["@previous", {}, "roll"]}}]]},
    ],
}

# #567: Light Flicker driving an Exposure node's stops over a plate. The flicker is a function of the
# frame, so every frame is new; the cost is the Exposure's, since the flicker itself is one value per frame.
SCENARIOS["light-flicker-2k"] = {
    "description": "A PNG16 plate under an Exposure whose stops are driven by a seeded Light Flicker.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.exposure", {},
                      {"drives": {"stops": ["bloom.light-flicker",
                                            {"rate": 14.0, "depth": 0.5, "dropoutRate": 2.0,
                                             "seed": 11}, "stops"]}}]]},
    ],
}

# #587: a Test Card source drawn as a procedural layer: the Zone Plate, the card with the most samples per
# pixel (16), moving so every frame is new. No media is read: the card is drawn at the composition's size.
# Written, not run.
SCENARIOS["test-card-2k"] = {
    "description": "A moving Zone Plate Test Card drawn procedurally at the composition's size.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Card",
         "generator": ["bloom.test-card", {"card": "Zone Plate", "zoneMoving": True,
                                           "label": False}]},
    ],
}

# #508: Light Rays over a plate, its length keyed so every frame is new.
SCENARIOS["light-rays-2k"] = {
    "description": "A PNG16 plate under Light Rays from its luminance, its length keyed.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "effects": [["bloom.light-rays",
                      {"source": "Luminance"},
                      {"keys": {"length": [[0, 0.4], [23, 0.9]]}}]]},
    ],
}

# #589: a Shape Source whose path arrives through the Path socket: a Path node holding a compound value
# of 64 contours, keyed so that every frame flattens and covers a new shape, read by the shape. No media
# is read. Written, not run.
def _ring(index, shift):
    """One small closed square of the Path node's value, on a grid of the composition."""
    left, top = 40 + (index % 8) * 220 + shift, 40 + (index // 8) * 120
    return {"closed": True, "anchors": [
        {"point": point, "inHandle": None, "outHandle": None, "mode": "corner"}
        for point in ([left, top], [left + 160, top], [left + 160, top + 80], [left, top + 80])]}


SCENARIOS["path-value-compound-2k"] = {
    "description": "A Shape Source reading a keyed 64-contour path from a Path node.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Shape",
         "generator": ["bloom.shape-source", {"kind": 6}],  # ShapeKind::Path
         "pathNode": {"keys": [
             [0, {"contours": [_ring(i, 0) for i in range(64)], "fillRule": "evenOdd"}],
             [24, {"contours": [_ring(i, 40) for i in range(64)], "fillRule": "evenOdd"}]]}},
    ],
}

# #483: the depth effects heavy nodes, from synthetic CG passes in both depth encodings. A PNG plate
# is fogged and then defocused through a physical lens from its 16-bit PNG mist pass (derived from
# the render's camera depth and read as data), the focus racking from the near pillars to the back;
# over it the CG beauty pass (multilayer EXR) is fogged and defocused from its own float Z channel,
# with polygon bokeh and highlight boost. Fog comes before the defocus, as the docs recommend.
MIST = "Plate mist"
EXR_Z = "CG beauty:channels"
SCENARIOS["depth-effects-2k"] = {
    "description": "A PNG plate fogged and defocused (physical lens, racking focus) from its "
                   "16-bit PNG mist pass, under the CG beauty pass (multilayer EXR) fogged and "
                   "defocused (polygon bokeh, highlight boost) from its own float Z channel.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate", "media": "png16",
         "passes": {MIST: {"media": "png16_mist", "colorSpace": "Raw"}},
         "effects": [
             ["bloom.depth-fog", {"interpretation": 3, "falloff": "Exponential", "start": 0.05,
                                  "end": 1.0, "density": 2.0, "amount": 0.8},
              {"depth": MIST}],
             ["bloom.z-defocus", {"mode": "Physical lens", "interpretation": 3, "near": 5.0,
                                  "far": 40.0, "focalLength": 85.0, "fStop": 2.0,
                                  "maxSize": 48.0},
              {"depth": MIST, "keys": {"focusDistance": [[0, 8.0], [23, 24.0]]}}]]},
        {"name": "CG beauty", "media": "exr_multilayer",
         "effects": [
             ["bloom.depth-fog", {"interpretation": 0, "start": 10.0, "end": 40.0,
                                  "amount": 0.6},
              {"depth": EXR_Z, "depthChannel": "ViewLayer.Depth.Z"}],
             ["bloom.z-defocus", {"interpretation": 0, "focusDistance": 9.0, "maxSize": 32.0,
                                  "shape": "Polygon (blades)", "blades": 6, "highlightGain": 1.5,
                                  "highlightThreshold": 0.8},
              {"depth": EXR_Z, "depthChannel": "ViewLayer.Depth.Z"}]]},
    ],
    "knownFailures": {"gpu-busy-during-fill": DEPTH_FILL_BUSY},
}

MOTION_FILL_BUSY = ("#386: the motion blur shot fills its 24 frames in 1.3 s with the GPU 35% busy "
                    "(2026-11-06), a few samples, and decodes three sequences on the host a frame.")
# #484: every motion-blur route but the solver at work on a shot: a moving plate through Transform
# 2D's shutter and a Directional Blur; a Motion Blur over a moving Transform 2D (sub-frames re-render
# what is upstream of it) with an Echo; a CG layer blurred by Vector Motion Blur reading a vector image
# (the bench render has no vector pass, so the Raw mist pass scaled to 48 pixels stands in: a real
# per-pixel vector field, and the cost does not depend on where it came from).
MOTION_VECTORS = "CG vectors"
SCENARIOS["motion-blur-2k"] = {
    "description": "Moving plates through Transform 2D's shutter, Motion Blur sub-frames and Echo, "
                   "under a CG layer blurred by Vector Motion Blur from a per-pixel vector image.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate shutter", "media": "png16",
         "effects": [
             ["bloom.transform-2d", {"motionBlur": True, "quality": "High"},
              {"keys": {"translate": [[0, [-96.0, 0.0]], [23, [96.0, 24.0]]]}}],
             ["bloom.directional-blur", {"angle": 15.0, "length": 16.0, "quality": "High"}]]},
        {"name": "Plate subframes", "media": "png8", "blend": "screen",
         "transform": {"opacity": 0.5},
         "effects": [
             ["bloom.transform-2d", {},
              {"keys": {"translate": [[0, [96.0, 0.0]], [23, [-96.0, -24.0]]]}}],
             ["bloom.motion-blur", {"quality": "Draft"}],
             ["bloom.echo", {"echoes": 3, "decay": 0.5, "operator": "Maximum"}]]},
        {"name": "CG vectors layer", "media": "exr_multilayer", "blend": "screen",
         "transform": {"opacity": 0.5},
         "passes": {MOTION_VECTORS: {"media": "png16_mist", "colorSpace": "Raw"}},
         "effects": [
             ["bloom.vector-motion-blur", {"convention": "Custom", "vectorScale": 48.0,
                                           "algorithm": "Gather", "quality": "Standard"},
              {"inputs": {"vectors": MOTION_VECTORS}}]]},
    ],
    "knownFailures": {"gpu-busy-during-fill": MOTION_FILL_BUSY},
}

# #484: the optical-flow solver on footage, with a matte, on its own so the parity gate stays
# meaningful for the rest. Its export parity is a known failure: footage reaches the solver through a
# GPU colour conversion that is within 2e-6 of the CPU reference but not bit-identical, and an
# iterative solver amplifies that to about 1e-2 at moving edges, alpha too through the matte's layers. Parity on bit-identical input is
# exact (gpu_scene_effect_native, motion-blur.md "Parity").
MOTION_MATTE = "Plate matte"
MOTION_FLOW_PARITY = ("the flow solver amplifies the upstream's within-tolerance colour differences "
                      "(motion-blur.md, Parity); exact on bit-identical input")
SCENARIOS["motion-blur-flow-2k"] = {
    "description": "A PNG16 plate through Optical Flow Motion Blur with a matte (two layers, eight "
                   "solves), at Draft.",
    "tier": "1080p",
    "frames": 24,
    "layers": [
        {"name": "Plate flow", "media": "png16",
         "passes": {MOTION_MATTE: {"media": "png16_mist", "colorSpace": "Raw"}},
         "effects": [
             ["bloom.optical-flow-motion-blur", {"quality": "Draft"},
              {"inputs": {"matte": MOTION_MATTE}}]]},
    ],
    "knownFailures": {"export-cpu-parity": MOTION_FLOW_PARITY},
}
