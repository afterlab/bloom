#!/usr/bin/env python3
"""The end-to-end update test of a release candidate, on the machine it runs on.

    update_e2e.py --installed FILE --candidate FILE --work DIR [--timeout SECONDS]
                  [--skip-rollback]

FILE is a release file of the platform this runs on:

    Bloom-<version>-linux-x86_64.tar.xz       a folder the update helper swaps
    Bloom-<version>-linux-x86_64.AppImage     one file the update helper replaces (with
                                              FILE.zsync beside the candidate, the update is
                                              a delta)
    Bloom-<version>-macos-arm64.zip           Bloom.app, replaced by the platform's updater
    Bloom-<version>-windows-x86_64-setup.exe  the installer, run by the platform's updater

--installed is a TEST BUILD of an older version (configured with -DBLOOM_UPDATE_TEST_BUILD=ON,
-DBLOOM_PACKAGE_VERSION=<older label> and, on macOS and Windows, the test key as
-DBLOOM_UPDATE_PUBLIC_KEYS, printed by `--print-test-key`). Only a test build reads its feed
address and key from the environment, and only a test build installs without a question.
--candidate is the release candidate, as the release workflow built it.

The test installs the test build under DIR, serves a release of the candidate from this
machine (signed with RFC 8032's public test key), starts the test build and waits until the
candidate has replaced it and confirmed its start. Then it installs the test build again and
serves a candidate that cannot start, and waits until the helper has put the test build back.
A line is printed for every step; the exit status is 0 when both parts passed.

Bloom runs without a window and under the package check's own identity
(BLOOM_PACKAGE_CHECK_PROFILE), so it never reads or writes the settings of the person whose
computer it is. What it cannot move is removed before and afterwards: on macOS the
BloomPackageCheck folders in ~/Library; on Windows the BloomPackageCheck folder in the local
application data folder, and the installation, which the test removes with the installer's own
uninstaller. The platform's updater on macOS keeps a few values of its own in the bundle
identifier's defaults, and those stay.

On macOS two things differ, because of the platform's updater. It accepts a new version only
when that version carries the key the installed one trusts, so the test puts its own public key
into the candidate's Info.plist and seals the bundle again; every program in it stays the
candidate's. And it would start the new version through the system's launcher, as the person at
the computer, so a test build tells it not to and the test starts the new version itself.

A candidate "that cannot start": on Linux and macOS its main program is replaced by one that
ends at once; on Windows the installer is the candidate's own and is started without the
argument that starts Bloom again, so the new version never confirms its start. On macOS and
Windows the helper waits 15 minutes for that confirmation before it puts the kept version back,
so the second part takes that long there.

Only the Python standard library is used, and the `openssl` program (OpenSSL 3) through
tools/release/sign_release.
"""

from __future__ import annotations

import argparse
import functools
import http.server
import importlib.util
import os
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
NAME = re.compile(r"^Bloom-(\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)-(linux|macos|windows)-")
SYSTEM = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}.get(platform.system(), "")
INSTALLER_ARGUMENTS = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"
PROFILE_ORGANIZATION = "BloomPackageCheck"
PROFILE_VARIABLE = "BLOOM_PACKAGE_CHECK_PROFILE"


def load(name: str):
    spec = importlib.util.spec_from_file_location(f"bloom_e2e_{name}", TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def say(text: str) -> None:
    print(f"e2e: {text}", flush=True)


def tool(name: str, *arguments: str) -> str:
    result = subprocess.run([sys.executable, "-I", str(TOOLS / name), *arguments],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"{name} failed: {(result.stdout + result.stderr).strip()}")
    return result.stdout


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    """Serves a folder, with the one-range requests a delta download makes."""

    def log_message(self, format: str, *arguments) -> None:  # noqa: A002
        # Kept for the report of a part that failed: what was asked for, and the answer.
        asked = getattr(self.server, "asked", None)
        if asked is not None:
            line = format % arguments
            asked[line] = asked.get(line, 0) + 1

    def send_head(self):
        wanted = self.headers.get("Range")
        path = Path(self.translate_path(self.path))
        found = re.match(r"^bytes=(\d+)-(\d*)$", wanted or "")
        if not found or not path.is_file():
            return super().send_head()
        size = path.stat().st_size
        first = int(found.group(1))
        last = min(int(found.group(2)) if found.group(2) else size - 1, size - 1)
        if first > last:
            self.send_error(416)
            return None
        self.range = (first, last)
        self.send_response(206)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Range", f"bytes {first}-{last}/{size}")
        self.send_header("Content-Length", str(last - first + 1))
        self.end_headers()
        return path.open("rb")

    def copyfile(self, source, outputfile) -> None:
        span = getattr(self, "range", None)
        if span is None:
            super().copyfile(source, outputfile)
            return
        source.seek(span[0])
        left = span[1] - span[0] + 1
        while left > 0:
            block = source.read(min(left, 1 << 16))
            if not block:
                break
            outputfile.write(block)
            left -= len(block)


class Served:
    """A folder served on this machine while the object lives."""

    def __init__(self, folder: Path) -> None:
        folder.mkdir(parents=True, exist_ok=True)
        self.folder = folder
        handler = functools.partial(RangeHandler, directory=str(folder))
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.server.asked = {}
        self.address = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


def same_bytes(file: Path, wanted: bytes) -> bool:
    try:
        return file.read_bytes() == wanted
    except OSError:
        return False


def version_of(file: Path) -> str:
    found = NAME.match(file.name)
    if not found:
        raise RuntimeError(f"{file.name} is not a release file's name")
    if found.group(2) != SYSTEM:
        raise RuntimeError(f"{file.name} is not a file of this system")
    return found.group(1)


def kind_of(file: Path) -> str:
    for suffix, kind in ((".tar.xz", "tar"), (".AppImage", "appimage"), (".zip", "zip"),
                         ("-setup.exe", "installer")):
        if file.name.endswith(suffix):
            return kind
    raise RuntimeError(f"{file.name} is not a file this test installs")


def profile_folders() -> list[Path]:
    """The per-user folders of the check's identity that the system does not let a test move:
    macOS keeps them in ~/Library, and Windows asks the system, not the environment, for the
    local application data folder."""
    if SYSTEM == "macos":
        return [Path.home() / "Library" / folder / PROFILE_ORGANIZATION
                for folder in ("Application Support", "Caches")]
    if SYSTEM == "windows" and os.environ.get("LOCALAPPDATA"):
        return [Path(os.environ["LOCALAPPDATA"]) / PROFILE_ORGANIZATION]
    return []


class Run:
    def __init__(self, options: argparse.Namespace) -> None:
        self.installed: Path = options.installed.resolve()
        self.candidate: Path = options.candidate.resolve()
        self.work: Path = options.work.resolve()
        self.timeout: int = options.timeout
        self.kind = kind_of(self.installed)
        if kind_of(self.candidate) != self.kind:
            raise RuntimeError("the installed file and the candidate are different kinds")
        self.old = version_of(self.installed)
        self.new = version_of(self.candidate)
        self.platform = {"linux": "linux-x86_64", "macos": "macos-arm64",
                         "windows": "windows-x86_64"}[SYSTEM]
        if self.work.exists():
            shutil.rmtree(self.work)
        self.work.mkdir(parents=True)
        fixtures = load("make_update_fixtures")
        self.key = fixtures.write_key(self.work, 0)
        self.public = tool("sign_release", "public-key", "--key-file", str(self.key)).strip()

    # --- the release this machine serves -----------------------------------------------------

    def keyed(self, name: str, file: Path) -> Path:
        """macOS: the candidate with this test's public key in its Info.plist, sealed again.

        The platform's updater accepts a new version only when it carries the key the installed
        version trusts (or is signed by the same developer identity, which a test has none of).
        The installed test build trusts this test's key, and a release candidate carries the
        release's, so the test puts its own key into the candidate. Every program in it stays
        the candidate's own."""
        folder = self.work / name / "keyed"
        subprocess.run(["ditto", "-x", "-k", str(file), str(folder / "tree")], check=True)
        bundle = folder / "tree" / "Bloom.app"
        plist = bundle / "Contents" / "Info.plist"
        with plist.open("rb") as stream:
            content = plistlib.load(stream)
        content["SUPublicEDKey"] = self.public
        with plist.open("wb") as stream:
            plistlib.dump(content, stream)
        subprocess.run(["codesign", "--force", "--sign", "-", str(bundle)], check=True)
        keyed = folder / file.name
        subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(bundle),
                        str(keyed)], check=True)
        return keyed

    def release(self, name: str, file: Path, installer_arguments: str) -> Served:
        served = Served(self.work / name / "served")
        if SYSTEM == "macos":
            file = self.keyed(name, file)
        target = served.folder / self.candidate.name
        shutil.copyfile(file, target)
        notes = served.folder / "notes.md"
        notes.write_text("### Added\n\n- The end-to-end update test's release.\n",
                         encoding="utf-8")
        tool("sign_release", "files", "--key-file", str(self.key), str(target))
        arguments = ["payload", "--version", self.new, "--date", "2026-01-01", "--notes",
                     str(notes), "--out", str(served.folder / "payload.json"), "--file",
                     f"{self.platform}={self.kind}={target}={served.address}/{target.name}"]
        control = file.with_name(file.name + ".zsync")
        if self.kind == "appimage" and control.is_file():
            copy = served.folder / control.name
            shutil.copyfile(control, copy)
            tool("sign_release", "files", "--key-file", str(self.key), str(copy))
            arguments += ["--file",
                          f"{self.platform}=zsync={copy}={served.address}/{copy.name}"]
        if self.kind in ("zip", "installer"):
            appcast = served.folder / "appcast.xml"
            extra = (["--installer-arguments", installer_arguments] if self.kind == "installer"
                     else [])
            tool("appcast.py", "--platform", SYSTEM, "--version", self.new, "--date",
                 "2026-01-01", "--notes", str(notes), "--file", str(target), "--url",
                 f"{served.address}/{target.name}", *extra, "--out", str(appcast))
            arguments += ["--appcast", f"{self.platform}={served.address}/appcast.xml"]
        tool("feed.py", *arguments)
        tool("sign_release", "feed", "--key-file", str(self.key), "--payload",
             str(served.folder / "payload.json"), "--out", str(served.folder / "alpha.json"))
        return served

    # --- installing ---------------------------------------------------------------------------

    def install(self, name: str) -> tuple[Path, Path]:
        """Installs the test build for one part. Returns (what is replaced, its program)."""
        place = self.work / name / "installed"
        place.mkdir(parents=True)
        if self.kind == "tar":
            with tarfile.open(self.installed) as archive:
                archive.extractall(place, filter="fully_trusted")
            root = next(place.iterdir())
            return root, root / "bin" / "Bloom"
        if self.kind == "appimage":
            root = place / "Bloom.AppImage"
            shutil.copyfile(self.installed, root)
            root.chmod(0o755)
            return root, root
        if self.kind == "zip":
            subprocess.run(["ditto", "-x", "-k", str(self.installed), str(place)], check=True)
            root = place / "Bloom.app"
            return root, root / "Contents" / "MacOS" / "Bloom"
        root = place / "Bloom"
        subprocess.run([str(self.installed), *INSTALLER_ARGUMENTS.split(), f"/DIR={root}"],
                       check=True)
        return root, root / "Bloom.exe"

    def remove(self, root: Path) -> None:
        if self.kind == "installer":
            uninstaller = root / "unins000.exe"
            if uninstaller.is_file():
                subprocess.run([str(uninstaller), *INSTALLER_ARGUMENTS.split()], check=False)

    def broken_candidate(self) -> tuple[Path, str]:
        """A candidate that cannot start, and the installer arguments of its release."""
        folder = self.work / "broken"
        folder.mkdir()
        broken = folder / self.candidate.name
        ends_at_once = "#!/bin/sh\nexit 1\n"
        if self.kind == "tar":
            with tarfile.open(self.candidate) as archive:
                archive.extractall(folder / "tree", filter="fully_trusted")
            root = next((folder / "tree").iterdir())
            (root / "bin" / "Bloom").write_text(ends_at_once, encoding="ascii")
            (root / "bin" / "Bloom").chmod(0o755)
            with tarfile.open(broken, "w:xz", preset=0) as archive:
                archive.add(root, arcname=root.name)
        elif self.kind == "appimage":
            broken.write_text(ends_at_once, encoding="ascii")
            broken.chmod(0o755)
        elif self.kind == "zip":
            subprocess.run(["ditto", "-x", "-k", str(self.candidate), str(folder / "tree")],
                           check=True)
            bundle = folder / "tree" / "Bloom.app"
            program = bundle / "Contents" / "MacOS" / "Bloom"
            program.write_text(ends_at_once, encoding="ascii")
            program.chmod(0o755)
            # The changed bundle is sealed again, or the updater refuses it before it installs.
            subprocess.run(["codesign", "--force", "--sign", "-", str(bundle)], check=True)
            subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(bundle),
                            str(broken)], check=True)
        else:
            # The candidate's own installer, started without the argument that starts Bloom.
            shutil.copyfile(self.candidate, broken)
            return broken, INSTALLER_ARGUMENTS
        return broken, INSTALLER_ARGUMENTS + " /RELAUNCH=1"

    # --- running ------------------------------------------------------------------------------

    def environment(self, name: str, served: Served) -> dict[str, str]:
        home = self.work / name / "home"
        profile = self.work / name / "profile"
        for folder in (home, profile):
            folder.mkdir(parents=True, exist_ok=True)
        environment = {
            PROFILE_VARIABLE: str(profile),
            "BLOOM_UPDATE_TEST_FEED_URL": f"{served.address}/alpha.json",
            "BLOOM_UPDATE_TEST_PUBLIC_KEYS": self.public,
            "BLOOM_UPDATE_TEST_INSTALL_NOW": "1",
            "QT_QPA_PLATFORM": "offscreen",
            # A Windows program without a console writes its messages to the debugger otherwise.
            "QT_FORCE_STDERR_LOGGING": "1",
        }
        if SYSTEM == "windows":
            for variable in ("SystemRoot", "SystemDrive", "TEMP", "TMP", "USERPROFILE",
                             "USERNAME", "ProgramData", "ComSpec", "PATHEXT"):
                if variable in os.environ:
                    environment[variable] = os.environ[variable]
            system_root = os.environ.get("SystemRoot", r"C:\Windows")
            environment["PATH"] = f"{system_root}\\System32;{system_root}"
            environment["LOCALAPPDATA"] = str(home)
            environment["APPDATA"] = str(home)
        else:
            environment["PATH"] = "/usr/bin:/bin"
            if SYSTEM == "linux":
                environment["HOME"] = str(home)
                for variable, folder in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                                         ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state")):
                    environment[variable] = str(home / folder)
                if self.kind == "appimage" and not shutil.which("fusermount3") \
                        and not shutil.which("fusermount"):
                    environment["APPIMAGE_EXTRACT_AND_RUN"] = "1"
            else:
                # macOS keeps the per-user folders where they are; the check's identity names them.
                environment["HOME"] = os.environ["HOME"]
        return environment

    def update_folder(self, name: str) -> Path | None:
        """The update folder of the run: where the plan, its journal and the start are written."""
        for root in [self.work / name / "home", *profile_folders()]:
            if root.is_dir():
                for found in root.rglob("updates"):
                    if found.is_dir():
                        return found
        return None

    def reported(self, program: Path) -> str:
        environment = dict(os.environ)
        for variable in ("DISPLAY", "WAYLAND_DISPLAY"):
            environment.pop(variable, None)
        if self.kind == "appimage" and not shutil.which("fusermount3") \
                and not shutil.which("fusermount"):
            environment["APPIMAGE_EXTRACT_AND_RUN"] = "1"
        try:
            result = subprocess.run([str(program), "--version"], capture_output=True, text=True,
                                    timeout=120, check=False, env=environment)
        except (OSError, subprocess.TimeoutExpired):
            return ""
        return result.stdout.strip().removeprefix("Bloom ").strip()

    def wait(self, what: str, condition) -> bool:
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            if condition():
                return True
            time.sleep(1)
        say(f"FAILED: {what} did not happen in {self.timeout} seconds")
        return False

    def processes(self) -> list[int]:
        """The processes this test's Bloom and its helper run as: those whose program file is
        under DIR (or in the check's own per-user folder, where the helper's copy is). On
        Linux also those started with this test's own settings folder: a program inside an
        AppImage runs from the image's mount, which is not under DIR."""
        found = []
        markers = [str(self.work) + os.sep] + [str(folder) + os.sep
                                               for folder in profile_folders()]
        if SYSTEM == "linux":
            profile = f"{PROFILE_VARIABLE}={self.work}{os.sep}".encode()
            for entry in Path("/proc").iterdir():
                if entry.name.isdigit() and int(entry.name) != os.getpid():
                    try:
                        program = os.readlink(entry / "exe")
                    except OSError:
                        continue
                    if any(program.startswith(marker) for marker in markers):
                        found.append(int(entry.name))
                        continue
                    try:
                        variables = (entry / "environ").read_bytes().split(b"\0")
                    except OSError:
                        continue
                    if any(variable.startswith(profile) for variable in variables):
                        found.append(int(entry.name))
        elif SYSTEM == "macos":
            listing = subprocess.run(["ps", "-axo", "pid=,comm="], capture_output=True,
                                     text=True, check=False).stdout
            for line in listing.splitlines():
                pid, _, program = line.strip().partition(" ")
                if pid.isdigit() and int(pid) != os.getpid() and any(
                        program.strip().startswith(marker) for marker in markers):
                    found.append(int(pid))
        else:
            listing = subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 "Get-CimInstance Win32_Process | ForEach-Object { \"$($_.ProcessId)|"
                 "$($_.ExecutablePath)\" }"], capture_output=True, text=True, check=False).stdout
            for line in listing.splitlines():
                pid, _, program = line.strip().partition("|")
                if pid.isdigit() and int(pid) != os.getpid() and any(
                        program.lower().startswith(marker.lower()) for marker in markers):
                    found.append(int(pid))
        return found

    def stop(self) -> None:
        for pid in self.processes():
            if SYSTEM == "windows":
                subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True,
                               check=False)
            else:
                try:
                    os.kill(pid, 15)
                except OSError:
                    pass
        deadline = time.monotonic() + 30
        while self.processes() and time.monotonic() < deadline:
            time.sleep(1)
        if SYSTEM != "windows":
            for pid in self.processes():
                try:
                    os.kill(pid, 9)
                except OSError:
                    pass

    def helper_log(self, name: str) -> str:
        """What the helper wrote when it ended: `updated: ...` or `restored: ...`. The journal
        beside it is cleared by the next start of Bloom, so the log is what is read."""
        folder = self.update_folder(name)
        try:
            return (folder / "plan.log").read_text("utf-8") if folder else ""
        except OSError:
            return ""

    def started(self, name: str) -> str:
        folder = self.update_folder(name)
        try:
            return (folder / "started").read_text("utf-8").strip() if folder else ""
        except OSError:
            return ""

    def explain(self, name: str, served: Served) -> None:
        """What a failed part left behind, for whoever reads the output on another machine."""
        asked = served.server.asked
        say(f"{name}: requests the release was asked: {len(asked) or 'none'}")
        for line, count in list(asked.items())[:40]:
            say(f"    {count} x {line}")
        output = self.work / name / "bloom-output.txt"
        try:
            lines = output.read_text("utf-8", errors="replace").splitlines()
        except OSError:
            lines = []
        say(f"{name}: Bloom's own output, last lines of {len(lines)}:")
        for line in lines[-60:]:
            say(f"    {line[:300]}")
        folder = self.update_folder(name)
        say(f"{name}: the update folder: {folder or 'none was made'}")
        if folder:
            for found in sorted(folder.rglob("*"))[:60]:
                if found.is_file():
                    say(f"    {found.relative_to(folder)} ({found.stat().st_size} bytes)")
            for small in ("plan.log", "started", "restored"):
                try:
                    say(f"    {small}: {(folder / small).read_text('utf-8').strip()[:400]}")
                except OSError:
                    pass
        say(f"{name}: processes of this test still running: {len(self.processes())}")
        if SYSTEM == "macos":
            # The platform's updater and its installer write to the system's log, not to Bloom's
            # output.
            wanted = ('subsystem BEGINSWITH "org.sparkle-project" '
                      'OR senderImagePath CONTAINS "Sparkle.framework" '
                      'OR senderImagePath ENDSWITH "/Autoupdate" '
                      'OR senderImagePath ENDSWITH "/Updater"')
            try:
                shown = subprocess.run(
                    ["log", "show", "--last", f"{self.timeout // 60 + 5}m", "--style", "compact",
                     "--info", "--predicate", wanted], capture_output=True, text=True,
                    timeout=300, check=False)
                lines = (shown.stdout + shown.stderr).splitlines()
            except (OSError, subprocess.TimeoutExpired) as error:
                lines = [f"the system's log could not be read: {error}"]
            say(f"{name}: the platform updater's lines in the system's log, last of {len(lines)}:")
            for line in lines[-120:]:
                say(f"    {line[:400]}")

    def part(self, name: str, file: Path, installer_arguments: str, expect_new: bool) -> bool:
        served = self.release(name, file, installer_arguments)
        try:
            passed = self.run_part(name, served, expect_new)
        except BaseException:
            self.explain(name, served)
            raise
        if not passed:
            self.explain(name, served)
        return passed

    def run_part(self, name: str, served: Served, expect_new: bool) -> bool:
        root, program = self.install(name)
        output = (self.work / name / "bloom-output.txt").open("wb")
        try:
            if self.reported(program) != self.old:
                say(f"FAILED: the installed test build does not report {self.old}")
                return False
            say(f"{name}: installed {self.old}; the release offers {self.new}")
            def start() -> None:
                subprocess.Popen([str(program)], env=self.environment(name, served),
                                 cwd=str(self.work / name), stdin=subprocess.DEVNULL,
                                 stdout=output, stderr=subprocess.STDOUT,
                                 start_new_session=SYSTEM != "windows")

            start()
            if SYSTEM == "macos":
                # The platform's updater would start the new version through the system's
                # launcher, as the person at the computer and with a window. A test build is
                # told not to (the driver's unattended run), and the test starts it here, under
                # its own identity: the installed program is the candidate's once the updater
                # has replaced the application.
                with zipfile.ZipFile(served.folder / self.candidate.name) as archive:
                    wanted = archive.read("Bloom.app/Contents/MacOS/Bloom")
                if same_bytes(program, wanted):
                    say("FAILED: the test build and the candidate have the same program")
                    return False
                if not self.wait("the platform's updater replacing the application",
                                 lambda: not self.processes() and same_bytes(program, wanted)):
                    return False
                say(f"{name}: the platform's updater replaced the application")
                start()
            if expect_new:
                if not self.wait("the candidate's confirmed start",
                                 lambda: self.started(name) == self.new):
                    return False
                say(f"{name}: {self.new} started and confirmed its start")
                if not self.wait("the helper's end", lambda: "updated:" in self.helper_log(name)):
                    return False
                self.stop()
                if self.reported(program) != self.new:
                    say(f"FAILED: the installation reports '{self.reported(program)}', "
                        f"not {self.new}")
                    return False
                say(f"{name}: the installation is {self.new}")
                return True
            if not self.wait("the helper putting the kept version back",
                             lambda: "restored:" in self.helper_log(name)):
                return False
            say(f"{name}: the helper put the kept version back")
            # The helper leaves a notice for the version it starts again, which takes it.
            folder = self.update_folder(name)
            if not self.wait(f"{self.old} starting again",
                             lambda: self.started(name) == self.old
                             and folder is not None and not (folder / "restored").exists()):
                return False
            self.stop()
            if self.reported(program) != self.old:
                say(f"FAILED: after going back the installation reports "
                    f"'{self.reported(program)}', not {self.old}")
                return False
            say(f"{name}: the installation is {self.old} again, and it started")
            return True
        finally:
            self.stop()
            output.close()
            say(f"{name}: the helper's log: {self.helper_log(name).strip() or '(none)'}")
            served.close()
            self.remove(root)

    def clean(self) -> None:
        for leftover in profile_folders():
            if leftover.is_dir():
                shutil.rmtree(leftover, ignore_errors=True)


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description="The end-to-end update test.")
    parser.add_argument("--installed", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--timeout", type=int, default=1500)
    parser.add_argument("--skip-rollback", action="store_true")
    parser.add_argument("--print-test-key", action="store_true")
    options = parser.parse_args(arguments)
    if options.print_test_key:
        import tempfile
        with tempfile.TemporaryDirectory() as work:
            key = load("make_update_fixtures").write_key(Path(work), 0)
            print(tool("sign_release", "public-key", "--key-file", str(key)).strip())
        return 0
    if not options.installed or not options.candidate or not options.work:
        parser.error("--installed, --candidate and --work are needed")
    if not SYSTEM:
        print("update_e2e.py: this system has no Bloom release", file=sys.stderr)
        return 2
    try:
        run = Run(options)
        passed = False
        try:
            say(f"{run.kind} on {run.platform}: {run.old} -> {run.new}")
            # Nothing of an earlier run may be read as this run's.
            run.clean()
            passed = run.part("update", run.candidate, INSTALLER_ARGUMENTS + " /RELAUNCH=1",
                              expect_new=True)
            if passed and not options.skip_rollback:
                run.clean()
                broken, installer_arguments = run.broken_candidate()
                passed = run.part("rollback", broken, installer_arguments, expect_new=False)
        finally:
            run.clean()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"update_e2e.py: {error}", file=sys.stderr)
        return 1
    say("PASSED" if passed else "FAILED")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
