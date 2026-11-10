#!/usr/bin/env python3
"""Bloom real-world scenario suite over MCP (issue #540).

Each scenario (scenario_spec.py) is a heavy comp shot built from the synthetic benchmark footage.
The runner drives a separate, offscreen Bloom through the public MCP interface only: its own user
folders (preferences, MCP token, caches), the in-app MCP server on a free loopback port, no
display. It never touches the user's running Bloom or project.

Per scenario, each measurement in a fresh Bloom process:

- build: the shot through `transact`, saved with `project.save-as`;
- fill: footage dropped from the OS page cache, `project.open`, the first Current Viewer frame,
  then `transport ram-preview` (the cold RAM preview fill) and the cached playback after it;
- playback: cold `transport play` from the start, no fill;
- scrub: `transport seek` to frames spread over the work area, each timed until the Viewer shows it
  Current;
- export: `render` of the work area to a float OpenEXR sequence (the final render route), its
  provenance from `export.status`, and a few frames compared with CPU reference renders of the same
  project (`bloom-cli render` with no Vulkan driver) under the documented parity gate.

`profiler.snapshot` is read throughout. The pass criteria and their stated limits are in
benchmarks.md ("Scenario suite"). Results go to <work-dir>/results/<timestamp>-scenarios[-label]
.json/.md/.log; `--baseline` compares fill fps, first-frame time and export s/frame against an
earlier result and fails a regression beyond the threshold.

    run_scenarios.py run [--scenario NAME ...] [--build-dir B] [--media-root R] [--work-dir W]
                         [--baseline old.json] [--threshold 10] [--label L]
    run_scenarios.py compare new.json old.json [--threshold 10]
    run_scenarios.py list
"""
import argparse
import datetime
import http.client
import json
import os
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench_media  # noqa: E402
import run_bench as bench  # noqa: E402
import scenario_spec  # noqa: E402

PROTOCOL = "2025-11-25"
EXR_PRESET = "FlatExrRgba32fLinRec709SceneV1"
# GPU scene evaluation: the resident arm and the GPU-prepared packed arm. GpuNeutral is a GPU
# display transform over a CPU-evaluated scene, so it counts as a CPU frame here.
GPU_PROVIDERS = ("gpuResident", "gpuPreparedPacked")
# metric: True when higher is better
COMPARED = {"fillFps": True, "firstFrameSeconds": False, "exportSecondsPerFrame": False}
BLENDS = {"normal": 0, "add": 1, "multiply": 2, "screen": 3, "overlay": 4, "darken": 5,
          "lighten": 6}
MASK_MODES = {"None": 0, "Add": 1, "Subtract": 2, "Intersect": 3, "Difference": 4}

# --- MCP over Streamable HTTP --------------------------------------------------------------------


class McpError(RuntimeError):
    pass


class McpClient:
    """One MCP session on the in-app server (POST /mcp, JSON or an SSE answer)."""

    def __init__(self, port, token, name="bloom-scenarios"):
        self.port = port
        self.token = token
        self.session = None
        self.next_id = 0
        self.lock = threading.Lock()
        answer = self.request("initialize", {
            "protocolVersion": PROTOCOL, "capabilities": {},
            "clientInfo": {"name": name, "version": "1"}})
        self.server = answer.get("serverInfo", {})
        self.post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def post(self, message, timeout=60):
        headers = {"Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream",
                   "Authorization": "Bearer " + self.token}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        try:
            connection.request("POST", "/mcp", json.dumps(message), headers)
            response = connection.getresponse()
            # The whole answer within `timeout`, also when progress events keep the stream busy.
            deadline = time.monotonic() + timeout
            chunks = []
            while True:
                chunk = response.read1(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                if time.monotonic() > deadline:
                    raise McpError(f"no answer within {timeout:g} s")
            body = b"".join(chunks).decode("utf-8", "replace")
            self.session = response.getheader("Mcp-Session-Id") or self.session
            kind = response.getheader("Content-Type") or ""
        finally:
            connection.close()
        if response.status >= 400:
            raise McpError(f"HTTP {response.status}: {body[:300]}")
        if not body:
            return None
        if kind.startswith("text/event-stream"):
            found = None
            for line in body.splitlines():
                if line.startswith("data:"):
                    data = json.loads(line[5:].strip())
                    if "id" in data:
                        found = data
            return found
        return json.loads(body)

    def request(self, method, params, timeout=60):
        with self.lock:
            self.next_id += 1
            ident = self.next_id
        answer = self.post({"jsonrpc": "2.0", "id": ident, "method": method, "params": params},
                           timeout)
        if answer is None or "error" in answer:
            raise McpError(f"{method}: {answer and answer.get('error')}")
        return answer["result"]

    def call(self, name, arguments=None, timeout=120, check=True, progress=False):
        params = {"name": name, "arguments": arguments or {}}
        if progress:
            # A task tool with a progress token answers as an event stream, so a long render
            # keeps its connection busy with progress events until the result.
            params["_meta"] = {"progressToken": self.next_id + 1}
        result = self.request("tools/call", params, timeout)
        content = result.get("structuredContent") or {}
        if check and (result.get("isError") or content.get("succeeded") is False):
            raise McpError(f"{name} failed: {json.dumps(content)[:1500]}")
        return content

    def close(self):
        if not self.session:
            return
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            connection.request("DELETE", "/mcp", headers={
                "Authorization": "Bearer " + self.token, "Mcp-Session-Id": self.session})
            connection.getresponse().read()
        except OSError:
            pass
        finally:
            connection.close()

# --- a separate offscreen Bloom ------------------------------------------------------------------


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def isolated_env(home, driver_cache):
    """No display, no session bus (no desktop notifications), and its own user folders."""
    env = {key: value for key, value in os.environ.items()
           if key not in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS")}
    env.update(bench.OFFSCREEN_ENV)
    env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / "config"),
               XDG_DATA_HOME=str(home / "data"), XDG_CACHE_HOME=str(home / "cache"),
               XDG_STATE_HOME=str(home / "state"), XDG_RUNTIME_DIR=str(home / "runtime"),
               # The driver's own shader cache is shared across the suite's instances, as it is
               # across an artist's sessions; Bloom's caches are not.
               __GL_SHADER_DISK_CACHE_PATH=str(driver_cache),
               MESA_SHADER_CACHE_DIR=str(driver_cache))
    return env


class Instance:
    """One Bloom process with the in-app MCP server, in a scratch home deleted when it stops."""

    QUIET = {"sounds": False, "eventSounds": False, "interactionSounds": False,
             "renderNotifications": False, "audioPlayback": False}

    def __init__(self, binary, scratch, driver_cache, log):
        self.binary = binary
        self.home = Path(tempfile.mkdtemp(prefix="instance-", dir=scratch))
        self.driver_cache = driver_cache
        self.log = log
        self.process = None
        self.client = None
        self.port = None

    def start(self, timeout=120):
        for folder in ("config/Kinetik", "data", "cache", "state", "runtime"):
            (self.home / folder).mkdir(parents=True, exist_ok=True)
        os.chmod(self.home / "runtime", 0o700)
        self.port = free_port()
        (self.home / "config/Kinetik/Bloom.conf").write_text(
            f"[mcp]\nport={self.port}\nautoStart=true\n")
        self.output = open(self.home / "bloom.log", "w", encoding="utf-8")
        self.process = subprocess.Popen([str(self.binary)], stdout=self.output,
                                        stderr=subprocess.STDOUT,
                                        env=isolated_env(self.home, self.driver_cache))
        self.started = time.monotonic()
        deadline = self.started + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"Bloom exited with {self.process.returncode} at start")
            tokens = list((self.home / "data").rglob("mcp/token"))
            if tokens:
                try:
                    self.client = McpClient(self.port, tokens[0].read_text().strip())
                    break
                except (OSError, McpError):
                    pass
            time.sleep(0.2)
        if self.client is None:
            raise RuntimeError("the in-app MCP server did not answer")
        self.client.call("preferences.set", {"preferences": self.QUIET})
        return self

    def connect(self):
        """A second session, for a sampler thread."""
        return McpClient(self.port, self.client.token, "bloom-scenarios-sampler")

    def stop(self):
        if self.client:
            self.client.close()
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(60)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.process:
            self.output.close()
            text = (self.home / "bloom.log").read_text(errors="replace")
            self.log.write(f"--- Bloom output (exit {self.process.returncode}) ---\n")
            self.log.write(text[-20000:] + "\n")
            self.log.flush()
        shutil.rmtree(self.home, ignore_errors=True)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

# --- the profiler stream -------------------------------------------------------------------------


def compact(snapshot, at):
    """The figures the checks read, from one profiler.snapshot."""
    memory, gpu = snapshot["memory"], snapshot["gpu"]
    workload, viewer = snapshot["workload"], snapshot.get("viewer") or {}
    pool = next((p for p in memory.get("pools") or () if p["name"] == "Preview frames"), {})
    return {
        "t": at, "serial": snapshot["serial"],
        "cached": workload.get("ramPreviewCachedFrames"),
        "total": workload.get("ramPreviewTotalFrames"),
        "caching": workload.get("ramPreviewCaching"),
        "spanFirst": workload.get("ramPreviewSpanFirstFrame"),
        "spanFrames": workload.get("ramPreviewSpanFrames"),
        "fillFps": workload.get("ramPreviewFillFps"),
        "playbackFps": workload.get("playbackFps"),
        "dropped": workload.get("droppedFrames"),
        "exportFps": workload.get("exportFps"),
        "freshness": viewer.get("freshness"), "provider": viewer.get("provider"),
        "frame": viewer.get("frame"), "playhead": viewer.get("playheadFrame"),
        "cpuFallbacks": viewer.get("cpuFallbacks"),
        "cpuFallbacksByReason": viewer.get("cpuFallbacksByReason"),
        "gpuAdmissionRefusals": viewer.get("gpuAdmissionRefusals"),
        "gpuCompletions": viewer.get("gpuCompletions"),
        "capacityWaits": viewer.get("capacityWaits"),
        "deviceClaim": gpu.get("admissionClaimBytes"), "deviceUsable": gpu.get("usableBytes"),
        "deviceLive": gpu.get("liveBytes"), "heapUsage": gpu.get("heapUsageBytes"),
        "hostClaim": memory.get("admissionClaimBytes"), "anon": memory.get("anonBytes"),
        "rss": memory.get("rssBytes"), "usable": memory.get("usableBytes"),
        "previewHost": pool.get("hostBytes"), "previewDevice": pool.get("deviceBytes"),
        "previewCeiling": pool.get("ceilingBytes"),
        "tasks": [(task["name"], task["executor"], task["state"])
                  for task in snapshot["workload"].get("tasks") or ()],
    }


class Watcher:
    """Reads profiler.snapshot on its own MCP session every `interval` seconds."""

    def __init__(self, instance, interval=0.25):
        self.client = instance.connect()
        self.interval = interval
        self.samples = []
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self.loop, daemon=True)

    def loop(self):
        while not self.stopping.is_set():
            try:
                snapshot = self.client.call("profiler.snapshot")["snapshot"]
                self.samples.append(compact(snapshot, time.monotonic()))
            except (OSError, McpError, KeyError):
                pass
            self.stopping.wait(self.interval)

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.stopping.set()
        self.thread.join()
        self.client.close()
        return self.samples


def snapshot(client):
    return compact(client.call("profiler.snapshot")["snapshot"], time.monotonic())


def wait_for(client, predicate, timeout, interval=0.05):
    deadline = time.monotonic() + timeout
    while True:
        sample = snapshot(client)
        if predicate(sample):
            return sample
        if time.monotonic() >= deadline:
            return None
        time.sleep(interval)

# --- building a scenario -------------------------------------------------------------------------


class Builder:
    def __init__(self, client, scenario, media, width, height, directory):
        self.client = client
        self.scenario = scenario
        self.media = media  # format -> (folder, manifest entry)
        self.width, self.height = width, height
        self.unit = width / 1920.0
        self.directory = directory
        self.assets = {}
        self.ranges = {}
        self.inputs = {}
        self.layers = {}  # layer name -> {"source": node, "output": node}
        self.mattes = {}  # matte name -> the node whose `mask` output it is (#383)
        self.passes = {}  # pass name -> (source node, port) a depth input reads (#483)

    def transact(self, op, **args):
        revision = self.client.call("query", {"kind": "project"})["revision"]
        result = self.client.call("transact", {"expectedRevision": revision, "label": op,
                                               "operations": [{"op": op, "args": args}]})
        if result.get("compileFailures"):
            raise RuntimeError(f"{op} left the shot unable to render: {result['compileFailures']}")
        return {entry["name"]: entry["id"] for entry in result.get("outputs") or ()}

    def parameter_ranges(self, node_type):
        if node_type not in self.ranges:
            record = self.client.call("query", {"kind": "node-types",
                                                "nodeType": node_type})["record"]
            self.ranges[node_type] = {p["role"]: p["metadata"] for p in record["parameters"]}
        return self.ranges[node_type]

    def set_values(self, node, node_type, values):
        metadata = self.parameter_ranges(node_type)
        for role, value in values.items():
            kind = metadata[role]["valueKind"]
            if role in scenario_spec.SCALED:
                maximum = metadata[role].get("maximum")
                scale = (lambda v: v * self.unit if maximum is None
                         else min(v * self.unit, maximum))
                value = [scale(v) for v in value] if isinstance(value, list) else scale(value)
            if kind == "integer" and isinstance(value, str):
                labels = {c["label"].lower(): c["value"] for c in metadata[role]["choices"]}
                value = labels[value.lower()]
            argument = {"integer": "integer", "scalar": "number", "boolean": "boolean",
                        "string": "text"}.get(kind, "vector")
            if kind in ("path", "gradient", "color"):
                self.transact("bloom.parameter.set-source", composition=self.composition,
                              parameter=self.parameter_id(node, role), value=value)
            else:
                self.transact("bloom.node.set-parameter-value", composition=self.composition,
                              node=node, role=role, **{argument: value})

    def set_keys(self, node, keys):
        """Keys `node`'s parameters: {role: [[frame, value], ...]}. A role whose schema cannot be
        animated but that is an operand socket (most value-graph operands: a path step's amount, a
        plot's range) is driven instead by a keyed value node of its kind, which is how an artist
        animates it; a role that is neither is an error, never a silently static value."""
        if not keys:
            return
        node_type = self.node_record(node)["typeId"]
        metadata = self.parameter_ranges(node_type)
        for role, frames in keys.items():
            if role not in metadata:
                raise RuntimeError(f"{node_type} has no parameter {role}")
            if not metadata[role].get("animatable"):
                self.drive_keys(node, node_type, role, metadata[role]["valueKind"], frames)
                continue
            parameter = self.parameter_id(node, role)
            for frame, value in frames:
                self.transact("bloom.animation.set-keyframe-at-time-for-parameter",
                              composition=self.composition, parameter=parameter, time=frame,
                              value=value)

    VALUE_NODES = {"scalar": "bloom.value-scalar", "vector2": "bloom.value-vector2",
                   "integer": "bloom.value-integer"}

    def drive_keys(self, node, node_type, role, kind, frames):
        """Drives `role` of `node` from a new value node keyed with `frames`."""
        if role not in self.input_ports(node_type) or kind not in self.VALUE_NODES:
            raise RuntimeError(f"{node_type}.{role} can be neither keyed nor driven")
        driver = self.transact("bloom.node.add", composition=self.composition,
                               nodeType=self.VALUE_NODES[kind])["node"]
        self.set_keys(driver, {"value": frames})
        self.transact("bloom.node.connect", composition=self.composition, sourceNode=driver,
                      sourcePort="value", targetNode=node, port=role)

    def input_ports(self, node_type):
        if node_type not in self.inputs:
            record = self.client.call("query", {"kind": "node-types",
                                                "nodeType": node_type})["record"]
            self.inputs[node_type] = {p["port"] for p in record["inputs"]}
        return self.inputs[node_type]

    def node_record(self, node):
        nodes = self.client.call("query", {"kind": "nodes",
                                           "composition": self.composition})["records"]
        for record in nodes:
            if record["id"] == node:
                return record
        raise RuntimeError(f"no node {node}")

    def parameter_id(self, node, role):
        record = self.node_record(node)
        if role in record["parameters"]:
            return record["parameters"][role]
        raise RuntimeError(f"node {node} has no {role}")

    def asset(self, fmt):
        if fmt not in self.assets:
            folder, _ = self.media[fmt]
            self.assets[fmt] = self.transact("bloom.asset.import", paths=[str(folder)],
                                             projectDirectory=str(self.directory))["asset"]
        return self.assets[fmt]

    def chain(self, source, port, effects, background=None, out_port="image"):
        """Feeds `effects` from (source, port); returns the last node. `out_port` is the port the
        effects chain on (an audio chain is "audio" all the way)."""
        node = source
        for effect in effects:
            node_type, values = effect[0], effect[1]
            options = effect[2] if len(effect) > 2 else {}
            fed_by = node
            fed_port = port
            node = self.transact("bloom.node.add-fed-by", composition=self.composition,
                                 nodeType=node_type, node=node, port=port)["node"]
            port = out_port
            if node_type == "bloom.light-wrap" and background is not None:
                self.transact("bloom.node.connect", composition=self.composition,
                              sourceNode=background, sourcePort="image", targetNode=node,
                              port="background")
            self.set_values(node, node_type, values)
            if options.get("depth"):
                # #483: the effect's depth input, a pass of the shot (or the layer's own channels).
                depth_node, depth_port = self.passes[options["depth"]]
                self.transact("bloom.node.connect", composition=self.composition,
                              sourceNode=depth_node, sourcePort=depth_port, targetNode=node,
                              port="depth")
                if "depthChannel" in options:
                    self.set_values(node, node_type, {"depthChannel": options["depthChannel"]})
            for input_port, name in (options.get("inputs") or {}).items():
                # #484: an extra image input (vectors, a matte), a pass of the shot.
                input_node, input_output = self.passes[name]
                self.transact("bloom.node.connect", composition=self.composition,
                              sourceNode=input_node, sourcePort=input_output, targetNode=node,
                              port=input_port)
            self.set_keys(node, options.get("keys") or {})
            for role, (driver_type, driver_values, driver_port, *driver_extra) in (
                    options.get("drives") or {}).items():
                # #567: a value node (Light Flicker) added beside the effect, its output driving one
                # of the effect's parameters. #568: "@previous" is the effect before this one in the
                # chain (a Camera Shake whose Offset and Roll drive a Rolling Shutter).
                if driver_type == "@previous":
                    driver = fed_by
                else:
                    driver = self.transact("bloom.node.add", composition=self.composition,
                                           nodeType=driver_type)["node"]
                    self.set_values(driver, driver_type, driver_values)
                    if "@picture" in driver_extra:
                        # #349: an Image Statistics driver measures the picture this effect reads.
                        self.transact("bloom.node.connect", composition=self.composition,
                                      sourceNode=fed_by, sourcePort=fed_port, targetNode=driver,
                                      port="input")
                self.transact("bloom.node.connect", composition=self.composition,
                              sourceNode=driver, sourcePort=driver_port, targetNode=node,
                              port=role)
            if options.get("mask"):
                # #383: the effect's mask input, limited to a named Mask of the shot.
                self.transact("bloom.node.connect", composition=self.composition,
                              sourceNode=self.mattes[options["mask"]], sourcePort="mask",
                              targetNode=node, port="mask")
                if "maskChannel" in options:
                    self.set_values(node, node_type, {"maskChannel": options["maskChannel"]})
        return node

    def node_type(self, node):
        nodes = self.client.call("query", {"kind": "nodes",
                                           "composition": self.composition})["records"]
        return next(r["typeId"] for r in nodes if r["id"] == node)

    def depth_passes(self, layer, source):
        """#483: the layer's own channel set as the pass "self", and each of its "passes" ({name:
        {"media": format, "colorSpace": label}}) as a source node of that footage read in that
        colour space (a depth pass is data: "Raw" reads its values unchanged)."""
        self.passes[f"{layer['name']}:channels"] = (source, "channels")
        for name, spec in (layer.get("passes") or {}).items():
            node = self.transact("bloom.node.add-asset-source", composition=self.composition,
                                 asset=self.asset(spec["media"]))["node"]
            if spec.get("colorSpace"):
                self.set_values(node, self.node_type(node), {"colorSpace": spec["colorSpace"],
                                                             "inputColorSpaceId": ""})
            self.passes[name] = (node, "image")

    def matte_nodes(self, layer, source):
        """#383: the layer's Cryptomatte selections (reading the source's channel set) and the
        Mask Combines of them, by name in self.mattes."""
        for name, mattes in (layer.get("cryptomattes") or {}).items():
            node = self.transact("bloom.node.add", composition=self.composition,
                                 nodeType="bloom.cryptomatte")["node"]
            self.transact("bloom.node.connect", composition=self.composition, sourceNode=source,
                          sourcePort="channels", targetNode=node, port="channels")
            self.set_values(node, "bloom.cryptomatte", {"mattes": mattes})
            self.mattes[name] = node
        for name, spec in (layer.get("qualifiers") or {}).items():
            # #523: an HSL Qualifier reading the layer's image, by name among the Masks.
            node = self.transact("bloom.node.add", composition=self.composition,
                                 nodeType="bloom.hsl-qualifier")["node"]
            self.transact("bloom.node.connect", composition=self.composition, sourceNode=source,
                          sourcePort="image", targetNode=node, port="input")
            self.set_values(node, "bloom.hsl-qualifier", spec.get("values") or {})
            self.set_keys(node, spec.get("keys") or {})
            self.mattes[name] = node
        for name, (mode, first, second) in (layer.get("maskCombine") or {}).items():
            node = self.transact("bloom.node.add", composition=self.composition,
                                 nodeType="bloom.mask-combine")["node"]
            for port, matte in (("a", first), ("b", second)):
                self.transact("bloom.node.connect", composition=self.composition,
                              sourceNode=self.mattes[matte], sourcePort="mask", targetNode=node,
                              port=port)
            self.set_values(node, "bloom.mask-combine", {"mode": mode})
            self.mattes[name] = node

    def layer_mattes(self, layer, output):
        """#383: the layer's masks shaped by named Masks and its node track matte."""
        if not layer.get("matteMasks") and not layer.get("trackMatte"):
            return
        record = next(r for r in self.records()["records"] if r["layerOutputNode"] == output)
        for name, mode in layer.get("matteMasks") or ():
            self.transact("bloom.layer.add-matte-mask", composition=self.composition,
                          layer=record["id"], node=self.mattes[name], port="mask",
                          mode=MASK_MODES[mode], name=name)
        if layer.get("trackMatte"):
            self.transact("bloom.layer.set-node-track-matte", composition=self.composition,
                          layer=record["id"], node=self.mattes[layer["trackMatte"]], port="mask")

    def layer_controls(self, output_node, layer):
        transform = dict(layer.get("transform") or {})
        if "position" in transform:
            x, y = transform["position"]
            transform["position"] = [x * self.width, y * self.height]
        values = {role: transform[role] for role in ("position", "scale", "rotation", "opacity")
                  if role in transform}
        if layer.get("blend"):
            values["blendMode"] = BLENDS[layer["blend"]]
        for role, value in values.items():
            argument = "integer" if role == "blendMode" else (
                "vector" if isinstance(value, list) else "number")
            self.transact("bloom.node.set-parameter-value", composition=self.composition,
                          node=output_node, role=role, **{argument: value})

    def records(self):
        return self.client.call("query", {"kind": "layers", "composition": self.composition})

    def footage(self, layer, merge=None):
        """Adds a footage layer: at the top of the composition's output Merge, or, with `merge`,
        as a source and Layer node feeding that (nested) Merge."""
        asset = self.asset(layer["media"])
        if merge is None:
            added = self.transact("bloom.layer.add-from-assets", composition=self.composition,
                                  time=0, assets=[asset])["layer"]
            record = next(r for r in self.records()["records"] if r["id"] == added)
            source, output = record["sourceNode"], record["layerOutputNode"]
        else:
            source = self.transact("bloom.node.add-asset-source", composition=self.composition,
                                   asset=asset)["node"]
            output = self.transact("bloom.node.add-fed-by", composition=self.composition,
                                   nodeType="bloom.layer-output", node=source,
                                   port="image")["node"]
            self.transact("bloom.node.connect", composition=self.composition, sourceNode=output,
                          sourcePort="image", targetNode=merge, role="content")
        color_layer = bench.spec.COLOR_LAYER.get(layer["media"])
        if color_layer:
            self.transact("bloom.node.set-parameter-value", composition=self.composition,
                          node=source, role="layer", text=color_layer)
        self.layers[layer["name"]] = {"source": source, "output": output}
        self.matte_nodes(layer, source)
        self.depth_passes(layer, source)
        background = None
        if layer.get("lightWrap"):
            background = self.layers[layer["lightWrap"]]["source"]
        last = self.chain(source, "image", layer.get("effects") or (), background)
        if last != source:
            self.transact("bloom.node.connect", composition=self.composition, sourceNode=last,
                          sourcePort="image", targetNode=output, port="image")
        self.layer_controls(output, layer)
        self.layer_mattes(layer, output)
        return output

    def nested(self, layer, output_merge):
        """A new Merge of the layer's own footage layers, whose picture (through its effects)
        is one layer of the output Merge."""
        merge = self.transact("bloom.node.add", composition=self.composition,
                              nodeType="bloom.layer-stack")["node"]
        for inner in layer["merge"]:
            self.footage(inner, merge)
        last = self.chain(merge, "image", layer.get("effects") or ())
        holder = self.transact("bloom.node.add-fed-by", composition=self.composition,
                               nodeType="bloom.layer-output", node=last, port="image")["node"]
        self.transact("bloom.node.connect", composition=self.composition, sourceNode=holder,
                      sourcePort="image", targetNode=output_merge, role="content")
        self.layer_controls(holder, layer)

    def generator(self, layer, output_merge):
        """A procedural source node (a Fields generator) whose `port` raster, through the
        layer's effects, is one layer of the output Merge."""
        node_type, values = layer["generator"]
        source = self.transact("bloom.node.add", composition=self.composition,
                               nodeType=node_type)["node"]
        self.set_values(source, node_type, values)
        self.set_keys(source, layer.get("keys") or {})
        if "pathNode" in layer:
            driver = self.transact("bloom.node.add", composition=self.composition,
                                   nodeType="bloom.value-path")["node"]
            self.set_keys(driver, {"value": layer["pathNode"]["keys"]})
            self.transact("bloom.parameter.set-source", composition=self.composition,
                          parameter=self.parameter_id(source, "path"), driverNode=driver,
                          driverPort="value")
        if "plotNode" in layer:
            plot = layer["plotNode"]
            group = self.function_group(plot["function"])
            driver = self.transact("bloom.node.add", composition=self.composition,
                                   nodeType="bloom.function-plot")["node"]
            self.set_values(driver, "bloom.function-plot", {"group": group, **plot["values"]})
            self.set_keys(driver, plot.get("keys") or {})
            self.transact("bloom.parameter.set-source", composition=self.composition,
                          parameter=self.parameter_id(source, "path"), driverNode=driver,
                          driverPort="path")
        if "pathChain" in layer:
            chain = layer["pathChain"]
            driver = self.transact("bloom.node.add", composition=self.composition,
                                   nodeType="bloom.value-path")["node"]
            self.set_keys(driver, {"value": chain["keys"]})
            port = "value"
            for step in chain["steps"]:
                step_node = self.transact("bloom.node.add", composition=self.composition,
                                          nodeType=step["type"])["node"]
                self.set_values(step_node, step["type"], step.get("values") or {})
                self.set_keys(step_node, step.get("keys") or {})
                self.transact("bloom.parameter.set-source", composition=self.composition,
                              parameter=self.parameter_id(step_node, "path"), driverNode=driver,
                              driverPort=port)
                driver, port = step_node, "path"
            self.transact("bloom.parameter.set-source", composition=self.composition,
                          parameter=self.parameter_id(source, "path"), driverNode=driver,
                          driverPort=port)
        if "textOutline" in layer:
            # #509: a text layer's glyph outlines (Text Outline reads the composition's first
            # text) through the path steps in turn, drawn by this generator.
            outline = layer["textOutline"]
            self.transact("bloom.layer.add-text", composition=self.composition,
                          name=outline.get("name", "Title"), text=outline["text"],
                          size=outline.get("size", 72.0))
            driver = self.transact("bloom.node.add", composition=self.composition,
                                   nodeType="bloom.text-outline")["node"]
            port = "path"
            for step in outline.get("steps") or ():
                step_node = self.transact("bloom.node.add", composition=self.composition,
                                          nodeType=step["type"])["node"]
                self.set_values(step_node, step["type"], step.get("values") or {})
                self.set_keys(step_node, step.get("keys") or {})
                self.transact("bloom.parameter.set-source", composition=self.composition,
                              parameter=self.parameter_id(step_node, "path"), driverNode=driver,
                              driverPort=port)
                driver, port = step_node, "path"
            self.transact("bloom.parameter.set-source", composition=self.composition,
                          parameter=self.parameter_id(source, "path"), driverNode=driver,
                          driverPort=port)
        if "axesNode" in layer:
            axes = layer["axesNode"]
            driver = self.transact("bloom.node.add", composition=self.composition,
                                   nodeType="bloom.axes")["node"]
            self.set_values(driver, "bloom.axes", axes["values"])
            self.set_keys(driver, axes.get("keys") or {})
            self.transact("bloom.parameter.set-source", composition=self.composition,
                          parameter=self.parameter_id(source, "path"), driverNode=driver,
                          driverPort="path")
        port = layer.get("port", "image")
        last = self.chain(source, port, layer.get("effects") or ())
        holder = self.transact("bloom.node.add-fed-by", composition=self.composition,
                               nodeType="bloom.layer-output", node=last,
                               port=port if last == source else "image")["node"]
        self.transact("bloom.node.connect", composition=self.composition, sourceNode=holder,
                      sourcePort="image", targetNode=output_merge, role="content")
        self.layers[layer["name"]] = {"source": source, "output": holder}
        self.layer_controls(holder, layer)
        return holder

    def function_group(self, name):
        """#590: a group definition made of Math nodes (scenario_spec.FUNCTION_GROUPS[name]) between a
        Group Input and a Group Output; answers the new composition's id."""
        host = self.composition
        group = self.transact("bloom.composition.add", name=name, width=64, height=64, duration=1,
                              frameRateNumerator=bench.spec.FPS, frameRateDenominator=1)["composition"]
        self.composition = group
        try:
            entry = self.transact("bloom.node.add", composition=group,
                                  nodeType="bloom.group-input")["node"]
            leave = self.transact("bloom.node.add", composition=group,
                                  nodeType="bloom.group-output")["node"]
            made = {}
            for step, operation, wiring in scenario_spec.FUNCTION_GROUPS[name]:
                node = self.transact("bloom.node.add", composition=group,
                                     nodeType="bloom.math")["node"]
                self.set_values(node, "bloom.math", {"operation": operation})
                made[step] = node
                for role, source in wiring.items():
                    driver, port = (entry, "scalar1") if source == "x" else (made[source], "result")
                    self.transact("bloom.parameter.set-source", composition=group,
                                  parameter=self.parameter_id(node, role), driverNode=driver,
                                  driverPort=port)
            self.transact("bloom.parameter.set-source", composition=group,
                          parameter=self.parameter_id(leave, "scalar1"), driverNode=node,
                          driverPort="result")
        finally:
            self.composition = host
        return group

    def audio(self, layer):
        """#569: a generated stereo tone with seeded noise as an audio layer, fed through the layer's
        audio effects before its Layer Output's audio input."""
        spec = layer["audio"]
        path = self.directory / f"{layer['name']}.wav"
        write_tone(path, spec.get("seconds", 10))
        asset = self.transact("bloom.asset.import", paths=[str(path)],
                              projectDirectory=str(self.directory))["asset"]
        added = self.transact("bloom.layer.add-audio", composition=self.composition,
                              asset=asset)["layer"]
        record = next(r for r in self.records()["records"] if r["id"] == added)
        source, output = record["sourceNode"], record["layerOutputNode"]
        last = self.chain(source, "audio", spec.get("effects") or (), out_port="audio")
        if last != source:
            self.transact("bloom.node.connect", composition=self.composition, sourceNode=last,
                          sourcePort="audio", targetNode=output, port="audio")
        # #347: Audio Waveform and Audio Spectrum read the source's audio and draw an image, which
        # takes its own Layer Output into the output Merge.
        for plot in spec.get("plots") or ():
            drawn = self.chain(source, "audio", [plot], out_port="image")
            holder = self.transact("bloom.node.add-fed-by", composition=self.composition,
                                   nodeType="bloom.layer-output", node=drawn, port="image")["node"]
            self.transact("bloom.node.connect", composition=self.composition, sourceNode=holder,
                          sourcePort="image", targetNode=self.output_merge(), role="content")
        self.layers[layer["name"]] = {"source": source, "output": output}

    def output_merge(self):
        entries = self.records()["entries"]
        found = [entry["merge"] for entry in entries if entry.get("outputMerge")]
        if not found:
            # A Merge with no entries yet: the one feeding the composition output.
            nodes = self.client.call("query", {"kind": "nodes",
                                               "composition": self.composition})["records"]
            output = next((r for r in nodes if r["typeId"] == "bloom.composition-output"), None)
            source = next((i["source"] for i in (output or {}).get("inputs", ())
                           if i["port"] == "image" and i.get("source")), None)
            found = [source["node"]] if source else []
        if not found:
            raise RuntimeError("the composition has no output Merge")
        return found[0]

    def build(self, name, frames):
        self.composition = self.transact(
            "bloom.composition.add", name=name, width=self.width, height=self.height,
            duration=frames, frameRateNumerator=bench.spec.FPS,
            frameRateDenominator=1)["composition"]
        merge = None
        for layer in self.scenario["layers"]:
            if "merge" in layer:
                self.nested(layer, merge or self.output_merge())
            elif "generator" in layer:
                self.generator(layer, merge or self.output_merge())
            elif "audio" in layer:
                self.audio(layer)
            elif "adjustment" in layer:
                merge = merge or self.output_merge()
                added = self.transact("bloom.merge.add-adjustment-layer",
                                      composition=self.composition, merge=merge)
                self.chain(added["node"], "image", layer["adjustment"])
            else:
                self.footage(layer)
            merge = merge or self.output_merge()
        self.transact("bloom.composition.set-work-area", composition=self.composition, start=0,
                      end=frames)
        return self.composition


def write_tone(path, seconds, rate=48000):
    """A deterministic stereo test sound: two slowly modulated tones and seeded noise, 16-bit."""
    import array  # noqa: PLC0415
    import math  # noqa: PLC0415
    import random  # noqa: PLC0415
    import wave  # noqa: PLC0415
    noise = random.Random(7)
    samples = array.array("h")
    for index in range(int(seconds * rate)):
        t = index / rate
        swell = 0.25 + 0.2 * math.sin(2 * math.pi * 0.4 * t)
        for tone in (220.0, 330.0):
            value = swell * math.sin(2 * math.pi * tone * t) + 0.02 * (noise.random() - 0.5)
            samples.append(int(max(-1.0, min(1.0, value)) * 32767))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(samples.tobytes())


def build_scenario(args, name, scenario, media, project, log):
    tier = bench.spec.TIERS[scenario["tier"]]
    width, height = tier[0], tier[1]
    frames = args.frames or scenario["frames"]
    project.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with Instance(args.bloom, args.scratch, args.driver_cache, log) as instance:
        client = instance.client
        client.call("project.new", {"discardChanges": True})
        builder = Builder(client, scenario, media, width, height, project.parent)
        composition = builder.build(name, frames)
        builder.transact("bloom.project.set-name", name=name)
        client.call("project.save-as", {"path": str(project), "replace": True})
        nodes = client.call("query", {"kind": "nodes", "composition": composition})["records"]
    return {"composition": composition, "frames": frames, "width": width, "height": height,
            "nodes": len(nodes), "buildSeconds": round(time.monotonic() - started, 2)}

# --- measurements --------------------------------------------------------------------------------


def open_project(client, project, timeout=300):
    """project.open, then wait until the project has finished opening (calls answer
    bloom.mcp.project-unavailable until then)."""
    client.call("project.open", {"path": str(project), "discardChanges": True}, timeout=timeout)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        answer = client.call("query", {"kind": "compositions"}, check=False)
        if answer.get("records"):
            return answer
        time.sleep(0.05)
    raise RuntimeError(f"{project} did not open")


def open_shot(instance, project, composition, timeout=300):
    """project.open, the composition in the Viewer at Full, playhead at 0; returns the first
    Current frame's timing and provenance."""
    client = instance.client
    opened = time.monotonic()
    open_project(client, project)
    client.call("composition.activate", {"composition": composition})
    client.call("viewer.set", {"resolution": "full", "zoom": "fit"})
    client.call("transport", {"action": "seek", "time": 0})
    first = wait_for(client, lambda s: s["freshness"] == "current" and s["frame"] == 0, timeout)
    return {"firstFrameSeconds": round(time.monotonic() - opened, 3) if first else None,
            "firstFrameProvider": first and first["provider"],
            "firstFrameCpuFallbacks": first and first["cpuFallbacks"]}


def delta(after, before, key):
    if after.get(key) is None or before.get(key) is None:
        return None
    return after[key] - before[key]


def fallback_reasons(after, before):
    reasons = {}
    for reason, count in (after.get("cpuFallbacksByReason") or {}).items():
        previous = (before.get("cpuFallbacksByReason") or {}).get(reason) or 0
        if count is not None and count - previous:
            reasons[reason] = count - previous
    return reasons


def high_claim_seconds(samples, fraction):
    """The longest stretch, in seconds, over which device claims stayed above `fraction` of the
    usable device capacity."""
    longest, since = 0.0, None
    for sample in samples:
        high = (sample.get("deviceClaim") is not None and bool(sample.get("deviceUsable")) and
                sample["deviceClaim"] > fraction * sample["deviceUsable"])
        if high and since is None:
            since = sample["t"]
        elif not high:
            since = None
        if since is not None:
            longest = max(longest, sample["t"] - since)
    return round(longest, 2)


def stream_summary(samples, fraction=0.9):
    """Peaks over a measured window and the frame provenance seen in it."""
    def peak(key):
        values = [s[key] for s in samples if s.get(key) is not None]
        return max(values) if values else None
    providers = {}
    for sample in samples:
        if sample["provider"]:
            providers[sample["provider"]] = providers.get(sample["provider"], 0) + 1
    tasks = sorted({f"{name} ({executor})" for s in samples for name, executor, _ in s["tasks"]})
    return {"peakDeviceClaimBytes": peak("deviceClaim"), "peakDeviceLiveBytes": peak("deviceLive"),
            "peakHeapUsageBytes": peak("heapUsage"), "peakHostClaimBytes": peak("hostClaim"),
            "peakAnonBytes": peak("anon"), "peakRssBytes": peak("rss"),
            "peakPreviewHostBytes": peak("previewHost"),
            "peakPreviewDeviceBytes": peak("previewDevice"),
            "minUsableBytes": min((s["usable"] for s in samples if s.get("usable")), default=None),
            "deviceUsableBytes": peak("deviceUsable"),
            "displayedProviders": providers, "tasksSeen": tasks, "samples": len(samples),
            "highClaimSeconds": high_claim_seconds(samples, fraction)}


def playback_rate(samples, frames):
    """Playback over a window from the playhead and the dropped-frame count (the profiler's own
    playback rate needs an open Profiler): the transport's frames per second, unwrapped across
    loop restarts, and the frames it showed (advanced minus dropped) per second."""
    rows = [s for s in samples if s.get("playhead") is not None]
    if len(rows) < 2 or rows[-1]["t"] <= rows[0]["t"]:
        return {}
    advanced = 0
    for previous, current in zip(rows, rows[1:]):
        step = current["playhead"] - previous["playhead"]
        advanced += step if step >= 0 else step + frames
    seconds = rows[-1]["t"] - rows[0]["t"]
    dropped = None
    if rows[-1].get("dropped") is not None and rows[0].get("dropped") is not None:
        dropped = max(0, rows[-1]["dropped"] - rows[0]["dropped"])
    return {"transportFps": round(advanced / seconds, 2),
            "shownFps": round((advanced - (dropped or 0)) / seconds, 2),
            "droppedFrames": dropped, "seconds": round(seconds, 2)}


def measure_fill(args, shot, media_files, log):
    cold = bench.drop_page_cache(media_files)
    with Instance(args.bloom, args.scratch, args.driver_cache, log) as instance:
        sampler = bench.Sampler(instance.process.pid).start()
        row = {"kind": "fill", "coldDropped": cold}
        try:
            row.update(open_shot(instance, shot["project"], shot["composition"],
                                 args.first_frame_timeout))
            client = instance.client
            watcher = Watcher(instance).start()
            before = snapshot(client)
            started, started_epoch = time.monotonic(), time.time()
            client.call("transport", {"action": "ram-preview"})
            began = wait_for(client, lambda s: s["caching"] or s["cached"] == shot["frames"], 30)
            done, stalled = None, False
            progress_at, progress = time.monotonic(), -1
            while began and time.monotonic() - started < args.fill_timeout:
                current = snapshot(client)
                if not current["caching"]:
                    done = current
                    break
                if (current["cached"] or 0) != progress:
                    progress_at, progress = time.monotonic(), current["cached"] or 0
                elif time.monotonic() - progress_at > args.stall_seconds:
                    stalled = True  # no frame cached for stall_seconds: stop the run
                    break
                time.sleep(0.1)
            ended, ended_epoch = time.monotonic(), time.time()
            after = done or snapshot(client)
            fill_seconds = ended - started
            row.update({
                "fillSeconds": round(fill_seconds, 3), "fillTimedOut": done is None,
                "fillStalled": stalled,
                "totalFrames": after["total"], "cachedFrames": after["cached"],
                "fillFps": round((after["cached"] or 0) / fill_seconds, 3),
                "spanFirstFrame": after["spanFirst"], "spanFrames": after["spanFrames"],
                "cpuFallbackFrames": delta(after, before, "cpuFallbacks"),
                "cpuFallbacksByReason": fallback_reasons(after, before),
                "gpuAdmissionRefusals": delta(after, before, "gpuAdmissionRefusals"),
                "gpuCompletions": delta(after, before, "gpuCompletions"),
                "capacityWaits": delta(after, before, "capacityWaits")})
            row.update(sampler.window(started_epoch, ended_epoch))
            # Cached playback: ram-preview plays the range once it is cached.
            time.sleep(args.play_seconds)
            played = snapshot(client)
            client.call("transport", {"action": "stop"})
            samples = watcher.stop()
            fill_samples = [s for s in samples if started <= s["t"] <= ended]
            rate = playback_rate([s for s in samples if s["t"] > ended], shot["frames"])
            row.update({"cachedPlaybackFps": rate.get("shownFps"),
                        "cachedPlayTransportFps": rate.get("transportFps"),
                        "cachedPlayDropped": rate.get("droppedFrames"),
                        "cachedPlayCpuFallbacks": delta(played, after, "cpuFallbacks")})
            # Contiguity from the playhead (frame 0): the out-of-order gap at every sample.
            gaps = [(s["cached"] or 0) - (s["spanFrames"] or 0) for s in fill_samples]
            starts = {s["spanFirst"] for s in fill_samples if s["spanFrames"]}
            row.update({"maxOutOfOrderFrames": max(gaps, default=0),
                        "spanStartsSeen": sorted(starts)})
            row.update(stream_summary(fill_samples, args.claim_capacity_percent / 100))
            row["previewCeilingBytes"] = after["previewCeiling"]
        finally:
            sampler.finish()
    return row


def measure_playback(args, shot, media_files, log):
    cold = bench.drop_page_cache(media_files)
    with Instance(args.bloom, args.scratch, args.driver_cache, log) as instance:
        row = {"kind": "playback", "coldDropped": cold}
        row.update(open_shot(instance, shot["project"], shot["composition"],
                             args.first_frame_timeout))
        if row["firstFrameSeconds"] is None:
            row["error"] = f"no first frame within {args.first_frame_timeout:g} s"
            return row
        client = instance.client
        watcher = Watcher(instance).start()
        before = snapshot(client)
        client.call("transport", {"action": "play"})
        time.sleep(args.play_seconds)
        after = snapshot(client)
        client.call("transport", {"action": "stop"})
        samples = watcher.stop()
        rate = playback_rate(samples, shot["frames"])
        row.update({"playbackFps": rate.get("shownFps"),
                    "playTransportFps": rate.get("transportFps"),
                    "playDropped": rate.get("droppedFrames"),
                    "cpuFallbackFrames": delta(after, before, "cpuFallbacks"),
                    "cpuFallbacksByReason": fallback_reasons(after, before)})
        row.update(stream_summary(samples, args.claim_capacity_percent / 100))
    return row


def measure_scrub(args, shot, media_files, log):
    cold = bench.drop_page_cache(media_files)
    frames = shot["frames"]
    half = frames // 2
    count = max(2, args.scrub)
    targets = []
    for index in range(count):
        step = (index // 2) * half // max(1, (count + 1) // 2)
        targets.append((half + step) % frames if index % 2 == 0 else step % half + 1)
    with Instance(args.bloom, args.scratch, args.driver_cache, log) as instance:
        row = {"kind": "scrub", "coldDropped": cold, "targets": targets}
        row.update(open_shot(instance, shot["project"], shot["composition"],
                             args.first_frame_timeout))
        if row["firstFrameSeconds"] is None:
            row["error"] = f"no first frame within {args.first_frame_timeout:g} s"
            return row
        client = instance.client
        before = snapshot(client)
        timings, providers = [], {}
        for target in targets:
            started = time.monotonic()
            client.call("transport", {"action": "seek", "time": target})
            shown = wait_for(client, lambda s, t=target: s["freshness"] == "current" and
                             s["frame"] == t, args.scrub_timeout, 0.02)
            timings.append(round(time.monotonic() - started, 3) if shown else None)
            if shown:
                providers[shown["provider"]] = providers.get(shown["provider"], 0) + 1
        after = snapshot(client)
        measured = [t for t in timings if t is not None]
        row.update({"scrubSeconds": timings,
                    "scrubMedianSeconds": statistics.median(measured) if measured else None,
                    "scrubMaxSeconds": max(measured) if measured else None,
                    "scrubMissed": len(timings) - len(measured), "displayedProviders": providers,
                    "cpuFallbackFrames": delta(after, before, "cpuFallbacks"),
                    "cpuFallbacksByReason": fallback_reasons(after, before)})
    return row


def measure_export(args, shot, media_files, out, log):
    cold = bench.drop_page_cache(media_files)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    # A few frames: enough for the rate, the provenance and the parity frames, and short enough
    # that the suite never holds the GPU for long.
    last = min(shot["frames"], args.export_frames) - 1
    with Instance(args.bloom, args.scratch, args.driver_cache, log) as instance:
        row = {"kind": "export", "coldDropped": cold, "preset": EXR_PRESET}
        sampler = bench.Sampler(instance.process.pid).start()
        try:
            client = instance.client
            open_project(client, shot["project"])
            watcher = Watcher(instance).start()
            started, started_epoch = time.monotonic(), time.time()
            result = client.call("render", {
                "composition": shot["composition"], "first": 0, "last": last,
                "preset": EXR_PRESET, "destination": str(out / "frame.####.exr"),
                "overwrite": "replace"}, timeout=args.export_timeout, check=False,
                progress=True)
            seconds = time.monotonic() - started
            row.update(sampler.window(started_epoch, time.time()))
            samples = watcher.stop()
        finally:
            sampler.finish()
        frames = result.get("publishedFrames") or 0
        row.update({"exportFrames": last + 1,
                    "succeeded": bool(result.get("succeeded", frames == last + 1)),
                    "exportSeconds": round(seconds, 3), "publishedFrames": frames,
                    "exportSecondsPerFrame": round(seconds / frames, 4) if frames else None,
                    "job": result.get("job")})
        if not frames:
            row["error"] = json.dumps(result)[:2000]
        # The run log's warnings and errors (a CPU frame is reported there with its reason).
        row["exportLog"] = [f"{entry.get('severity')} {entry.get('code')} frame "
                            f"{entry.get('frame')}: {entry.get('message')}"
                            for entry in result.get("log") or ()
                            if entry.get("severity") not in ("info", "debug")][:24]
        status = client.call("export.status", {}, check=False)
        verification = None
        for job in (status.get("recentTransientJobs") or []) + (status.get("jobs") or []):
            if job.get("id") == result.get("job"):
                for output in job.get("outputs") or ():
                    verification = output.get("verification") or verification
        if verification:
            row.update({"gpuFrames": verification.get("gpuFrames"),
                        "cpuFallbackFrames": verification.get("cpuReferenceFrames"),
                        "gpuDisplayFrames": verification.get("gpuDisplayFrames"),
                        "cpuDisplayFrames": verification.get("cpuDisplayFrames"),
                        "verification": verification.get("overall")})
        row.update(stream_summary(samples, args.claim_capacity_percent / 100))
        row["files"] = sorted(str(path) for path in out.glob("frame.*.exr"))
    return row


def measure_audio_export(args, shot, out, log):
    """#569, #570: the composition's sound exported as WAV as mixed, then normalized. Seconds are the
    wall time of each export; the normalized one reports the measured and delivered loudness."""
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    spec = shot["audioExport"]
    row = {"kind": "audio-export", "runs": []}
    with Instance(args.bloom, args.scratch, args.driver_cache, log) as instance:
        client = instance.client
        open_project(client, shot["project"])
        for label, extra in (("mixed", {}),
                             ("normalized", {"loudnessLufs": spec["loudnessLufs"],
                                             "truePeakDbtp": spec["truePeakDbtp"]})):
            started = time.monotonic()
            result = client.call("export", {
                "composition": shot["composition"], "first": 0, "last": shot["frames"] - 1,
                "preset": "PcmWavV1", "destination": str(out / f"{label}.wav"),
                "overwrite": "replace", "audio": True, **extra},
                timeout=args.export_timeout, check=False, progress=True)
            run = {"label": label, "seconds": round(time.monotonic() - started, 3),
                   "succeeded": bool(result.get("files"))}
            if result.get("loudness"):
                run["loudness"] = result["loudness"]
            if not run["succeeded"]:
                run["error"] = json.dumps(result)[:2000]
            row["runs"].append(run)
    return row


def cpu_reference(args, project, frame, out, log):
    """One frame on the CPU reference path: bloom-cli render with no Vulkan driver to find."""
    env = bench.bench_env()
    env.update(VK_ICD_FILENAMES=str(args.scratch / "no-driver.json"),
               VK_DRIVER_FILES=str(args.scratch / "no-driver.json"))
    shutil.rmtree(out, ignore_errors=True)
    command = [str(args.cli), "render", "--project", str(project), "--frame", str(frame),
               "--preset", EXR_PRESET, "--out", str(out), "--replace"]
    log.write("$ " + " ".join(command) + "\n")
    started = time.monotonic()
    done = subprocess.run(command, capture_output=True, text=True, env=env,
                          timeout=args.export_timeout)
    log.write(done.stdout + done.stderr + "\n")
    device = {}
    for line in done.stdout.splitlines():
        if line.startswith("device "):
            device = dict(part.split("=") for part in line.split()[1:])
    return {"returncode": done.returncode, "seconds": round(time.monotonic() - started, 2),
            "gpuFrames": int(device.get("gpu-frames", -1)),
            "cpuReferenceFrames": int(device.get("cpu-reference-frames", -1)),
            "path": out / "frame.exr"}


def parity(gpu_path, cpu_path, keyed_alpha=False):
    """The documented reference-parity gate: every finite RGB component within 2e-6 absolute or
    relative, alpha exact, no finite CPU value non-finite on the GPU. With `keyed_alpha` (the shot
    keys alpha from tolerance-qualified colour, scenario_spec.keyed_alpha), alpha is held to that
    same 2e-6 tolerance instead (#545). `alphaMismatches` counts every alpha sample that is not
    exact, `alphaAboveGate` those beyond the alpha gate, `maxAlphaError` the largest difference."""
    import numpy as np  # noqa: PLC0415 (only the export check needs numpy)
    import imageio_min  # noqa: PLC0415
    gpu, cpu = imageio_min.read_exr(gpu_path), imageio_min.read_exr(cpu_path)
    result = {"failingComponents": 0, "maxAbsError": 0.0, "nonFinite": 0, "alphaMismatches": 0,
              "alphaAboveGate": 0, "maxAlphaError": 0.0,
              "alphaGate": "2e-6" if keyed_alpha else "exact"}
    for channel in ("R", "G", "B", "A"):
        a = np.asarray(gpu[channel], dtype=np.float64)
        e = np.asarray(cpu[channel], dtype=np.float64)
        finite = np.isfinite(e)
        result["nonFinite"] += int(np.count_nonzero(finite & ~np.isfinite(a)))
        error = np.abs(np.where(finite, a - e, 0.0))
        result["maxAbsError"] = max(result["maxAbsError"], float(np.nanmax(error)))
        allowed = 2e-6 * np.maximum(1.0, np.abs(e))
        if channel == "A":
            result["maxAlphaError"] = float(np.nanmax(error))
            result["alphaMismatches"] = int(np.count_nonzero(finite & (a != e)))
            result["alphaAboveGate"] = int(np.count_nonzero(
                finite & (error > allowed) if keyed_alpha else finite & (a != e)))
        else:
            result["failingComponents"] += int(np.count_nonzero(finite & (error > allowed)))
    result["passed"] = not (result["failingComponents"] or result["nonFinite"] or
                            result["alphaAboveGate"])
    return result


def measure_parity(args, shot, export, work, log):
    files = export.get("files") or []
    if not files:
        return {"kind": "parity", "skipped": "no exported frames"}
    count = max(1, args.reference_frames)
    last = (export.get("exportFrames") or shot["frames"]) - 1
    frames = sorted({round(index * last / max(1, count - 1)) for index in range(count)})
    rows = []
    for frame in frames:
        gpu_path = next((Path(f) for f in files if f.endswith(f".{frame:04d}.exr")), None)
        reference = cpu_reference(args, shot["project"], frame, work / f"cpu-{frame}", log)
        entry = {"frame": frame, "cpuSeconds": reference["seconds"],
                 "referenceCpuFrames": reference["cpuReferenceFrames"],
                 "referenceGpuFrames": reference["gpuFrames"]}
        if reference["returncode"] != 0 or not reference["path"].is_file():
            entry["error"] = f"bloom-cli render exited {reference['returncode']}"
        elif reference["cpuReferenceFrames"] != 1 or reference["gpuFrames"] != 0:
            entry["error"] = "the reference render did not run on the CPU reference path"
        elif gpu_path is None:
            entry["error"] = "the export has no such frame"
        else:
            entry.update(parity(gpu_path, reference["path"],
                                keyed_alpha=bool(shot.get("keyedAlpha"))))
        rows.append(entry)
    return {"kind": "parity", "frames": rows,
            "passed": all(row.get("passed") for row in rows)}

# --- checks --------------------------------------------------------------------------------------


def check(name, passed, detail):
    return {"check": name, "outcome": "pass" if passed is True else (
        "not-measured" if passed is None else "fail"), "detail": detail}


def evaluate(args, shot, rows):
    fill = next((r for r in rows if r["kind"] == "fill"), {})
    export = next((r for r in rows if r["kind"] == "export"), {})
    parity_row = next((r for r in rows if r["kind"] == "parity"), {})
    results = []
    # 1. No GPU-capable frame on the CPU: the GPU route's fallback counters, the displayed frames'
    #    providers, the first frames and the export's provenance.
    fallbacks = {r["kind"]: r.get("cpuFallbackFrames") for r in rows if "cpuFallbackFrames" in r}
    providers = set()
    for r in rows:
        providers.update((r.get("displayedProviders") or {}).keys())
        if r.get("firstFrameProvider"):
            providers.add(r["firstFrameProvider"])
    cpu_providers = sorted(providers - set(GPU_PROVIDERS))
    first_fallbacks = [r.get("firstFrameCpuFallbacks") for r in rows if "firstFrameProvider" in r]
    counted = list(fallbacks.values()) + first_fallbacks + [fill.get("cachedPlayCpuFallbacks")]
    cpu_tasks = sorted({t for r in rows for t in r.get("tasksSeen") or ()
                        if "cpu reference" in t.lower() or "fallback" in t.lower()})
    passed = (None if any(v is None for v in counted) else
              all(v == 0 for v in counted) and not cpu_providers and not cpu_tasks)
    results.append(check("no-cpu-fallback", passed, {
        "cpuFallbackFrames": fallbacks, "firstFrameCpuFallbacks": first_fallbacks,
        "cpuProvidersShown": cpu_providers, "cpuTaskNames": cpu_tasks,
        "reasons": {r["kind"]: r.get("cpuFallbacksByReason") for r in rows
                    if r.get("cpuFallbacksByReason")}}))
    # 2. Claims within the stated factor of the measured peak use.
    claim_rows = [r for r in rows if r.get("peakDeviceClaimBytes") is not None]
    ratios = {}
    for r in claim_rows:
        if r.get("peakDeviceLiveBytes"):
            ratios[f"{r['kind']}.device"] = round(r["peakDeviceClaimBytes"] /
                                                  r["peakDeviceLiveBytes"], 2)
        if r.get("peakHostClaimBytes") is not None and r.get("peakAnonBytes"):
            ratios[f"{r['kind']}.host"] = round(r["peakHostClaimBytes"] / r["peakAnonBytes"], 2)
    results.append(check("claims-within-factor",
                         None if not ratios else all(v <= args.claim_factor
                                                     for v in ratios.values()),
                         {"factor": args.claim_factor, "claimOverPeakUse": ratios}))
    # 3. The fill ends by itself (not stalled, not timed out), the GPU is busy during it, and the
    #    cache fills contiguously from the playhead.
    ended = None if "fillTimedOut" not in fill else not (fill["fillTimedOut"] or
                                                         fill.get("fillStalled"))
    # The Viewer shows a first frame in every measurement and every scrub target.
    viewer_rows = [r for r in rows if r["kind"] in ("fill", "playback", "scrub")]
    no_first = [r["kind"] for r in viewer_rows if "firstFrameSeconds" in r and
                r["firstFrameSeconds"] is None]
    missed = sum(r.get("scrubMissed") or 0 for r in viewer_rows)
    progress = None if not any("firstFrameSeconds" in r for r in viewer_rows) else (
        not no_first and not missed and not fill.get("fillStalled"))
    reasons = []
    if no_first:
        reasons.append(f"the Viewer showed no first frame within {args.first_frame_timeout:g} s "
                       f"({', '.join(no_first)})")
    if missed:
        reasons.append(f"{missed} scrub target(s) not shown within {args.scrub_timeout:g} s")
    if fill.get("fillStalled"):
        reasons.append(f"the fill cached no frame for {args.stall_seconds:g} s "
                       f"({fill.get('cachedFrames')} of {fill.get('totalFrames')} cached)")
    results.append(check("viewer-and-fill-progress", progress, {
        "message": "; ".join(reasons) or "the fill and the Viewer made progress"}))
    # Device claims do not sit at the usable capacity (work waiting for room instead of running).
    high = {r["kind"]: r["highClaimSeconds"] for r in rows if r.get("highClaimSeconds") is not None}
    worst = max(high.values(), default=None)
    results.append(check("claims-below-capacity",
                         None if worst is None else worst <= args.claim_capacity_seconds, {
        "message": (f"device claims stayed above {args.claim_capacity_percent:g}% of usable "
                    f"capacity for {worst:g} s (limit {args.claim_capacity_seconds:g} s)")
        if worst is not None else "not measured", "secondsByMeasurement": high}))
    results.append(check("fill-completes", ended, {
        key: fill.get(key) for key in ("fillSeconds", "fillTimedOut", "fillStalled",
                                       "cachedFrames", "totalFrames", "capacityWaits")}))
    busy = fill.get("gpuBusyPercent")
    results.append(check("gpu-busy-during-fill",
                         None if busy is None else busy >= args.gpu_busy_percent,
                         {"gpuBusyPercent": busy, "minimum": args.gpu_busy_percent}))
    contiguous = None
    if fill.get("cachedFrames") is not None:
        contiguous = (fill.get("spanFirstFrame") == 0 and
                      fill.get("spanFrames") == fill.get("cachedFrames") and
                      set(fill.get("spanStartsSeen") or [0]) == {0})
    results.append(check("fill-contiguous-from-playhead", contiguous, {
        "spanFirstFrame": fill.get("spanFirstFrame"), "spanFrames": fill.get("spanFrames"),
        "cachedFrames": fill.get("cachedFrames"), "spanStartsSeen": fill.get("spanStartsSeen"),
        "maxOutOfOrderFrames": fill.get("maxOutOfOrderFrames")}))
    # 4. RAM preview within the usable-memory rule, with no fixed cap.
    within = None
    if fill.get("minUsableBytes"):
        usable = fill["minUsableBytes"]
        held = fill.get("peakPreviewHostBytes") or 0
        complete = fill.get("cachedFrames") == fill.get("totalFrames")
        ceiling = fill.get("previewCeilingBytes") or 0
        # A ceiling that follows usable memory (not a fixed figure), frames within it, and a fill
        # that stopped early only when memory, not a cap, was the limit.
        follows = ceiling >= 0.9 * usable
        limited = complete or held >= 0.9 * usable or (
            fill.get("deviceUsableBytes") and (fill.get("peakDeviceClaimBytes") or 0) >=
            0.9 * fill["deviceUsableBytes"])
        within = held <= usable and follows and bool(limited)
    results.append(check("ram-preview-usable-memory", within, {
        "peakPreviewHostBytes": fill.get("peakPreviewHostBytes"),
        "peakPreviewDeviceBytes": fill.get("peakPreviewDeviceBytes"),
        "previewCeilingBytes": fill.get("previewCeilingBytes"),
        "usableBytes": fill.get("minUsableBytes"), "cachedFrames": fill.get("cachedFrames"),
        "totalFrames": fill.get("totalFrames")}))
    # 5. Export: complete, zero CPU-fallback frames, parity with the CPU reference.
    export_ok = None
    if export:
        wanted = export.get("exportFrames") or shot["frames"]
        export_ok = (export.get("publishedFrames") == wanted and
                     export.get("cpuFallbackFrames") == 0 and export.get("gpuFrames") == wanted)
    results.append(check("export-gpu-complete", export_ok, dict(
        {key: export.get(key) for key in ("exportFrames", "publishedFrames", "gpuFrames",
                                          "cpuFallbackFrames", "verification", "error")},
        message=(f"{export.get('gpuFrames')} GPU and {export.get('cpuFallbackFrames')} CPU "
                 f"reference frames of {export.get('exportFrames')}") if export else
        "not measured")))
    # The final render is not far slower than the preview fill of the same frames.
    rate = None
    fill_per_frame = (fill["fillSeconds"] / fill["cachedFrames"]
                      if fill.get("cachedFrames") and fill.get("fillSeconds") else None)
    message = "not measured"
    seconds = export.get("exportSecondsPerFrame")
    if seconds is not None and fill_per_frame is not None and ended:
        limit = args.export_fill_factor * fill_per_frame + args.export_allowance
        rate = seconds <= limit
        message = (f"export {seconds:g} s/frame against the fill's {fill_per_frame:.3g} s/frame "
                   f"(limit {limit:.3g} s: {args.export_fill_factor:g} x fill + "
                   f"{args.export_allowance:g} s)")
    elif seconds is not None and fill:
        # A fill that stalled or timed out gives no preview rate to compare against.
        rate = False
        message = (f"export {seconds:g} s/frame, and the fill did not complete, so there is no "
                   "preview rate it keeps up with")
    results.append(check("export-rate", rate, {
        "message": message,
        "exportGpuBusyPercent": export.get("gpuBusyPercent")}))
    keyed = shot.get("keyedAlpha") or []
    described = []
    for frame in parity_row.get("frames") or ():
        if frame.get("error"):
            described.append(f"frame {frame['frame']}: {frame['error']}")
            continue
        alpha = (f"max alpha difference {frame.get('maxAlphaError', 0.0):.3g} "
                 f"({frame.get('alphaMismatches', 0)} alpha samples not exact)")
        if frame.get("passed"):
            described.append(f"frame {frame['frame']}: within the gate (max abs "
                             f"{frame['maxAbsError']:.3g}, {alpha})")
        else:
            described.append(f"frame {frame['frame']}: {frame['failingComponents']} RGB "
                             f"components above 2e-6 (max abs {frame['maxAbsError']:.3g}), "
                             f"{frame['alphaAboveGate']} alpha "
                             f"samples beyond the {'2e-6' if keyed else 'exact'} alpha gate, "
                             f"{alpha}, {frame['nonFinite']} non-finite")
    gate = "2e-6 absolute or relative per RGB component, " + (
        "alpha keyed from tolerance-qualified colour within 2e-6 (" + "; ".join(keyed) + ")"
        if keyed else "exact alpha")
    results.append(check("export-cpu-parity", parity_row.get("passed") if parity_row and
                         "frames" in parity_row else None,
                         {"message": "; ".join(described) or "not measured",
                          "gate": gate,
                          "frames": parity_row.get("frames"),
                          "skipped": parity_row.get("skipped")}))
    return known(results, shot.get("knownFailures") or {})


def known(results, failures):
    """A check the scenario lists as a known failure (with its issue) that does not pass is
    `known-failure`, which does not fail the run; one that passes says it no longer fails."""
    for item in results:
        reference = failures.get(item["check"])
        if not reference:
            continue
        item["knownFailure"] = reference
        if item["outcome"] != "pass":
            item["outcome"] = "known-failure"
    return results


def failing(checks):
    return [c for c in checks if c["outcome"] not in ("pass", "known-failure")]

# --- the run -------------------------------------------------------------------------------------


def scenario_media(root, manifest, tier, formats, frames=0):
    found = {}
    for fmt in formats:
        if fmt in bench.spec.DERIVED_PASSES:
            # #483: a pass derived from the rendered media, written on first use.
            found[fmt] = bench_media.derive_pass(root, tier, fmt, frames)
            continue
        entry = manifest["assets"].get(f"{tier}/{fmt}")
        if entry is None:
            raise SystemExit(f"the manifest has no {tier}/{fmt} footage")
        folder = bench.media_path(root, entry)
        if not folder.exists():
            raise SystemExit(f"missing media {folder}; generate it with bench_media.py")
        found[fmt] = (folder, entry)
    return found


def scenario_formats(scenario):
    formats = set()
    for layer in scenario["layers"]:
        for inner in layer.get("merge") or [layer]:
            if inner.get("media"):
                formats.add(inner["media"])
            formats.update(spec["media"] for spec in (inner.get("passes") or {}).values())
    return sorted(formats)


def summary_metrics(rows):
    fill = next((r for r in rows if r["kind"] == "fill"), {})
    export = next((r for r in rows if r["kind"] == "export"), {})
    return {"fillFps": fill.get("fillFps"), "firstFrameSeconds": fill.get("firstFrameSeconds"),
            "exportSecondsPerFrame": export.get("exportSecondsPerFrame")}


def compare(new, old, threshold):
    """Regressions of fill fps, first-frame time and export s/frame beyond `threshold` percent."""
    previous = {s["name"]: s["metrics"] for s in old.get("scenarios", [])}
    rows = []
    for scenario in new.get("scenarios", []):
        before = previous.get(scenario["name"])
        if not before:
            continue
        for metric, higher_better in COMPARED.items():
            a, b = scenario["metrics"].get(metric), before.get(metric)
            if a is None or b is None or b == 0:
                continue
            change = (a - b) / b * 100.0
            regressed = change < -threshold if higher_better else change > threshold
            rows.append({"scenario": scenario["name"], "metric": metric, "baseline": b,
                         "value": a, "changePercent": round(change, 1), "regressed": regressed})
    return rows


def write_markdown(path, result):
    lines = [f"# Scenario suite {result['startedUtc']}", "",
             f"Bloom {result['bloom'].get('commit')} (dirty: {result['bloom'].get('dirty')}), "
             f"{result['machine'].get('gpu', {}).get('name', 'no NVIDIA GPU')}, "
             f"{result['machine'].get('cpu')}", ""]
    for scenario in result["scenarios"]:
        metrics = scenario["metrics"]
        lines += [f"## {scenario['name']}", "", scenario["description"], "",
                  f"{scenario['build'].get('width')}x{scenario['build'].get('height')}, "
                  f"{scenario['build'].get('frames')} frames, {scenario['build'].get('nodes')} "
                  "nodes", "",
                  "| Metric | Value |", "|---|---|"]
        for key, value in metrics.items():
            lines.append(f"| {key} | {value} |")
        for row in scenario["measurements"]:
            if row["kind"] == "playback":
                lines.append(f"| playbackFps (cold) | {row.get('playbackFps')} |")
            if row["kind"] == "scrub":
                lines.append(f"| scrub median / max s | {row.get('scrubMedianSeconds')} / "
                             f"{row.get('scrubMaxSeconds')} |")
            if row["kind"] == "fill":
                lines.append(f"| cachedPlaybackFps | {row.get('cachedPlaybackFps')} |")
                lines.append(f"| fill GPU busy % | {row.get('gpuBusyPercent')} |")
        lines += ["", "| Check | Outcome | Detail |", "|---|---|---|"]
        for item in scenario["checks"]:
            detail = json.dumps(item["detail"], separators=(",", ":"))
            outcome = item["outcome"]
            if item.get("knownFailure"):
                outcome += (f" ({item['knownFailure']})" if outcome == "known-failure"
                            else f" (listed as known: {item['knownFailure']}; it passes now)")
            lines.append(f"| {item['check']} | {outcome} | `{detail[:400]}` |")
        lines.append("")
    if result.get("comparison"):
        lines += ["## Comparison", "", "| Scenario | Metric | Baseline | Value | Change % |",
                  "|---|---|---|---|---|"]
        for row in result["comparison"]:
            flag = " (regression)" if row["regressed"] else ""
            lines.append(f"| {row['scenario']} | {row['metric']} | {row['baseline']} | "
                         f"{row['value']} | {row['changePercent']}{flag} |")
    path.write_text("\n".join(lines) + "\n")


def command_run(args):
    if not sys.platform.startswith("linux"):
        # The instance's user folders (preferences, MCP token, caches) are isolated through the
        # XDG variables; macOS and Windows keep preferences elsewhere (the defaults database, the
        # registry), so the runner refuses rather than touch the user's own settings.
        raise SystemExit("the scenario suite isolates its Bloom instance on Linux only")
    build = Path(args.build_dir).resolve()
    args.bloom = Path(args.bloom or build / "apps/bloom/Bloom")
    args.cli = Path(args.cli or build / "apps/bloom-cli/bloom-cli")
    for binary in (args.bloom, args.cli):
        if not binary.is_file():
            raise SystemExit(f"missing {binary}; build Bloom and bloom-cli first")
    root = bench_media.media_root(args.media_root)
    manifest = bench_media.load_manifest(HERE / "media_manifest.json")
    work = Path(args.work_dir).resolve()
    args.scratch = work / "scratch"
    args.driver_cache = work / "driver-cache"
    for folder in (args.scratch, args.driver_cache, work / "results"):
        folder.mkdir(parents=True, exist_ok=True)
    names = args.scenario or list(scenario_spec.SCENARIOS)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    base = work / "results" / (f"{stamp}-scenarios" + (f"-{args.label}" if args.label else ""))
    result = {"startedUtc": stamp, "machine": bench.machine_info(),
              "bloom": bench.bloom_revision(build), "mediaRoot": str(root),
              "limits": {"claimFactor": args.claim_factor,
                         "gpuBusyPercent": args.gpu_busy_percent, "threshold": args.threshold},
              "scenarios": []}
    failed = False
    with open(base.with_suffix(".log"), "w", encoding="utf-8") as log:
        for name in names:
            scenario = scenario_spec.SCENARIOS[name]
            media = scenario_media(root, manifest, scenario["tier"], scenario_formats(scenario),
                                   args.frames or scenario["frames"])
            files = [p for _, entry in media.values() for p in bench.media_files(root, entry)]
            project = work / "projects" / name / f"{name}.bloom"
            print(f"[{name}] building", flush=True)
            entry = {"name": name, "description": scenario["description"], "measurements": []}
            try:
                built = build_scenario(args, name, scenario, media, project, log)
                entry["build"] = built
                shot = dict(built, project=project, audioExport=scenario.get("audioExport"),
                            knownFailures=scenario.get("knownFailures") or {},
                            keyedAlpha=scenario_spec.keyed_alpha(scenario))
                steps = [("fill", lambda: measure_fill(args, shot, files, log)),
                         ("playback", lambda: measure_playback(args, shot, files, log)),
                         ("scrub", lambda: measure_scrub(args, shot, files, log))]
                if not args.no_export:
                    steps.append(("export", lambda: measure_export(
                        args, shot, files, work / "exports" / name, log)))
                if shot["audioExport"] and not args.no_export:
                    steps.append(("audio-export", lambda: measure_audio_export(
                        args, shot, work / "exports" / name, log)))
                for kind, step in steps:
                    print(f"[{name}] {kind}", flush=True)
                    try:
                        entry["measurements"].append(step())
                    except Exception as error:  # record it and keep measuring the rest
                        entry["measurements"].append({"kind": kind, "error": repr(error)})
                export = next((r for r in entry["measurements"] if r["kind"] == "export"), None)
                if export and export.get("files"):
                    print(f"[{name}] parity", flush=True)
                    entry["measurements"].append(
                        measure_parity(args, shot, export, work / "reference" / name, log))
                entry["checks"] = evaluate(args, shot, entry["measurements"])
            except Exception as error:
                entry.setdefault("build", {})["error"] = repr(error)
                entry["checks"] = [check("built", False, repr(error))]
            entry["metrics"] = summary_metrics(entry["measurements"])
            failed = failed or bool(failing(entry["checks"]))
            for item in entry["checks"]:
                note = ""
                if item.get("knownFailure"):
                    note = (f" ({item['knownFailure']})" if item["outcome"] == "known-failure"
                            else f" (listed as known: {item['knownFailure']}; it passes now)")
                message = item["detail"].get("message") if isinstance(item["detail"],
                                                                      dict) else None
                print(f"[{name}] {item['check']}: {item['outcome']}{note}"
                      + (f": {message}" if message else ""), flush=True)
            result["scenarios"].append(entry)
            # Written after every scenario, so a long run keeps what it measured.
            base.with_suffix(".json").write_text(json.dumps(result, indent=1, default=str))
            if not args.keep_outputs:
                for folder in (work / "exports" / name, work / "reference" / name):
                    shutil.rmtree(folder, ignore_errors=True)
    if args.baseline:
        result["comparison"] = compare(result, json.loads(Path(args.baseline).read_text()),
                                       args.threshold)
        failed = failed or any(row["regressed"] for row in result["comparison"])
    base.with_suffix(".json").write_text(json.dumps(result, indent=1, default=str))
    write_markdown(base.with_suffix(".md"), result)
    print(f"results: {base.with_suffix('.json')}")
    return 1 if failed else 0


def command_compare(args):
    new, old = (json.loads(Path(p).read_text()) for p in (args.new, args.old))
    rows = compare(new, old, args.threshold)
    for row in rows:
        flag = "REGRESSION" if row["regressed"] else "ok"
        print(f"{row['scenario']:24} {row['metric']:22} {row['baseline']:>10} -> "
              f"{row['value']:>10} ({row['changePercent']:+.1f}%) {flag}")
    return 1 if any(row["regressed"] for row in rows) else 0


def command_list(_args):
    for name, scenario in scenario_spec.SCENARIOS.items():
        print(f"{name}: {scenario['tier']}, {scenario['frames']} frames. "
              f"{scenario['description']}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run")
    run.add_argument("--scenario", action="append", choices=sorted(scenario_spec.SCENARIOS))
    run.add_argument("--build-dir", default="build/pw-desktop")
    run.add_argument("--bloom", help="the Bloom application (default: <build>/apps/bloom/Bloom)")
    run.add_argument("--cli", help="bloom-cli (default: <build>/apps/bloom-cli/bloom-cli)")
    run.add_argument("--media-root")
    run.add_argument("--work-dir", default="bench-work/scenarios")
    run.add_argument("--label")
    run.add_argument("--frames", type=int, help="override every scenario's work area length")
    run.add_argument("--play-seconds", type=float, default=6.0)
    run.add_argument("--scrub", type=int, default=6)
    run.add_argument("--scrub-timeout", type=float, default=60.0)
    run.add_argument("--first-frame-timeout", type=float, default=120.0)
    run.add_argument("--fill-timeout", type=float, default=600.0)
    run.add_argument("--stall-seconds", type=float, default=120.0)
    run.add_argument("--export-timeout", type=float, default=900.0)
    run.add_argument("--reference-frames", type=int, default=1)
    run.add_argument("--export-frames", type=int, default=4)
    run.add_argument("--export-fill-factor", type=float, default=4.0)
    run.add_argument("--export-allowance", type=float, default=0.5)
    run.add_argument("--claim-capacity-percent", type=float, default=90.0)
    run.add_argument("--claim-capacity-seconds", type=float, default=60.0)
    run.add_argument("--claim-factor", type=float, default=3.0)
    run.add_argument("--gpu-busy-percent", type=float, default=50.0)
    run.add_argument("--no-export", action="store_true")
    run.add_argument("--keep-outputs", action="store_true")
    run.add_argument("--baseline")
    run.add_argument("--threshold", type=float, default=10.0)
    comparison = commands.add_parser("compare")
    comparison.add_argument("new")
    comparison.add_argument("old")
    comparison.add_argument("--threshold", type=float, default=10.0)
    commands.add_parser("list")
    args = parser.parse_args()
    sys.exit({"run": command_run, "compare": command_compare, "list": command_list}[
        args.command](args))


if __name__ == "__main__":
    main()
