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
- a procedural layer: {"name", "generator": [node type, {role: value}], "port": <its raster
  output, "image" by default>, "keys": {role: [[frame, value], ...]}, "transform", "blend",
  "effects": [...]} (a generator node drawn as a layer, with no media).

A footage layer may also carry mattes (#383): "cryptomattes": {name: matte list} adds a
Cryptomatte node per name reading the layer's channel set (the matte list in the common syntax),
"maskCombine": {name: [mode, first, second]} a Mask Combine of two of them, "matteMasks":
[[name, mode], ...] layer masks shaped by those Masks (mode a layer-mask mode name), and
"trackMatte": name a node track matte read from one (any earlier layer's). An effect may take a
third element {"mask": name, "maskChannel": value} connecting that Mask to its mask input.

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
# Cryptomatte shot's under #549, which tripled its fill).
MATTE_FILL_BUSY = ("#549: the Cryptomatte shot fills at 7 to 8 fps with the GPU 35 to 51% busy "
                   "(2026-11-06, about 3 fps and 10 to 18% before #549): each frame's host work is "
                   "mostly the ZIP decode of the multilayer EXR, once for the beauty and once more "
                   "for the Cryptomatte rank channels")
NOISE_FILL_BUSY = ("#386: the Noise shot fills its 24 frames in about 0.4 s (59 fps on "
                   "2026-11-06), two or three GPU utilisation samples: too short to judge the "
                   "average busy fairly (31%, peak 50%)")
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
BLURRED_COLOUR = ("bloom.gaussian-blur", "bloom.light-wrap", "bloom.glow", "bloom.drop-shadow")


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
