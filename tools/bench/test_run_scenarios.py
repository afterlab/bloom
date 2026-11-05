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


if __name__ == "__main__":
    unittest.main()
