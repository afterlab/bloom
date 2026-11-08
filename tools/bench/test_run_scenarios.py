"""Checks of the scenario suite runner (run_scenarios.py) that need no GPU and no media: its MCP
client against a real Streamable HTTP server (bloom_mcp_http_test_host), the playback rate, the
baseline comparison, the pass criteria and the parity gate.

    python3 test_run_scenarios.py <bloom_mcp_http_test_host>
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_scenarios as suite  # noqa: E402

HOST_BINARY = sys.argv.pop(1) if len(sys.argv) > 1 else None


class ClientAgainstServer(unittest.TestCase):
    @unittest.skipUnless(HOST_BINARY, "needs bloom_mcp_http_test_host")
    def test_initialize_call_transact_and_errors(self):
        with tempfile.TemporaryDirectory(prefix="bloom-scenario-client-") as folder:
            port = suite.free_port()
            host = subprocess.Popen([HOST_BINARY, "--port", str(port), "--folder", folder],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            try:
                self.assertTrue(host.stdout.readline().startswith("READY"))
                token = (pathlib.Path(folder) / "mcp" / "token").read_text().strip()
                client = suite.McpClient(port, token)
                self.assertTrue(client.session, "initialize returns a session id")
                revision = client.call("query", {"kind": "project"})["revision"]
                builder = suite.Builder(client, {"layers": []}, {}, 1920, 1080, folder)
                created = builder.transact("bloom.composition.add", name="Shot", width=1920,
                                           height=1080, duration=24)
                self.assertIn("composition", created, "transact outputs are read by name")
                self.assertGreater(client.call("query", {"kind": "project"})["revision"],
                                   revision)
                # #525: a procedural layer: a keyed Noise with its choices by label, through a
                # Glow, as a layer of the output Merge.
                builder.composition = created["composition"]
                holder = builder.generator(
                    {"name": "Noise", "blend": "screen",
                     "generator": ["bloom.field-noise", {"type": "Spline",
                                                         "fractal": "Dynamic Twist",
                                                         "size": [320, 240]}],
                     "keys": {"evolution": [[0, 0.0], [24, 2.0]]},
                     "effects": [["bloom.glow", {"mode": "Physical", "size": 160}]]},
                    builder.output_merge())
                nodes = {r["id"]: r for r in client.call(
                    "query", {"kind": "nodes", "composition": builder.composition})["records"]}
                self.assertIn(holder, nodes, "the procedural layer is in the graph")
                noise = builder.layers["Noise"]["source"]
                self.assertEqual(nodes[noise]["typeId"], "bloom.field-noise")
                keys = client.call("query", {"kind": "keyframes",
                                             "composition": builder.composition,
                                             "parameter": nodes[noise]["parameters"]["evolution"]})
                self.assertIn('"keyframe"', json.dumps(keys), "the evolution is keyed")
                with self.assertRaises(suite.McpError):
                    builder.transact("bloom.composition.delete", composition=999999)
                with self.assertRaises(suite.McpError):
                    client.call("no-such-tool")
                client.close()
                with self.assertRaises(suite.McpError):
                    client.call("query", {"kind": "project"})
            finally:
                host.stdin.write("quit\n")
                host.stdin.close()
                host.wait(60)
                host.stdout.close()


def sample(t, **values):
    row = {key: None for key in ("playhead", "dropped")}
    row.update(t=t, **values)
    return row


class PlaybackRate(unittest.TestCase):
    def test_unwraps_loops_and_subtracts_dropped(self):
        samples = [sample(0.0, playhead=20, dropped=0), sample(0.5, playhead=4, dropped=1),
                   sample(1.0, playhead=16, dropped=2)]
        rate = suite.playback_rate(samples, 24)
        self.assertEqual(rate["transportFps"], 20.0)  # 8 frames to the loop end, then 4, then 12
        self.assertEqual(rate["droppedFrames"], 2)
        self.assertEqual(rate["shownFps"], 18.0)

    def test_needs_two_samples(self):
        self.assertEqual(suite.playback_rate([sample(0.0, playhead=1)], 24), {})


class Comparison(unittest.TestCase):
    def result(self, fill, first, export):
        return {"scenarios": [{"name": "shot", "metrics": {
            "fillFps": fill, "firstFrameSeconds": first, "exportSecondsPerFrame": export}}]}

    def test_regressions_beyond_the_threshold_only(self):
        rows = suite.compare(self.result(9.5, 2.1, 1.0), self.result(10.0, 2.0, 1.0), 10)
        self.assertFalse(any(row["regressed"] for row in rows), "within noise")
        rows = {row["metric"]: row for row in
                suite.compare(self.result(8.0, 2.5, 1.2), self.result(10.0, 2.0, 1.0), 10)}
        self.assertTrue(rows["fillFps"]["regressed"], "slower fill")
        self.assertTrue(rows["firstFrameSeconds"]["regressed"], "slower first frame")
        self.assertTrue(rows["exportSecondsPerFrame"]["regressed"], "slower export")
        self.assertFalse(suite.compare(self.result(12.0, 1.0, 0.5),
                                       self.result(10.0, 2.0, 1.0), 10)[0]["regressed"])


def args(**overrides):
    values = {"claim_factor": 3.0, "gpu_busy_percent": 50.0, "first_frame_timeout": 120.0,
              "scrub_timeout": 60.0, "stall_seconds": 120.0, "claim_capacity_percent": 90.0,
              "claim_capacity_seconds": 60.0, "export_fill_factor": 4.0, "export_allowance": 0.5}
    values.update(overrides)
    return type("Args", (), values)


def passing_rows(frames=24):
    gib = 1 << 30
    common = {"cpuFallbackFrames": 0, "firstFrameProvider": "gpuResident",
              "firstFrameCpuFallbacks": 0, "displayedProviders": {"gpuPreparedPacked": 3},
              "peakDeviceClaimBytes": 2 * gib, "peakDeviceLiveBytes": gib,
              "peakHostClaimBytes": gib, "peakAnonBytes": gib, "tasksSeen": [],
              "firstFrameSeconds": 2.0, "highClaimSeconds": 1.0}
    fill = dict(common, kind="fill", fillTimedOut=False, fillStalled=False, fillSeconds=4.8,
                cachedFrames=frames,
                totalFrames=frames, spanFirstFrame=0, spanFrames=frames, spanStartsSeen=[0],
                maxOutOfOrderFrames=2,
                gpuBusyPercent=90.0, cachedPlayCpuFallbacks=0, peakPreviewHostBytes=gib,
                minUsableBytes=40 * gib, previewCeilingBytes=40 * gib)
    export = {"kind": "export", "exportFrames": 4, "publishedFrames": 4, "gpuFrames": 4,
              "cpuFallbackFrames": 0, "exportSecondsPerFrame": 0.9}
    parity = {"kind": "parity", "frames": [{"frame": 0, "passed": True, "maxAbsError": 0.0}],
              "passed": True}
    return [fill, dict(common, kind="playback"), export, parity]


def outcomes(rows, known=None, **overrides):
    shot = {"frames": 24, "knownFailures": known or {}}
    return {item["check"]: item["outcome"]
            for item in suite.evaluate(args(**overrides), shot, rows)}


class Checks(unittest.TestCase):
    def test_every_check_passes(self):
        self.assertEqual(set(outcomes(passing_rows()).values()), {"pass"})

    def test_cpu_fallbacks_fail(self):
        rows = passing_rows()
        rows[1]["cpuFallbackFrames"] = 1
        self.assertEqual(outcomes(rows)["no-cpu-fallback"], "fail")
        rows = passing_rows()
        rows[0]["displayedProviders"] = {"gpuNeutral": 1}
        self.assertEqual(outcomes(rows)["no-cpu-fallback"], "fail", "a CPU-evaluated scene")
        rows = passing_rows()
        rows[2].update(gpuFrames=0, cpuFallbackFrames=4)
        self.assertEqual(outcomes(rows)["export-gpu-complete"], "fail")
        self.assertEqual(outcomes(rows)["no-cpu-fallback"], "fail")

    def test_claims_fill_order_busy_and_memory(self):
        rows = passing_rows()
        rows[0]["peakDeviceClaimBytes"] = 4 * rows[0]["peakDeviceLiveBytes"]
        self.assertEqual(outcomes(rows)["claims-within-factor"], "fail")
        self.assertEqual(outcomes(rows, claim_factor=5.0)["claims-within-factor"], "pass")
        rows = passing_rows()
        rows[0].update(spanFirstFrame=3, spanStartsSeen=[3])
        self.assertEqual(outcomes(rows)["fill-contiguous-from-playhead"], "fail")
        rows = passing_rows()
        rows[0].update(fillTimedOut=True, fillStalled=True, cachedFrames=0, spanFrames=0)
        self.assertEqual(outcomes(rows)["fill-completes"], "fail", "a fill that made no progress")
        rows = passing_rows()
        rows[0]["gpuBusyPercent"] = 20.0
        self.assertEqual(outcomes(rows)["gpu-busy-during-fill"], "fail")
        rows = passing_rows()
        rows[0]["gpuBusyPercent"] = None
        self.assertEqual(outcomes(rows)["gpu-busy-during-fill"], "not-measured")
        rows = passing_rows()
        rows[0].update(cachedFrames=10, spanFrames=10)
        self.assertEqual(outcomes(rows)["ram-preview-usable-memory"], "fail",
                         "a fill that stopped far below usable memory hit a cap")
        rows = passing_rows()
        rows[0]["previewCeilingBytes"] = 1 << 30
        self.assertEqual(outcomes(rows)["ram-preview-usable-memory"], "fail", "a fixed ceiling")

    def test_export_rate_against_the_fill(self):
        rows = passing_rows()  # fill 0.2 s/frame: the limit is 4 x 0.2 + 0.5 = 1.3 s
        self.assertEqual(outcomes(rows)["export-rate"], "pass")
        rows[2]["exportSecondsPerFrame"] = 144.0
        self.assertEqual(outcomes(rows)["export-rate"], "fail", "minutes per frame")
        rows = passing_rows()
        rows[0].update(fillStalled=True, cachedFrames=1, fillSeconds=120.0)
        self.assertEqual(outcomes(rows)["export-rate"], "fail", "no completed fill to keep up with")

    def test_progress_and_claims_near_capacity(self):
        rows = passing_rows()
        rows[1]["firstFrameSeconds"] = None
        self.assertEqual(outcomes(rows)["viewer-and-fill-progress"], "fail")
        rows = passing_rows()
        rows[0]["fillStalled"] = True
        self.assertEqual(outcomes(rows)["viewer-and-fill-progress"], "fail")
        rows = passing_rows()
        rows[0]["highClaimSeconds"] = 600.0
        self.assertEqual(outcomes(rows)["claims-below-capacity"], "fail")
        claims = [{"t": t, "deviceClaim": claim, "deviceUsable": 100}
                  for t, claim in ((0, 95), (5, 96), (10, 50), (11, 99), (31, 99))]
        self.assertEqual(suite.high_claim_seconds(claims, 0.9), 20.0)

    def test_known_failures_do_not_fail_the_run(self):
        rows = passing_rows()
        rows[2]["exportSecondsPerFrame"] = 144.0
        checks = suite.evaluate(args(), {"frames": 24, "knownFailures": {
            "export-rate": "#544", "fill-completes": "#544"}}, rows)
        by_name = {item["check"]: item for item in checks}
        self.assertEqual(by_name["export-rate"]["outcome"], "known-failure")
        self.assertEqual(by_name["export-rate"]["knownFailure"], "#544")
        self.assertEqual(by_name["fill-completes"]["outcome"], "pass", "a fixed one passes")
        self.assertEqual(suite.failing(checks), [])
        rows[1]["cpuFallbackFrames"] = 2
        self.assertEqual([c["check"] for c in suite.failing(
            suite.evaluate(args(), {"frames": 24, "knownFailures": {"export-rate": "#544"}},
                           rows))], ["no-cpu-fallback"], "an unlisted failure still fails")

    def test_missing_measurements_are_not_passes(self):
        result = outcomes([{"kind": "fill", "error": "boom"}])
        self.assertNotIn("pass", result.values())


class ParityGate(unittest.TestCase):
    def test_gate(self):
        try:
            import numpy as np
            import imageio_min
        except ImportError:
            self.skipTest("numpy is not installed")
        with tempfile.TemporaryDirectory() as folder:
            base = np.full((4, 4), 0.5, dtype=np.float32)

            def write(name, red, alpha):
                path = pathlib.Path(folder) / name
                imageio_min.write_exr(path, {"R": red, "G": base, "B": base, "A": alpha})
                return path

            one = np.ones((4, 4), dtype=np.float32)
            reference = write("cpu.exr", base, one)
            close = write("close.exr", base + np.float32(1e-7), one)
            far = write("far.exr", base + np.float32(1e-4), one)
            alpha = write("alpha.exr", base, one * np.float32(0.999))
            self.assertTrue(suite.parity(close, reference)["passed"])
            self.assertEqual(suite.parity(far, reference)["failingComponents"], 16)
            self.assertEqual(suite.parity(alpha, reference)["alphaMismatches"], 16)
            # #545: alpha keyed from tolerance-qualified colour is held to the same 2e-6, and its
            # largest difference is reported; every other alpha stays exact.
            ulp = write("ulp.exr", base, one * np.float32(0.9999999))
            exact = suite.parity(ulp, reference)
            self.assertFalse(exact["passed"])
            self.assertEqual(exact["alphaAboveGate"], 16)
            keyed = suite.parity(ulp, reference, keyed_alpha=True)
            self.assertTrue(keyed["passed"])
            self.assertEqual((keyed["alphaMismatches"], keyed["alphaAboveGate"]), (16, 0))
            self.assertAlmostEqual(keyed["maxAlphaError"], 1.0 - float(np.float32(0.9999999)))
            self.assertEqual(keyed["alphaGate"], "2e-6")
            beyond = suite.parity(alpha, reference, keyed_alpha=True)
            self.assertFalse(beyond["passed"])
            self.assertEqual(beyond["alphaAboveGate"], 16)


class KeyedAlpha(unittest.TestCase):
    def test_only_keys_of_tolerance_qualified_colour(self):
        import scenario_spec as spec  # noqa: PLC0415
        # The Physical Glow keys decoded footage, which both routes produce bit for bit.
        self.assertEqual(spec.keyed_alpha(spec.SCENARIOS["two-layer-glow-2k"]), [])
        for name in ("cg-comp-2k", "cg-comp-4k"):
            self.assertEqual(spec.keyed_alpha(spec.SCENARIOS[name]), [
                "CG beauty: bloom.glow keys colour after bloom.light-wrap",
                "Passes: bloom.glow keys colour after bloom.grade"])
        self.assertEqual(spec.keyed_alpha(spec.SCENARIOS["cryptomatte-mattes-2k"]), [
            "Highlights: bloom.glow keys colour after bloom.gaussian-blur"])
        glow = ["bloom.glow", {"threshold": 0.5}]
        layer = {"name": "L", "media": "png8"}
        self.assertEqual(spec.keyed_alpha({"layers": [dict(layer, effects=[
            ["bloom.grade", {"gainRed": 1.1}], glow, ["bloom.gaussian-blur", {}]])]}), [])
        self.assertEqual(spec.keyed_alpha({"layers": [dict(layer, effects=[
            ["bloom.levels", {"masterGamma": 1.2}], glow])]}),
            ["L: bloom.glow keys colour after bloom.levels"])
        self.assertEqual(spec.keyed_alpha({"layers": [
            dict(layer, effects=[["bloom.gaussian-blur", {}]]),
            {"name": "A", "adjustment": [glow]}]}),
            ["A: bloom.glow keys colour after bloom.gaussian-blur"])

    def test_check_reports_the_alpha_gate(self):
        rows = passing_rows()
        rows[3]["frames"][0].update(maxAlphaError=5.9e-8, alphaMismatches=5, alphaAboveGate=0)
        shot = {"frames": 24, "knownFailures": {}, "keyedAlpha": ["L: bloom.glow keys colour"]}
        item = next(c for c in suite.evaluate(args(), shot, rows)
                    if c["check"] == "export-cpu-parity")
        self.assertEqual(item["outcome"], "pass")
        self.assertIn("max alpha difference 5.9e-08", item["detail"]["message"])
        self.assertIn("within 2e-6 (L: bloom.glow keys colour)", item["detail"]["gate"])


class NodeCost(unittest.TestCase):
    """#383: per-node GPU cost ratios, their budgets and the baseline comparison."""

    @staticmethod
    def case(name, ms, reference="grade", comparable="", tier="1080p", ok=True):
        return {"case": name, "node": name, "tier": tier, "parameter": "", "value": 0,
                "reference": reference, "comparable": comparable, "ok": ok, "error": "",
                "coldMs": ms, "warmMs": 0.01, "ms": ms, "dispatches": 4, "warmDispatches": 0}

    def cases(self, image_to_mask_ms=5.0):
        return [self.case("grade", 4.0), self.case("luma-key-mix", 4.5),
                self.case("image-to-mask-mix", image_to_mask_ms, comparable="luma-key-mix")]

    def test_ratios_against_reference_and_comparable(self):
        import node_cost  # noqa: PLC0415
        rows = {r["case"]: r for r in node_cost.with_ratios(self.cases())}
        self.assertEqual(rows["image-to-mask-mix"]["referenceRatio"], 1.25)
        self.assertEqual(rows["image-to-mask-mix"]["comparableRatio"], 1.111)
        self.assertIsNone(rows["grade"]["referenceRatio"])
        self.assertEqual(node_cost.violations(self.cases()), [])

    def test_over_budget_and_unrun_cases_are_flagged(self):
        import node_cost  # noqa: PLC0415
        found = node_cost.violations(self.cases(image_to_mask_ms=40.0))
        self.assertEqual([f["case"] for f in found], ["1080p image-to-mask-mix"] * 2)
        self.assertIn("the reference grade", found[0]["reason"])
        unrun = self.cases() + [self.case("matte-x", 0, ok=False, comparable="grade")]
        self.assertEqual(node_cost.violations(unrun)[0]["case"], "1080p matte-x")
        # #549: Cryptomatte runs at 2160p, so an unrun 2160p Cryptomatte case is a violation.
        unrun_2160p = self.cases() + [self.case("cryptomatte-image-16", 0, tier="2160p", ok=False)]
        self.assertEqual(node_cost.violations(unrun_2160p)[0]["case"],
                         "2160p cryptomatte-image-16")

    def test_compare_flags_growth_over_the_threshold(self):
        import node_cost  # noqa: PLC0415
        baseline = {"cases": self.cases()}
        current = {"cases": self.cases(image_to_mask_ms=6.0)}
        comparison = node_cost.compare(current, baseline, 10.0)
        self.assertEqual({r["metric"] for r in comparison["regressions"]},
                         {"ms", "referenceRatio", "comparableRatio"})
        self.assertEqual(comparison["budgetViolations"], [])
        self.assertEqual(node_cost.compare(baseline, baseline, 10.0)["regressions"], [])


class DepthScenario(unittest.TestCase):
    """#483: the depth effects scenario, its derived mist pass and its depth inputs."""

    @staticmethod
    def read_png16(path):
        import struct  # noqa: PLC0415
        import zlib  # noqa: PLC0415

        import numpy as np  # noqa: PLC0415
        data = pathlib.Path(path).read_bytes()
        width, height = struct.unpack(">II", data[16:24])
        position, idat = 8, b""
        while position < len(data):
            length = struct.unpack(">I", data[position:position + 4])[0]
            if data[position + 4:position + 8] == b"IDAT":
                idat += data[position + 8:position + 8 + length]
            position += 12 + length
        rows = np.frombuffer(zlib.decompress(idat), dtype=np.uint8).reshape(height, -1)
        filtered = rows[:, 1:].astype(np.int64)
        assert (rows[:, 0] == 1).all(), "the writer uses the Sub filter on every row"
        for x in range(8, filtered.shape[1]):
            filtered[:, x] = (filtered[:, x] + filtered[:, x - 8]) % 256
        raw = filtered.astype(np.uint8).tobytes()
        return np.frombuffer(raw, dtype=">u2").reshape(height, width, 4)

    def test_mist_pass_is_derived_from_the_render_depth(self):
        import bench_media  # noqa: PLC0415
        import imageio_min  # noqa: PLC0415
        import numpy as np  # noqa: PLC0415
        spec = suite.bench.spec
        recipe = spec.DERIVED_PASSES["png16_mist"]
        with tempfile.TemporaryDirectory(prefix="bloom-mist-") as folder:
            root = pathlib.Path(folder)
            source = root / spec.sequence_path("720p", recipe["source"]).replace("####", "0001")
            source.parent.mkdir(parents=True)
            start, depth = recipe["start"], recipe["depth"]
            z = np.array([[start, start + depth / 2, start + depth, 1.0e10]], dtype=np.float32)
            imageio_min.write_exr(source, {recipe["channel"]: z,
                                           "ViewLayer.Combined.R": np.zeros_like(z)})
            found, entry = bench_media.derive_pass(root, "720p", "png16_mist", 1)
            self.assertEqual((entry["type"], entry["frames"], entry["kind"]),
                             ("sequence", 1, "derived"))
            png = found / "bench_720p_mist.0001.png"
            self.assertEqual(imageio_min.read_png_header(png)["bitDepth"], 16)
            pixels = self.read_png16(png)
            self.assertEqual(pixels[0, :, 0].tolist(), [0, 32768, 65535, 65535],
                             "mist runs 0 at the start to 1 at start + depth; empty depth is 1")
            self.assertTrue((pixels[0, :, 0] == pixels[0, :, 2]).all() and
                            (pixels[0, :, 3] == 65535).all(), "grey and opaque")
            self.assertNotIn(b"sRGB", png.read_bytes()[:200], "the pass is untagged data")
            stamp = png.stat().st_mtime_ns
            bench_media.derive_pass(root, "720p", "png16_mist", 1)
            self.assertEqual(png.stat().st_mtime_ns, stamp, "an existing frame is kept")

    def test_scenario_reads_both_depth_encodings_and_fogs_first(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["depth-effects-2k"]
        self.assertEqual(suite.scenario_formats(scenario),
                         ["exr_multilayer", "png16", "png16_mist"])
        passes = {f"{layer['name']}:channels" for layer in scenario["layers"]}
        passes.update(name for layer in scenario["layers"]
                      for name in (layer.get("passes") or {}))
        for layer in scenario["layers"]:
            kinds = [effect[0] for effect in layer["effects"]]
            self.assertLess(kinds.index("bloom.depth-fog"), kinds.index("bloom.z-defocus"),
                            "fog before defocus")
            for effect in layer["effects"]:
                self.assertIn(effect[2]["depth"], passes, "each depth input names a pass")
        self.assertEqual(spec.keyed_alpha(scenario), [])

    def test_lens_flare_scenario_keys_a_plate_flare(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["lens-flare-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.lens-flare")
        self.assertIn("intensity", effect[2]["keys"], "every frame is new")
        self.assertGreater(effect[1]["sourceBrightness"], 0.0, "the plate is probed")

    def test_film_grain_scenario_draws_new_grain_on_a_plate(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["film-grain-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.film-grain")
        self.assertNotIn("animation", effect[1], "every frame draws new grain")

    def test_sensor_noise_scenario_draws_every_term_on_a_plate(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["film-grain-sensor-noise-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.film-grain")
        self.assertEqual(effect[1]["mode"], 1)
        self.assertGreater(effect[1]["fixedPattern"], 0.0)
        self.assertNotIn("animation", effect[1], "every frame draws new grain")

    def test_frame_rate_hold_scenario_holds_a_moving_plate_with_a_cadence(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["frame-rate-hold-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effects = scenario["layers"][0]["effects"]
        self.assertEqual([effect[0] for effect in effects],
                         ["bloom.transform-2d", "bloom.frame-rate-hold"])
        self.assertTrue(effects[1][1]["irregular"])
        self.assertIn("translate", effects[0][2]["keys"])

    def test_codec_degrade_scenario_keys_its_quality_over_a_gop(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["codec-degrade-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.codec-degrade")
        self.assertEqual(effect[1]["gopLength"], "12 frames")
        self.assertIn("quality", effect[2]["keys"])

    def test_lens_distortion_scenario_keys_its_radial_coefficient(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["lens-distortion-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.lens-distortion")
        self.assertEqual(effect[1]["mode"], "Redistort")
        self.assertIn("k1", effect[2]["keys"])

    def test_chromatic_aberration_scenario_keys_its_amount(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["chromatic-aberration-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.chromatic-aberration")
        self.assertIn("amount", effect[2]["keys"])

    def test_rolling_shutter_scenario_reads_a_camera_shake(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["rolling-shutter-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effects = scenario["layers"][0]["effects"]
        self.assertEqual([effect[0] for effect in effects],
                         ["bloom.camera-shake", "bloom.rolling-shutter"])
        drives = effects[1][2]["drives"]
        self.assertEqual(drives["cameraOffset"][0], "@previous")
        self.assertEqual(drives["cameraRoll"][2], "roll")

    def test_light_flicker_scenario_drives_an_exposure(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["light-flicker-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.exposure")
        driver = effect[2]["drives"]["stops"]
        self.assertEqual(driver[0], "bloom.light-flicker")
        self.assertEqual(driver[2], "stops")

    def test_test_card_scenario_draws_a_moving_zone_plate_with_no_media(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["test-card-2k"]
        self.assertEqual(suite.scenario_formats(scenario), [])
        generator = scenario["layers"][0]["generator"]
        self.assertEqual(generator[0], "bloom.test-card")
        self.assertEqual(generator[1]["card"], "Zone Plate")
        self.assertTrue(generator[1]["zoneMoving"])

    def test_light_rays_scenario_keys_its_length(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["light-rays-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.light-rays")
        self.assertEqual(effect[1]["source"], "Luminance")
        self.assertIn("length", effect[2]["keys"])

    def test_path_value_scenario_keys_a_compound_path_into_a_shape(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["path-value-compound-2k"]
        self.assertEqual(suite.scenario_formats(scenario), [])
        layer = scenario["layers"][0]
        self.assertEqual(layer["generator"][0], "bloom.shape-source")
        keys = layer["pathNode"]["keys"]
        self.assertEqual([frame for frame, _ in keys], [0, 24])
        self.assertEqual(len(keys[0][1]["contours"]), 64)
        self.assertNotEqual(keys[0][1], keys[1][1])

    def test_auto_exposure_scenario_measures_a_flickering_plate(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["auto-exposure-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effects = scenario["layers"][0]["effects"]
        self.assertEqual([effect[0] for effect in effects],
                         ["bloom.exposure", "bloom.auto-exposure"])
        self.assertIn("stops", effects[0][2]["drives"], "the plate's brightness changes every frame")
        self.assertEqual(effects[1][1]["lookBack"], 48)

    def test_audio_effects_scenario_chains_every_effect_and_exports_wav(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["audio-effects-wav"]
        self.assertEqual(suite.scenario_formats(scenario), [])
        voice = next(layer for layer in scenario["layers"] if "audio" in layer)
        self.assertEqual([effect[0] for effect in voice["audio"]["effects"]],
                         ["bloom.audio-equalizer", "bloom.audio-compressor",
                          "bloom.audio-auto-level", "bloom.audio-mono", "bloom.audio-hiss"])
        self.assertEqual(scenario["audioExport"]["loudnessLufs"], -16.0)

    def test_camera_shake_scenario_turns_its_motion_blur_on(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["camera-shake-2k"]
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.camera-shake")
        self.assertTrue(effect[1]["motionBlur"])

    def test_motion_blur_scenarios_exercise_every_route(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenarios = [spec.SCENARIOS["motion-blur-2k"], spec.SCENARIOS["motion-blur-flow-2k"]]
        layers = [layer for scenario in scenarios for layer in scenario["layers"]]
        kinds = {effect[0] for layer in layers for effect in layer["effects"]}
        self.assertLessEqual({"bloom.transform-2d", "bloom.directional-blur", "bloom.motion-blur",
                              "bloom.echo", "bloom.vector-motion-blur",
                              "bloom.optical-flow-motion-blur"}, kinds)
        passes = {name for layer in layers for name in (layer.get("passes") or {})}
        wired = [name for layer in layers for effect in layer["effects"]
                 if len(effect) > 2 for name in effect[2].get("inputs", {}).values()]
        self.assertEqual(sorted(wired), sorted(passes), "every pass feeds one extra input")
        self.assertEqual(suite.scenario_formats(scenarios[0]),
                         ["exr_multilayer", "png16", "png16_mist", "png8"])
        self.assertNotIn("export-cpu-parity", scenarios[0]["knownFailures"],
                         "only the solver's parity is a known parity failure")

    def test_sharpen_scenario_keys_its_amount_on_a_plate(self):
        import scenario_spec as spec  # noqa: PLC0415
        scenario = spec.SCENARIOS["sharpen-2k"]
        self.assertEqual(suite.scenario_formats(scenario), ["png16"])
        effect = scenario["layers"][0]["effects"][0]
        self.assertEqual(effect[0], "bloom.sharpen")
        self.assertIn("amount", effect[2]["keys"], "every frame is new")


if __name__ == "__main__":
    unittest.main()
