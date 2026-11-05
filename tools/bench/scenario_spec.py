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
- an adjustment layer: {"name", "adjustment": [effects]} (filters the stack below it).

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
# scenarios.md), after the #544 fixes: the final render no longer falls back to the CPU and the
# fills no longer starve on capacity waits; these checks are what is left of #544.
EXPORT_RATE = ("#544: a four-frame export of a 2K shot that fills fast is slower per frame than "
               "4 x the fill plus 0.5 s (pipeline latency, serial EXR encode and verify)")
SHORT_FILL_BUSY = "#544: the light 2K shot's 3 s fill keeps the GPU under half busy on average"
FILL_ORDER = ("#544: the 4K comp's fill still waits for device capacity a few times and caches "
              "some frames ahead of the contiguous run")
ALPHA_PARITY = "#545: GPU export frames differ from the CPU reference in alpha"

SCENARIOS = {
    # The smallest shot that needs the whole chain admitted at once: a plate and one CG pass
    # with one large Glow (the effect-chain admission case of #539).
    "two-layer-glow-2k": {
        "description": "A PNG plate under a CG beauty pass (multilayer EXR) with one large "
                       "Physical Glow.",
        "tier": "1080p",
        "frames": 48,
        "knownFailures": {"export-rate": EXPORT_RATE, "export-cpu-parity": ALPHA_PARITY,
                          "gpu-busy-during-fill": SHORT_FILL_BUSY},
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
        "knownFailures": {"export-rate": EXPORT_RATE, "export-cpu-parity": ALPHA_PARITY},
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

# The same comp at 4K (the 2160p tier), with a shorter work area.
SCENARIOS["cg-comp-4k"] = dict(SCENARIOS["cg-comp-2k"], tier="2160p", frames=12, knownFailures={
    "export-cpu-parity": ALPHA_PARITY, "fill-contiguous-from-playhead": FILL_ORDER})
