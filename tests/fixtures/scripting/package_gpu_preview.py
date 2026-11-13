"""Requires a packaged Bloom to show a Viewer frame that was evaluated on the GPU.

cmake/package/check_package.cmake (rule `gpu`) runs this script with the package's own bloom-cli
(`bloom-cli run package_gpu_preview.py`), so it runs in the carried interpreter and everything it
starts is the package's.

The script

  1. writes the check's own settings (BLOOM_PACKAGE_CHECK_PROFILE) so that the application starts
     its MCP server on a free loopback port when it launches;
  2. starts the packaged application without a window (Qt's offscreen platform);
  3. through that server, as an artist's agent would, adds a composition with one Solid layer and
     shows it in the Viewer;
  4. reads `query kind=render-route` until the Viewer displays a current frame, and requires that
     the frame's scene was evaluated on the GPU. A frame from the CPU reference path fails the
     check, with the fallback reasons the preview service counted;
  5. asks the application for a picture of its status bar as it is then
     (BLOOM_PACKAGE_CHECK_CAPTURE, apps/bloom/main.cpp), which the check prints so that a person
     can see the Render value the package shows. The verdict is the route's, not the picture's.

Nothing here reads a device itself: the route is the one the displayed frame recorded, which is
what the status bar shows as Render GPU or Render CPU.

Output (BLOOM_PACKAGE_CHECK_OUTPUT), one record per line:

  process application <number>   the process of the application
  route <json>                   the render-route answer the verdict was made from
  capture <file>                 the PNG of the status bar, taken after that answer
  done gpu                       the displayed frame was evaluated on the GPU
  error <text>                   something did not work; the check fails with this text
"""

import json
import os
import pathlib
import socket
import subprocess
import sys
import time

OUTPUT = os.environ["BLOOM_PACKAGE_CHECK_OUTPUT"]
APPLICATION = os.environ["BLOOM_PACKAGE_CHECK_APPLICATION"]
PROFILE = pathlib.Path(os.environ["BLOOM_PACKAGE_CHECK_PROFILE"])
# How long the application may take to start its server, and how long the Viewer may take to
# show a current frame of a one-layer composition (a first frame compiles its pipelines).
START_SECONDS = 180.0
FRAME_SECONDS = 240.0
# How long the application may take to write the picture of its status bar.
CAPTURE_SECONDS = 30.0


class CheckFailure(Exception):
    pass


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def data_directory():
    # src/platform/user_data_directory.cpp; the check points each of these under its own work
    # directory before it starts this script.
    if sys.platform == "win32":
        root = pathlib.Path(os.environ["LOCALAPPDATA"])
    elif sys.platform == "darwin":
        root = pathlib.Path(os.environ["HOME"]) / "Library" / "Application Support"
    elif os.environ.get("XDG_DATA_HOME"):
        root = pathlib.Path(os.environ["XDG_DATA_HOME"])
    else:
        root = pathlib.Path(os.environ["HOME"]) / ".local" / "share"
    return root / "Afterlab" / "Bloom"


def write_settings(port):
    # The check's settings are a file (src/ui/application_identity.cpp): organization
    # BloomPackageCheck, application Bloom.
    folder = PROFILE / "BloomPackageCheck"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "Bloom.ini").write_text(
        f"[mcp]\nport={port}\nautoStart=true\n", encoding="utf-8"
    )


class Server:
    """The application's MCP server over its loopback HTTP transport."""

    def __init__(self, port, token):
        self.port = port
        self.token = token
        self.session = None
        self.next_id = 0

    def post(self, message, timeout=120.0):
        payload = json.dumps(message).encode()
        lines = [
            "POST /mcp HTTP/1.1",
            f"Host: 127.0.0.1:{self.port}",
            f"Authorization: Bearer {self.token}",
            "Content-Type: application/json",
            "Accept: application/json, text/event-stream",
            f"Content-Length: {len(payload)}",
            "Connection: close",
        ]
        if self.session:
            lines.append(f"Mcp-Session-Id: {self.session}")
        request = ("\r\n".join(lines) + "\r\n\r\n").encode() + payload
        chunks = []
        with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as connection:
            connection.sendall(request)
            while True:
                chunk = connection.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                head, separator, body = b"".join(chunks).partition(b"\r\n\r\n")
                if not separator:
                    continue
                if head.startswith((b"HTTP/1.1 202", b"HTTP/1.1 204")):
                    break
                length = [
                    line
                    for line in head.split(b"\r\n")
                    if line.lower().startswith(b"content-length:")
                ]
                if length and len(body) >= int(length[0].split(b":")[1]):
                    break
        head, _, body = b"".join(chunks).partition(b"\r\n\r\n")
        lines = head.decode(errors="replace").split("\r\n")
        status = int(lines[0].split()[1]) if lines and lines[0] else 0
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(":")
            headers[name.strip().lower()] = value.strip()
        return status, headers, body

    def initialize(self):
        status, headers, body = self.post(
            {
                "jsonrpc": "2.0",
                "id": 0,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {"name": "package-check", "version": "1"},
                },
            }
        )
        if status != 200:
            raise CheckFailure(f"the application's server refused the session ({status}): {body!r}")
        self.session = headers["mcp-session-id"]
        self.post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def call(self, name, arguments):
        self.next_id += 1
        status, _, body = self.post(
            {
                "jsonrpc": "2.0",
                "id": self.next_id,
                "method": "tools/call",
                "params": {"name": name, "arguments": arguments},
            }
        )
        if status != 200:
            raise CheckFailure(f"{name} was answered with {status}: {body!r}")
        answer = json.loads(body)
        if "error" in answer:
            raise CheckFailure(f"{name} failed: {answer['error']}")
        result = answer["result"]
        if result.get("isError"):
            raise CheckFailure(f"{name} failed: {result}")
        return result["structuredContent"]

    def transact(self, op, arguments):
        revision = self.call("query", {"kind": "project"})["revision"]
        result = self.call(
            "transact",
            {"expectedRevision": revision, "operations": [{"op": op, "args": arguments}]},
        )
        if not result.get("succeeded"):
            raise CheckFailure(f"{op} was refused: {result}")
        return result


def wait_for_server(process, port):
    token_file = data_directory() / "mcp" / "token"
    started = time.monotonic()
    while time.monotonic() - started < START_SECONDS:
        if process.poll() is not None:
            raise CheckFailure(f"the application exited with {process.returncode} while starting")
        if token_file.is_file() and token_file.stat().st_size > 0:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                    return token_file.read_text(encoding="utf-8").strip()
            except OSError:
                pass
        time.sleep(0.25)
    raise CheckFailure(
        f"the application did not start its server within {int(START_SECONDS)} seconds"
    )


def displayed_route(server, process):
    started = time.monotonic()
    route = {}
    while time.monotonic() - started < FRAME_SECONDS:
        if process.poll() is not None:
            raise CheckFailure(
                f"the application exited with {process.returncode} while preparing the frame"
            )
        route = server.call("query", {"kind": "render-route"})
        if route.get("available") and route.get("hasFrame") and route.get("freshness") == "current":
            return route
        time.sleep(0.25)
    raise CheckFailure(
        f"the Viewer showed no current frame within {int(FRAME_SECONDS)} seconds: {route}"
    )


def status_bar_picture(process):
    picture = pathlib.Path(OUTPUT + ".status-bar.png")
    request = pathlib.Path(str(picture) + ".request")
    request.write_text("", encoding="utf-8")
    started = time.monotonic()
    while time.monotonic() - started < CAPTURE_SECONDS:
        if process.poll() is not None:
            raise CheckFailure(
                f"the application exited with {process.returncode} while its status bar was "
                "being captured"
            )
        if not request.exists() and picture.is_file():
            return picture
        time.sleep(0.1)
    raise CheckFailure(
        f"the application wrote no picture of its status bar within {int(CAPTURE_SECONDS)} seconds"
    )


def check(record):
    port = free_port()
    write_settings(port)
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment["BLOOM_PACKAGE_CHECK_CAPTURE"] = OUTPUT + ".status-bar.png"
    log = OUTPUT + ".application-log"
    with open(log, "wb") as output:
        process = subprocess.Popen(
            [APPLICATION],
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=environment,
        )
    record(f"process application {process.pid}")
    try:
        server = Server(port, wait_for_server(process, port))
        server.initialize()
        server.transact("bloom.composition.add", {"name": "Package check"})
        compositions = server.call("query", {"kind": "compositions"})["records"]
        composition = max(item["id"] for item in compositions)
        server.transact(
            "bloom.layer.add-solid",
            {"composition": composition, "name": "Solid", "color": [0.2, 0.5, 0.8, 1]},
        )
        shown = server.call("editor.reveal", {"composition": composition})
        if not shown.get("succeeded"):
            raise CheckFailure(f"the composition could not be shown in the Viewer: {shown}")
        route = displayed_route(server, process)
        record("route " + json.dumps(route, sort_keys=True))
        if route.get("scene") != "gpu":
            raise CheckFailure(
                "the Viewer's frame was evaluated on the CPU, not the GPU (provider "
                f"{route.get('provider')}; why: {route.get('why')}; CPU fallbacks: "
                f"{json.dumps(route.get('cpuFallbacks'), sort_keys=True)})"
            )
        record(f"capture {status_bar_picture(process)}")
        record("done gpu")
    finally:
        # Stopped, not asked to quit, so it saves no window state.
        process.kill()
        process.wait()


def main():
    with open(OUTPUT, "w", encoding="utf-8") as output:

        def record(line):
            # One record per line, and the check reads the lines as a CMake list.
            line = line.replace("\n", " ").replace(";", ",")
            output.write(line + "\n")
            output.flush()

        try:
            check(record)
        except CheckFailure as failure:
            record(f"error {failure}")
        except BaseException as error:  # noqa: BLE001 - every failure is reported, then raised
            record(f"error {type(error).__name__}: {error}")
            raise


main()
