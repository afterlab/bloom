#!/usr/bin/env python3
"""Tests the release steps (tools/release/release.py) on a stand-in release of a few small files.

    python3 -I tools/release/test_release.py [CMAKE]

The signing runs the `openssl` program (OpenSSL 3) where the machine has it, and otherwise
Bloom's own build tool, named in BLOOM_ED25519_TOOL by the test's registration
(ed25519_route.py); the two generated files need `cmake`. Where the signing has no program or
`cmake` is missing the test reports a skip (exit status 77, the SKIP_RETURN_CODE of its
registration) and says why. The key is RFC 8032's first public test key, as in the other release tools' tests.
"""

from __future__ import annotations

import base64
import importlib.util
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

sys.dont_write_bytecode = True

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parents[1]
CMAKE = "cmake"
SPARKLE = "{http://www.andymatuschak.org/xml-namespaces/sparkle}"
BASE = "https://github.com/afterlab/bloom/releases/download/v0.1.0-alpha.1"
CHANNEL = "https://github.com/afterlab/bloom/releases/download/alpha-channel"


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


release = load("bloom_release_steps", TOOLS / "release.py")
feed = load("bloom_release_feed_for_steps", TOOLS / "feed.py")
fixtures = load("bloom_release_fixture_keys", TOOLS / "make_update_fixtures.py")
route = load("bloom_release_route_for_steps", TOOLS / "ed25519_route.py")


def step(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(TOOLS / "release.py"), *arguments],
                          capture_output=True, text=True, check=False)


class TagTests(unittest.TestCase):
    def test_the_tree_is_the_version_its_tag_names(self) -> None:
        declared = release.DEFAULT_VERSION.search(
            (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)
        result = step("version", "--tag", f"v{declared}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"version={declared}\n", result.stdout)
        self.assertRegex(result.stdout, r"date=\d{4}-\d{2}-\d{2}\n")

    def test_other_tags_are_refused(self) -> None:
        for tag in ("v9.9.9", "0.1.0-alpha.1", "v0.1.0-alpha", "release", "v0.1.0-alpha.1 "):
            result = step("version", "--tag", tag)
            self.assertEqual(result.returncode, 1, tag)
        self.assertIn("the source tree is", step("version", "--tag", "v9.9.9").stderr)
        self.assertIn("not a version tag", step("version", "--tag", "release").stderr)

    def test_a_rehearsal_takes_the_tree_s_version_and_names_its_tag(self) -> None:
        declared = release.DEFAULT_VERSION.search(
            (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)
        tagged = step("version", "--tag", f"v{declared}")
        rehearsed = step("version", "--rehearsal")
        self.assertEqual(rehearsed.returncode, 0, rehearsed.stderr)
        self.assertEqual(rehearsed.stdout, tagged.stdout)
        self.assertIn(f"tag=v{declared}\n", rehearsed.stdout)
        # One or the other, never both and never neither.
        self.assertEqual(step("version", "--rehearsal", "--tag", f"v{declared}").returncode, 2)
        self.assertEqual(step("version").returncode, 2)

    def test_a_rehearsal_of_a_tree_without_its_changelog_section_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            tree = Path(folder)
            (tree / "CMakeLists.txt").write_text(
                'set(BLOOM_PACKAGE_VERSION "9.9.9" CACHE STRING "The version")\n',
                encoding="utf-8")
            shutil.copy(ROOT / "CHANGELOG.md", tree / "CHANGELOG.md")
            result = step("version", "--rehearsal", "--source", str(tree))
            self.assertEqual(result.returncode, 1)
            self.assertIn("CHANGELOG.md", result.stderr)
            (tree / "CMakeLists.txt").write_text(
                'set(BLOOM_PACKAGE_VERSION "nightly" CACHE STRING "The version")\n',
                encoding="utf-8")
            result = step("version", "--rehearsal", "--source", str(tree))
            self.assertEqual(result.returncode, 1)
            self.assertIn("not a release version", result.stderr)


class ValidationTests(unittest.TestCase):
    CONTEXT = "bloom/hardware-validation"

    def check(self, statuses: list) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / "statuses.json"
            path.write_text(json.dumps(statuses), encoding="utf-8")
            return step("validation", "--statuses", str(path))

    def test_a_recorded_success_passes(self) -> None:
        result = self.check([{"context": "other", "state": "failure"},
                             {"context": self.CONTEXT, "state": "success"}])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_no_record_is_refused(self) -> None:
        for statuses in ([], [{"context": "ci", "state": "success"}]):
            result = self.check(statuses)
            self.assertEqual(result.returncode, 1)
            self.assertIn("was not recorded", result.stderr)

    def test_the_newest_record_decides(self) -> None:
        # GitHub lists the newest first: a failure recorded after a success refuses the commit,
        # and a success recorded after a failure passes it.
        result = self.check([{"context": self.CONTEXT, "state": "failure"},
                             {"context": self.CONTEXT, "state": "success"}])
        self.assertEqual(result.returncode, 1)
        self.assertIn("'failure', not 'success'", result.stderr)
        result = self.check([{"context": self.CONTEXT, "state": "success"},
                             {"context": self.CONTEXT, "state": "failure"}])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.check([{"context": self.CONTEXT, "state": "pending"}]).returncode, 1)

    def test_settings(self) -> None:
        key = base64.b64encode(bytes(32)).decode("ascii")
        good = step("settings", "--public-keys", f"{key},{key}", "--feed-url",
                    "https://updates.example.org/alpha.json")
        self.assertEqual(good.returncode, 0, good.stderr)
        for keys, url in ((key, "http://updates.example.org/alpha.json"),
                          ("AAAA", "https://updates.example.org/alpha.json"),
                          ("not base64", "https://updates.example.org/alpha.json")):
            self.assertEqual(step("settings", "--public-keys", keys, "--feed-url", url)
                             .returncode, 1, (keys, url))


class WorkflowTests(unittest.TestCase):
    """The rules the release workflow's text must keep (.github/workflows/release.yml)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        cls.code = "\n".join(line for line in cls.text.splitlines()
                             if not line.lstrip().startswith("#"))

    def job(self, name: str) -> str:
        found = re.search(rf"(?ms)^  {re.escape(name)}:\n(.*?)(?=^  [a-z-]+:\n|\Z)", self.code)
        self.assertIsNotNone(found, name)
        return found.group(1)

    def test_it_starts_for_a_created_tag_or_by_hand_only(self) -> None:
        triggers = re.search(r"(?ms)^on:\n(.*?)^\S", self.code).group(1)
        self.assertEqual(re.findall(r"(?m)^  ([a-z_]+):", triggers),
                         ["create", "workflow_dispatch"])
        # Every other job needs the first, which ends the run for anything but a version tag.
        gate = self.job("gate")
        self.assertIn("github.event.ref_type == 'tag' && startsWith(github.event.ref, 'v')", gate)
        for name in ("linux", "macos", "windows", "linux-start", "linux-desktop",
                     "macos-install", "macos-update", "windows-install", "candidate",
                     "publish"):
            self.assertRegex(self.job(name), r"needs: (gate|\[gate, )", name)
        # The captures only gather what three of those made.
        self.assertIn("needs: [linux-desktop, macos-install, windows-install]",
                      self.job("captures"))

    def test_every_action_is_pinned_to_a_commit(self) -> None:
        uses = re.findall(r"(?m)^\s+(?:- )?uses: (\S+)", self.code)
        self.assertTrue(uses)
        for action in uses:
            self.assertRegex(action, r"^actions/[a-z-]+@[0-9a-f]{40}$")

    def test_the_validation_record_comes_before_any_build(self) -> None:
        gate = self.job("gate")
        self.assertIn("release.py validation", gate)
        self.assertIn("statuses: read", gate)

    def test_only_the_last_job_waits_for_approval_and_reads_the_key(self) -> None:
        self.assertEqual(self.code.count("environment: release"), 1)
        self.assertEqual(self.code.count("secrets."), 1)
        self.assertEqual(self.code.count("contents: write"), 1)
        publish = self.job("publish")
        for text in ("environment: release", "secrets.BLOOM_UPDATE_SIGNING_KEY",
                     "contents: write", "gh release create"):
            self.assertIn(text, publish)
        self.assertEqual(self.code.count("gh release create"), 2)

    def test_a_rehearsal_is_a_start_by_hand_that_publishes_nothing(self) -> None:
        dispatch = re.search(r"(?ms)^  workflow_dispatch:\n(.*?)^\S", self.code).group(1)
        self.assertRegex(dispatch, r"(?m)^      rehearsal:\n(?:        .*\n)*?        type: boolean")
        self.assertRegex(dispatch, r"(?m)^      rehearsal:\n(?:        .*\n)*?        default: false")
        rehearsal = "github.event_name == 'workflow_dispatch' && inputs.rehearsal"
        # The job that signs and publishes does not run, and refuses if it ever did.
        publish = self.job("publish")
        self.assertIn("    if: github.event_name != 'workflow_dispatch' || !inputs.rehearsal\n",
                      publish)
        self.assertIn(f"BLOOM_REHEARSAL: ${{{{ {rehearsal} }}}}", publish)
        steps = re.findall(r"(?m)^      - name: (.+)$", publish)
        self.assertEqual(steps[0], "Refuse to publish a rehearsal")
        # Nothing outside that job touches a release, a tag or the channel's files.
        for word in ("gh release", "gh api -X", "git tag", "git push", "alpha-channel --"):
            self.assertEqual(self.code.replace(publish, "").count(word), 0, word)
        # Every job before it runs in a rehearsal: none of them is conditional on it.
        for name in ("linux", "linux-start", "linux-desktop", "macos", "macos-install",
                     "macos-update", "windows", "windows-install", "captures", "candidate"):
            self.assertNotRegex(self.job(name), r"(?m)^    if:", name)
        self.assertIn("needs: [gate, linux, linux-start, macos, macos-update, windows, captures]",
                      self.job("candidate"))

    def test_a_rehearsal_builds_the_commit_it_was_started_for(self) -> None:
        gate = self.job("gate")
        self.assertIn("release.py version --rehearsal", gate)
        self.assertIn("ref: ${{ env.BLOOM_REHEARSAL == 'true' && github.sha || "
                      "format('refs/tags/{0}', env.BLOOM_RELEASE_TAG) }}", gate)
        # The tag's name and its validation record are required of everything else.
        for step_name in ("Refuse a name that is not a version tag",
                          "Require the recorded hardware validation of the commit"):
            self.assertIn(f"      - name: {step_name}\n        if: env.BLOOM_REHEARSAL != 'true'\n",
                          gate)
        # The later jobs check out what the first job decided, and nothing else.
        refs = re.findall(r"(?m)^          ref: (.+)$", self.code.replace(gate, ""))
        self.assertEqual(len(refs), 9)
        checkouts = len(re.findall(r"uses: actions/checkout@", self.code.replace(gate, "")))
        self.assertEqual(checkouts, len(refs))
        self.assertEqual(set(refs), {"${{ needs.gate.outputs.ref }}"})
        # The key a rehearsal may build with is made and thrown away in one step.
        made = gate.index("openssl genpkey")
        self.assertIn('rm -f "$RUNNER_TEMP/rehearsal-key.pem"', gate[made:made + 400])
        self.assertEqual(self.code.count("vars."), 2)
        self.assertEqual(gate.count("vars."), 2)

    def test_the_installed_application_is_checked_and_captured(self) -> None:
        mac = self.job("macos-install")
        self.assertIn("tools/ci/macos-package/installed.sh", mac)
        self.assertIn("tools/ci/captures/macos.sh", mac)
        self.assertIn("tools\\ci\\captures\\windows.ps1", self.job("windows-install"))
        self.assertIn("tools/ci/captures/linux.sh", self.job("linux-desktop"))
        self.assertIn("name: desktop-captures", self.job("captures"))
        script = (ROOT / "tools" / "ci" / "macos-package" / "installed.sh").read_text(
            encoding="utf-8")
        for text in ('sudo installer -pkg "$pkg" -target /', "/Applications/Bloom.app",
                     "/usr/local/bin/bloom-cli", '"-DCLI_PROGRAM=$link"', "hdiutil attach"):
            self.assertIn(text, script)

    def test_the_macos_update_test_runs_after_the_upload_and_decides_the_run(self) -> None:
        mac = self.job("macos")
        upload = mac.index("name: files-macos")
        test = mac.index("tools/ci/macos-package/update-test.sh")
        self.assertLess(upload, test)
        # It may fail without ending the job, and its result is another job's whole purpose.
        self.assertIn("        id: update\n        continue-on-error: true\n", mac)
        self.assertIn("update: ${{ steps.update.outcome }}", mac)
        result = self.job("macos-update")
        self.assertIn("BLOOM_UPDATE_TEST: ${{ needs.macos.outputs.update }}", result)
        self.assertIn('if [ "$BLOOM_UPDATE_TEST" != success ]; then', result)
        script = (ROOT / "tools" / "ci" / "macos-package" / "update-test.sh").read_text(
            encoding="utf-8")
        for text in ("-DBLOOM_UPDATE_TEST_BUILD=ON", "--print-test-key", "update_e2e.py",
                     '--candidate "$candidate"'):
            self.assertIn(text, script)

    def test_the_windows_dependency_build_gets_msys2_s_own_programs(self) -> None:
        windows = self.job("windows")
        step = windows[windows.index("- name: Build dependency superbuild prefix"):]
        step = step[:step.index("\n      - name:", 10)]
        path = step.index('set "PATH=%RUNNER_TEMP%\\tools-venv\\Scripts;C:\\msys64\\usr\\bin;%PATH%"')
        environment = step.index("vcvars64.bat")
        checked = step.index('if /i not "%BLOOM_FIRST_EXPR%"=="C:\\msys64\\usr\\bin\\expr.exe"')
        built = step.index("cmake -S dependencies/superbuild")
        # MSYS2 before the machine's path, the compiler before MSYS2, both checked, then the build.
        self.assertLess(path, environment)
        self.assertLess(environment, checked)
        self.assertLess(checked, built)
        self.assertIn('findstr /i /c:"\\VC\\Tools\\MSVC\\"', step)
        self.assertIn('bash.exe -lc "pacman -S --noconfirm --needed make patch pkgconf perl"',
                      windows)

    def test_the_clean_start_covers_the_three_linux_forms(self) -> None:
        start = self.job("linux-start")
        for form in ("linux-x86_64.tar.xz", "linux-x86_64.AppImage", "start-release/install.sh"):
            self.assertIn(form, start)
        self.assertIn("linux-start", self.job("candidate"))


class ReleaseTests(unittest.TestCase):
    VERSION = "0.1.0-alpha.1"

    @classmethod
    def setUpClass(cls) -> None:
        cls.work = Path(tempfile.mkdtemp(prefix="bloom-release-steps-"))
        cls.key = fixtures.write_key(cls.work, 0)
        cls.other_key = fixtures.write_key(cls.work, 1)
        signer = [sys.executable, "-I", str(TOOLS / "sign_release"), "public-key", "--key-file"]
        cls.public = subprocess.run([*signer, str(cls.key)], capture_output=True, text=True,
                                    check=True).stdout.strip()
        cls.built = cls.work / "built"
        cls.built.mkdir()
        for index, name in enumerate(release.release_files(cls.VERSION)):
            if name.endswith(".zsync"):
                content = (b"zsync: 0.6.2\nFilename: " + name[:-6].encode() + b"\nURL: "
                           + name[:-6].encode() + b"\nSHA-1: 00\n\n\x00\x01\x02\n\n\x03")
            else:
                content = name.encode() * (index + 3)
            (cls.built / name).write_bytes(content)
        cls.candidate = cls.work / "candidate"
        cls.made = step("candidate", "--version", cls.VERSION, "--files", str(cls.built),
                        "--public-keys", cls.public, "--base-url", BASE, "--out",
                        str(cls.candidate), "--cmake", CMAKE)
        cls.out = cls.work / "published"
        cls.published = step("publish", "--version", cls.VERSION, "--date", "2026-11-17",
                             "--candidate", str(cls.candidate), "--public-keys", cls.public,
                             "--base-url", BASE, "--channel-url", CHANNEL, "--key-file",
                             str(cls.key), "--out", str(cls.out))

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.work, ignore_errors=True)

    def test_the_candidate_is_whole(self) -> None:
        self.assertEqual(self.made.returncode, 0, self.made.stderr)
        names = {path.name for path in self.candidate.iterdir()}
        self.assertEqual(names, {*release.release_files(self.VERSION), "release-notes.md",
                                 "install.sh", "PKGBUILD", ".SRCINFO", "SHA256SUMS"})
        sums = (self.candidate / "SHA256SUMS").read_text(encoding="ascii")
        for name in (*release.release_files(self.VERSION), "install.sh", "PKGBUILD"):
            self.assertIn(f"  {name}\n", sums)
        # The notes are the changelog's section, unchanged.
        changelog = load("bloom_release_changelog_for_steps", TOOLS / "changelog.py")
        self.assertEqual((self.candidate / "release-notes.md").read_text(encoding="utf-8"),
                         changelog.section((ROOT / "CHANGELOG.md").read_text(encoding="utf-8"),
                                           self.VERSION))
        script = (self.candidate / "install.sh").read_text(encoding="utf-8")
        self.assertIn(self.public, script)
        self.assertIn(BASE, script)

    def test_a_missing_file_stops_the_candidate(self) -> None:
        partial = self.work / "partial"
        shutil.copytree(self.built, partial)
        (partial / f"Bloom-{self.VERSION}-macos-arm64.pkg").unlink()
        result = step("candidate", "--version", self.VERSION, "--files", str(partial),
                      "--public-keys", self.public, "--base-url", BASE, "--out",
                      str(self.work / "partial-out"), "--cmake", CMAKE)
        self.assertEqual(result.returncode, 1)
        self.assertIn("macos-arm64.pkg", result.stderr)

    def test_the_published_release(self) -> None:
        self.assertEqual(self.published.returncode, 0, self.published.stderr)
        folder = self.out / "release"
        key = base64.b64decode(self.public)
        content = feed.read_feed((folder / "alpha.json").read_bytes(), [key])
        self.assertEqual((content["version"], content["channel"]), (self.VERSION, "alpha"))
        self.assertEqual(content["notes"],
                         (folder / "release-notes.md").read_text(encoding="utf-8"))
        listed = {}
        for platform, entry in content["platforms"].items():
            for item in entry["files"]:
                listed[item["url"].rsplit("/", 1)[1]] = (platform, item)
        self.assertEqual(set(listed), set(release.release_files(self.VERSION)))
        for name, (platform, item) in listed.items():
            data = (folder / name).read_bytes()
            self.assertEqual(item["url"], f"{BASE}/{name}")
            self.assertEqual(item["size"], len(data))
            self.assertEqual((platform, item["kind"]), release.release_files(self.VERSION)[name])
            self.assertTrue(feed.verify_signature(key, data,
                                                     base64.b64decode(item["signature"])), name)
        self.assertEqual(content["platforms"]["macos-arm64"]["appcast"],
                         f"{BASE}/appcast-macos.xml")
        self.assertEqual(content["platforms"]["windows-x86_64"]["appcast"],
                         f"{BASE}/appcast-windows.xml")
        # The checksum list's signature is the raw form the install script verifies.
        self.assertTrue(feed.verify_signature(key, (folder / "SHA256SUMS").read_bytes(),
                                                 (folder / "SHA256SUMS.sig").read_bytes()))
        # Only the checksum list's signature is a file of the release.
        self.assertEqual([path.name for path in folder.iterdir() if path.suffix == ".sig"],
                         ["SHA256SUMS.sig"])

    def test_the_appcasts_name_the_update_files(self) -> None:
        self.assertEqual(self.published.returncode, 0, self.published.stderr)
        folder = self.out / "release"
        expected = {"macos": f"Bloom-{self.VERSION}-macos-arm64.zip",
                    "windows": f"Bloom-{self.VERSION}-windows-x86_64-setup.exe"}
        for platform, name in expected.items():
            item = ET.parse(folder / f"appcast-{platform}.xml").getroot().find("channel/item")
            enclosure = item.find("enclosure")
            self.assertEqual(enclosure.get("url"), f"{BASE}/{name}")
            self.assertEqual(int(enclosure.get("length")), (folder / name).stat().st_size)
            self.assertEqual(item.find(f"{SPARKLE}version").text, "0.1.0a1")
        windows = ET.parse(folder / "appcast-windows.xml").getroot().find("channel/item")
        self.assertEqual(windows.find("enclosure").get(f"{SPARKLE}installerArguments"),
                         release.INSTALLER_ARGUMENTS)
        macos = ET.parse(folder / "appcast-macos.xml").getroot().find("channel/item")
        self.assertEqual(macos.find(f"{SPARKLE}minimumSystemVersion").text,
                         release.macos_minimum_system(ROOT))

    def test_the_channel_files(self) -> None:
        self.assertEqual(self.published.returncode, 0, self.published.stderr)
        channel = self.out / "channel"
        appimage = f"Bloom-{self.VERSION}-linux-x86_64.AppImage"
        self.assertEqual({path.name for path in channel.iterdir()},
                         {"alpha.json", "appcast-macos.xml", "appcast-windows.xml", "install.sh",
                          f"{appimage}.zsync"})
        for name in ("alpha.json", "appcast-macos.xml", "appcast-windows.xml", "install.sh"):
            self.assertEqual((channel / name).read_bytes(),
                             (self.out / "release" / name).read_bytes(), name)
        # The control file is the release's, with the full address of the AppImage: the two
        # files are in different releases.
        original = (self.out / "release" / f"{appimage}.zsync").read_bytes()
        changed = (channel / f"{appimage}.zsync").read_bytes()
        self.assertEqual(changed, original.replace(b"URL: " + appimage.encode(),
                                                   f"URL: {BASE}/{appimage}".encode()))
        self.assertNotEqual(changed, original)

    def test_another_key_is_refused(self) -> None:
        result = step("publish", "--version", self.VERSION, "--date", "2026-11-17",
                      "--candidate", str(self.candidate), "--public-keys", self.public,
                      "--base-url", BASE, "--channel-url", CHANNEL, "--key-file",
                      str(self.other_key), "--out", str(self.work / "other-key"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("not one of the release's public keys", result.stderr)
        self.assertFalse((self.work / "other-key" / "release" / "alpha.json").exists())

    def test_a_changed_candidate_is_refused(self) -> None:
        changed = self.work / "changed"
        shutil.copytree(self.candidate, changed)
        tar = changed / f"Bloom-{self.VERSION}-linux-x86_64.tar.xz"
        tar.write_bytes(tar.read_bytes() + b"!")
        result = step("publish", "--version", self.VERSION, "--date", "2026-11-17",
                      "--candidate", str(changed), "--public-keys", self.public, "--base-url",
                      BASE, "--channel-url", CHANNEL, "--key-file", str(self.key), "--out",
                      str(self.work / "changed-out"))
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.work / "changed-out" / "release" / "alpha.json").exists())


if __name__ == "__main__":
    if len(sys.argv) > 1:
        CMAKE = sys.argv.pop(1)
    try:
        route.route()
    except ValueError as error:
        print(f"NotRun: the release steps' test has {error}.", file=sys.stderr)
        sys.exit(77)
    if shutil.which(CMAKE) is None:
        print("NotRun: the release steps' test needs `cmake` for the generated install script "
              "and package description.", file=sys.stderr)
        sys.exit(77)
    unittest.main()
