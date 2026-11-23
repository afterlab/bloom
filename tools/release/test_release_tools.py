#!/usr/bin/env python3
"""Tests the release data tools in this folder: changelog.py, sums.py, appcast.py and the way they
work with feed.py and sign_release.

    python3 -I tools/release/test_release_tools.py

The signing runs the `openssl` program (OpenSSL 3) where the machine has it, and otherwise
Bloom's own build tool, which the test's registration names in BLOOM_ED25519_TOOL
(ed25519_route.py). With neither the test reports a skip (exit status 77, the SKIP_RETURN_CODE of
its registration) and says why. The keys are RFC 8032's public test keys.
"""

from __future__ import annotations

import base64
import contextlib
import importlib.util
import io
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
SPARKLE = "{http://www.andymatuschak.org/xml-namespaces/sparkle}"


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


feed = load("bloom_release_feed", TOOLS / "feed.py")
changelog = load("bloom_release_changelog", TOOLS / "changelog.py")
sums = load("bloom_release_sums", TOOLS / "sums.py")
appcast = load("bloom_release_appcast", TOOLS / "appcast.py")
fixtures = load("bloom_release_fixtures", TOOLS / "make_update_fixtures.py")
route = load("bloom_release_route", TOOLS / "ed25519_route.py")

VERSION = "0.1.0-alpha.1"
DATE = "2026-11-17"
GOOD = """# Changelog

All notable changes to Bloom are written here.

## [Unreleased]

## [0.1.0-alpha.1] - 2026-11-17

This is the first alpha. Expect rough edges.

### Added

- Text layers with `code` and **bold** words and a [link](https://example.invalid/page?a=1&b=2).
- Installers for every platform:
  - macOS: a disk image.
  - Windows: an installer
    that needs no administrator rights.
- A note with <angle brackets> & an ampersand.

### Fixed

- Playback no longer stutters.

## [0.0.9] - 2026-10-01

### Removed

- The old thing.

[Unreleased]: https://github.com/afterlab/bloom/compare/v0.1.0-alpha.1...HEAD
[0.1.0-alpha.1]: https://github.com/afterlab/bloom/compare/v0.0.9...v0.1.0-alpha.1
[0.0.9]: https://github.com/afterlab/bloom/releases/tag/v0.0.9
"""


def run(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", *arguments], capture_output=True, check=False)


def tool(*arguments: str) -> bytes:
    result = run(*arguments)
    if result.returncode != 0:
        raise AssertionError(f"{arguments[0]} {arguments[1]} failed: "
                             f"{result.stderr.decode('utf-8', 'replace')}")
    return result.stdout


class ChangelogTests(unittest.TestCase):
    def test_good_file_passes(self) -> None:
        self.assertEqual(changelog.problems(GOOD, VERSION), [])

    def test_repository_changelog_passes(self) -> None:
        text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertEqual(changelog.problems(text, VERSION), [])

    def test_each_rule_fails_by_name(self) -> None:
        cases = {
            "title": GOOD.replace("# Changelog", "# Changes", 1),
            "unreleased-first": GOOD.replace("## [Unreleased]\n\n", "", 1),
            "version-section": GOOD.replace("## [0.1.0-alpha.1] - 2026-11-17",
                                            "## [0.1.1] - 2026-11-17", 1),
            "date": GOOD.replace("- 2026-11-17", "- soon", 1),
            "order": GOOD.replace("## [0.0.9] - 2026-10-01", "## [0.2.0] - 2026-10-01", 1)
                         .replace("[0.0.9]:", "[0.2.0]:"),
            "group-heading": GOOD.replace("### Fixed", "### Improved", 1),
            "group-empty": GOOD.replace("- Playback no longer stutters.\n", "", 1),
            "list-item": GOOD.replace("- Playback no longer stutters.",
                                      "Playback no longer stutters.", 1),
            "commit-subject": GOOD.replace("- Playback no longer stutters.",
                                           "- fix(ui): playback no longer stutters.", 1),
            "issue-number": GOOD.replace("Playback no longer", "Playback (#123) no longer", 1),
            "link-reference": GOOD.replace("[0.0.9]: https://github.com/afterlab/bloom/releases"
                                           "/tag/v0.0.9\n", "", 1),
        }
        for rule, text in cases.items():
            with self.subTest(rule=rule):
                found = changelog.problems(text, VERSION)
                self.assertIn(rule, [item[1] for item in found], found)
                for number, name, _ in found:
                    self.assertGreaterEqual(number, 1)
                    self.assertTrue(name)

    def test_command_line_reports_line_and_rule(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / "CHANGELOG.md"
            path.write_text(GOOD.replace("Playback no longer", "Playback (#5) no longer"),
                            encoding="utf-8")
            result = run(str(TOOLS / "changelog.py"), "check", "--file", str(path),
                         "--version", VERSION)
            self.assertEqual(result.returncode, 1)
            self.assertRegex(result.stderr.decode(), r"CHANGELOG\.md:\d+: issue-number:")
            path.write_text(GOOD, encoding="utf-8")
            self.assertEqual(run(str(TOOLS / "changelog.py"), "check", "--file", str(path),
                                 "--version", VERSION).returncode, 0)

    def test_section_body(self) -> None:
        body = changelog.section(GOOD, VERSION)
        self.assertTrue(body.startswith("This is the first alpha."))
        self.assertTrue(body.endswith("- Playback no longer stutters.\n"))
        self.assertIsNone(re.search(r"^## ", body, re.MULTILINE))
        self.assertNotIn("[Unreleased]:", body)
        self.assertEqual(changelog.section(GOOD, "0.0.9").splitlines()[0], "### Removed")
        with self.assertRaises(ValueError):
            changelog.section(GOOD, "9.9.9")


class SumsTests(unittest.TestCase):
    def test_write_and_check(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            folder = Path(work)
            (folder / "b.bin").write_bytes(b"second")
            (folder / "a.bin").write_bytes(b"first")
            (folder / "c.bin").write_bytes(b"third")
            tool(str(TOOLS / "sums.py"), "write", "--out", str(folder / "SHA256SUMS"),
                 str(folder / "c.bin"), str(folder / "a.bin"), str(folder / "b.bin"))
            data = (folder / "SHA256SUMS").read_bytes()
            self.assertNotIn(b"\r", data)
            self.assertTrue(data.endswith(b"\n"))
            names = [line.split("  ", 1)[1] for line in data.decode().splitlines()]
            self.assertEqual(names, ["a.bin", "b.bin", "c.bin"])
            tool(str(TOOLS / "sums.py"), "check", "--sums", str(folder / "SHA256SUMS"),
                 "--folder", str(folder))
            tool(str(TOOLS / "sums.py"), "check", "--sums", str(folder / "SHA256SUMS"),
                 "--folder", str(folder), "b.bin")
            if shutil.which("sha256sum"):
                result = subprocess.run(["sha256sum", "-c", "SHA256SUMS"], cwd=folder,
                                        capture_output=True, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
            (folder / "b.bin").write_bytes(b"secone")
            result = run(str(TOOLS / "sums.py"), "check", "--sums", str(folder / "SHA256SUMS"),
                         "--folder", str(folder))
            self.assertEqual(result.returncode, 1)
            self.assertIn("b.bin", result.stderr.decode())
            self.assertEqual(run(str(TOOLS / "sums.py"), "check", "--sums",
                                 str(folder / "SHA256SUMS"), "--folder", str(folder),
                                 "a.bin").returncode, 0)

    def test_refuses_two_files_with_one_name(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            for folder in ("one", "two"):
                (Path(work) / folder).mkdir()
                (Path(work) / folder / "same.bin").write_bytes(b"x")
            result = run(str(TOOLS / "sums.py"), "write", "--out", str(Path(work) / "SHA256SUMS"),
                         str(Path(work) / "one" / "same.bin"), str(Path(work) / "two" / "same.bin"))
            self.assertEqual(result.returncode, 1)
            self.assertIn("same.bin", result.stderr.decode())


class VersionTests(unittest.TestCase):
    def test_compact_form(self) -> None:
        cases = {
            "0.1.0-alpha.1": "0.1.0a1", "0.1.0-alpha.12": "0.1.0a12", "1.20.3-beta.4": "1.20.3b4",
            "1.2.3-rc.5": "1.2.3rc5", "0.1.0": "0.1.0", "0.1.0-alpha": "0.1.0-alpha",
            "0.1.0-alpha.": "0.1.0-alpha.", "0.1.0-alpha.x": "0.1.0-alpha.x",
            "0.1.0-alpha.1.2": "0.1.0-alpha.1.2", "0.1.0-alpha.1+build": "0.1.0-alpha.1+build",
            "0.1.0-dev.3": "0.1.0-dev.3", "0.1.0+build": "0.1.0+build",
            "0.1-alpha.1": "0.1-alpha.1", "": "",
        }
        # The cases of the C++ test are the same mapping; keep this table equal to them.
        source = (ROOT / "src" / "core" / "tests" / "version_tests.cpp").read_text(encoding="utf-8")
        listed = dict(re.findall(r'\{"([^"]*)", "([^"]*)"\}', source))
        self.assertEqual(listed, cases)
        for label, compact in cases.items():
            self.assertEqual(appcast.build_version(label), compact, label)


class WindowsUpdateInstallerTests(unittest.TestCase):
    """#788: what the Windows installer and Bloom agree on while an update installs."""

    def setUp(self) -> None:
        self.script = (ROOT / "packaging" / "windows" / "bloom.iss").read_text(encoding="utf-8")
        self.header = (ROOT / "src" / "update" / "include" / "bloom" / "update" /
                       "installer_activity.hpp").read_text(encoding="utf-8")

    def define(self, name: str) -> str:
        found = re.search(rf'^\s*#define {name} "([^"]*)"', self.script, re.MULTILINE)
        self.assertIsNotNone(found, name)
        return found.group(1)

    def constant(self, name: str) -> str:
        found = re.search(rf'{name} = L?"([^"]*)"', self.header)
        self.assertIsNotNone(found, name)
        return found.group(1)

    def test_installer_arguments(self) -> None:
        release = load("bloom_release_release", TOOLS / "release.py")
        arguments = release.INSTALLER_ARGUMENTS.split()
        # A visible progress window and nothing else: no question, no cancel, no close of a Bloom.
        for wanted in ("/SILENT", "/SUPPRESSMSGBOXES", "/NOCANCEL", "/NORESTART",
                       "/NOCLOSEAPPLICATIONS", "/RELAUNCH=1"):
            self.assertIn(wanted, arguments)
        self.assertNotIn("/VERYSILENT", arguments)

    def test_bloom_and_the_installer_agree(self) -> None:
        self.assertEqual(self.define("SetupMutexName"), self.constant("kWindowsSetupMutex"))
        e2e = (TOOLS / "update_e2e.py").read_text(encoding="utf-8")
        self.assertIn(f'SETUP_MUTEX = "{self.constant("kWindowsSetupMutex")}"', e2e)
        self.assertIn("SetupMutex={#SetupMutexName}", self.script)
        started = self.define("StartedByInstaller")
        self.assertEqual(started, self.constant("kStartedByInstallerArgument"))
        # Every start of Bloom by the installer carries the argument, or it would end at once.
        runs = [line for line in self.script.splitlines()
                if line.startswith('Filename: "{app}\\Bloom.exe"')]
        self.assertEqual(len(runs), 2)
        for line in runs:
            self.assertIn('Parameters: "{#StartedByInstaller}"', line)
        self.assertIn("Exec(Folder + '\\Bloom.exe', '{#StartedByInstaller}'", self.script)
        # The window a Bloom opened during the install looks for.
        self.assertEqual(self.define("AppName"), "Bloom")
        self.assertIn("WizardForm.Caption := 'Updating {#AppName}'", self.script)
        self.assertEqual(self.constant("kUpdatingWindowTitle"), "Updating Bloom")
        self.assertEqual(self.constant("kSetupWindowTitle"), "Setup - Bloom")
        # The notice of an update that was not installed.
        self.assertIn(f"FailureNoticeName = '{self.constant('kInstallFailureNoticeName')}'",
                      self.script)

    def test_an_update_never_installs_without_the_kept_version(self) -> None:
        prepare = self.script[self.script.index("function PrepareToInstall"):]
        prepare = prepare[:prepare.index("\nend;\n") + 6]
        # In an update a folder that cannot be renamed stops the install before anything is
        # copied; only an install without /RELAUNCH=1 goes on over the folder.
        update_branch = prepare.index("if RelaunchRequested() then\n      begin")
        self.assertLess(update_branch, prepare.index("installing over it"))
        self.assertIn("Result := '{#AppName} was not updated: '", prepare)
        self.assertIn("KeptAside := True;", prepare)
        # A stopped update puts the kept version back, tells it and starts it.
        deinit = self.script[self.script.index("procedure DeinitializeSetup"):]
        for step in ("RenameFile(KeptFolder, Folder)", "SaveStringToFile(Folder + '\\' + "
                     "FailureNoticeName", "Exec(Folder + '\\Bloom.exe'"):
            self.assertIn(step, deinit)
        # The program is installed after the files beside it and before the folders.
        files = self.script[self.script.index("[Files]"):self.script.index("[Icons]")]
        self.assertLess(files.index('Excludes: "\\Bloom.exe"'),
                        files.index('Source: "{#SourceDir}\\Bloom.exe"'))
        self.assertLess(files.index('Source: "{#SourceDir}\\Bloom.exe"'), files.index("#sub AddFolder"))


class AppcastTests(unittest.TestCase):
    def test_notes_to_html(self) -> None:
        text = appcast.notes_to_html(changelog.section(GOOD, VERSION))
        self.assertIn("<p>This is the first alpha. Expect rough edges.</p>", text)
        self.assertIn("<h3>Added</h3>", text)
        self.assertIn("<code>code</code>", text)
        self.assertIn("<strong>bold</strong>", text)
        self.assertIn('<a href="https://example.invalid/page?a=1&amp;b=2">link</a>', text)
        self.assertIn("&lt;angle brackets&gt; &amp; an ampersand", text)
        self.assertIn("<li>macOS: a disk image.</li>", text)
        self.assertIn("an installer that needs no administrator rights.", text)
        self.assertEqual(text.count("<ul>"), text.count("</ul>"))
        self.assertEqual(text.count("<li>"), text.count("</li>"))
        ET.fromstring("<body>" + text + "</body>")
        self.assertIn("&lt;script&gt;", appcast.notes_to_html("- <script>alert(1)</script>"))
        self.assertNotIn("<a ", appcast.notes_to_html("[x](javascript:alert(1))"))

    def test_notes_document(self) -> None:
        """#787: the notes' page states its colors and a sans-serif system font, on the page and
        on its body, and its content is the notes unchanged."""
        notes = changelog.section(GOOD, VERSION)
        page = appcast.notes_document(notes)
        self.assertTrue(page.startswith("<!DOCTYPE html>"))
        style = re.search(r"<style>(.*)</style>", page).group(1)
        self.assertIn("color-scheme: light", style)
        self.assertIn("background: #ffffff", style)
        self.assertIn("color: #1f1f1f", style)
        for font in ('"Segoe UI"', "-apple-system", "sans-serif"):
            self.assertIn(font, style)
        self.assertRegex(style, r"font-family: [^;]*sans-serif;")
        self.assertRegex(style, r"code \{[^}]*background: #f0f0f0; color: #1f1f1f;")
        self.assertRegex(style, r"a \{ color: #[0-9a-f]{6}; \}")
        body = re.search(r'<body style="([^"]*)">\n(.*)\n</body></html>$', page, re.S)
        self.assertIsNotNone(body)
        self.assertIn("background: #ffffff;", body.group(1))
        self.assertIn("color: #1f1f1f;", body.group(1))
        self.assertIn("'Segoe UI'", body.group(1))
        self.assertIn("sans-serif", body.group(1))
        # The notes themselves are what notes_to_html makes, unchanged.
        self.assertEqual(body.group(2), appcast.notes_to_html(notes))
        # Dark text on a light background: the contrast ratio of the stated pair is high.
        def luminance(color: str) -> float:
            channels = [int(color[i:i + 2], 16) / 255 for i in (1, 3, 5)]
            linear = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
                      for c in channels]
            return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]
        ratio = ((luminance(appcast.NOTES_BACKGROUND) + 0.05) /
                 (luminance(appcast.NOTES_TEXT) + 0.05))
        self.assertGreater(ratio, 7.0)
        # The appcast's description is this page.
        text = appcast.appcast_text("windows", VERSION, DATE, notes, 1, "x",
                                    "https://example.invalid/a")
        item = ET.fromstring(text).find("./channel/item")
        self.assertEqual(item.findtext("description"), page)

    def test_date(self) -> None:
        self.assertEqual(appcast.rfc822("2026-11-17"), "Tue, 17 Nov 2026 00:00:00 +0000")
        self.assertEqual(appcast.rfc822("2026-01-05"), "Mon, 05 Jan 2026 00:00:00 +0000")

    def test_refuses_plain_http(self) -> None:
        with self.assertRaises(ValueError):
            appcast.appcast_text("macos", VERSION, DATE, "", 1, "x", "http://example.invalid/a")


class NoKeyTests(unittest.TestCase):
    TOOL_FILES = ("feed.py", "sums.py", "appcast.py", "changelog.py")

    def options_of(self, name: str) -> set[str]:
        """Every option string of every parser the tool builds when run with --help."""
        found: set[str] = set()
        module = {"feed.py": feed, "sums.py": sums, "appcast.py": appcast,
                  "changelog.py": changelog}[name]
        import argparse
        original = argparse.ArgumentParser.add_argument

        def record(parser, *names, **keywords):
            found.update(item for item in names if item.startswith("-"))
            return original(parser, *names, **keywords)

        argparse.ArgumentParser.add_argument = record
        try:
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                module.main(["--help"])
        finally:
            argparse.ArgumentParser.add_argument = original
        return found

    def test_no_tool_takes_a_key(self) -> None:
        for name in self.TOOL_FILES:
            options = self.options_of(name)
            self.assertTrue(options, name)
            for option in options:
                self.assertNotIn("key-file", option, name)
                self.assertNotIn("key-variable", option, name)
                self.assertNotIn("private", option, name)
            if name != "feed.py":
                self.assertFalse([item for item in options if "key" in item], (name, options))
        # feed.py verify takes the PUBLIC key; nothing else of it names a key.
        self.assertEqual({item for item in self.options_of("feed.py") if "key" in item},
                         {"--public-key"})

    def test_sources_never_read_a_private_key(self) -> None:
        for name in self.TOOL_FILES:
            source = (TOOLS / name).read_text(encoding="utf-8")
            self.assertNotIn("PRIVATE KEY", source.replace("never reads a private key", ""), name)
            self.assertNotIn("--key", source, name)
            self.assertNotIn("key-file", source, name)
            self.assertNotIn("os.environ", source, name)
        # `-inkey` is the way `openssl` is given a key. One file hands keys to a signing program
        # (ed25519_route.py): a private key file's name to sign with, which only sign_release
        # gives it, and a public key to check a signature with (with -pubin).
        mentions = {path.name for path in TOOLS.iterdir()
                    if path.is_file() and path.name != Path(__file__).name
                    and "-inkey" in path.read_text(encoding="utf-8", errors="ignore")}
        self.assertEqual(mentions, {"ed25519_route.py"})
        route_source = (TOOLS / "ed25519_route.py").read_text(encoding="utf-8")
        self.assertEqual(route_source.count("-inkey"), 2)
        self.assertIn('"-verify", "-pubin", "-inkey"', route_source)
        self.assertNotIn("PRIVATE KEY", route_source)
        # Two tools use that file: sign_release, for everything a private key does, and
        # feed.py, which only checks a signature with a public key.
        users = {path.name for path in TOOLS.iterdir()
                 if path.is_file() and not path.name.startswith("test_")
                 and path.name != "ed25519_route.py"
                 and "ed25519_route.py" in path.read_text(encoding="utf-8", errors="ignore")}
        self.assertEqual(users, {"sign_release", "feed.py"})
        feed_source = (TOOLS / "feed.py").read_text(encoding="utf-8")
        self.assertEqual(re.findall(r"load_route_module\(\)\.(\w+)", feed_source), ["verify"])


class FixtureTests(unittest.TestCase):
    def test_fixtures_are_reproduced_byte_for_byte(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            made = Path(work) / "fixtures"
            tool(str(TOOLS / "make_update_fixtures.py"), str(made))
            checked_in = ROOT / "src" / "update" / "tests" / "fixtures"
            expected = sorted(path.name for path in checked_in.iterdir() if path.is_file())
            self.assertEqual(sorted(path.name for path in made.iterdir()), expected)
            for name in expected:
                with self.subTest(name=name):
                    self.assertEqual((made / name).read_bytes(), (checked_in / name).read_bytes())


class ChainTests(unittest.TestCase):
    """The release data tools together, with the signing tool and a test key."""

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)
        self.keys = self.work / "keys"
        self.keys.mkdir()
        self.key = fixtures.write_key(self.keys, 0)
        self.release = self.work / "release"
        self.release.mkdir()
        self.files = {
            "linux-x86_64": ("tar", "Bloom-0.1.0-alpha.1-linux-x86_64.tar.xz", b"linux" * 1000),
            "macos-arm64": ("zip", "Bloom-0.1.0-alpha.1-macos-arm64.zip", b"macos" * 777),
            "windows-x86_64": ("installer", "Bloom-Setup-0.1.0-alpha.1-windows-x86_64.exe",
                               b"windows" * 333),
        }
        for _, name, data in self.files.values():
            (self.release / name).write_bytes(data)
        self.public = base64.b64decode(
            tool(str(TOOLS / "sign_release"), "public-key", "--key-file", str(self.key)).strip())
        self.base = "https://github.com/afterlab/bloom/releases/download/v0.1.0-alpha.1/"

    def sign(self, *arguments: str) -> None:
        tool(str(TOOLS / "sign_release"), *arguments[:1], "--key-file", str(self.key),
             *arguments[1:])

    def build(self) -> dict[str, Path]:
        release = self.release
        paths = [release / name for _, name, _ in self.files.values()]
        changes = self.work / "CHANGELOG.md"
        changes.write_text(GOOD, encoding="utf-8")
        notes = self.work / "notes.md"
        tool(str(TOOLS / "changelog.py"), "notes", "--version", VERSION, "--file", str(changes),
             "--out", str(notes))
        tool(str(TOOLS / "sums.py"), "write", "--out", str(release / "SHA256SUMS"), *map(str, paths))
        self.sign("detached", "--out", str(release / "SHA256SUMS.sig"), str(release / "SHA256SUMS"))
        self.sign("files", *map(str, paths))
        command = [str(TOOLS / "feed.py"), "payload", "--version", VERSION, "--date", DATE,
                   "--notes", str(notes), "--out", str(self.work / "payload.json")]
        for platform, (kind, name, _) in self.files.items():
            command += ["--file", f"{platform}={kind}={release / name}={self.base}{name}"]
        command += ["--appcast", f"macos-arm64={self.base}appcast-macos.xml",
                    "--appcast", f"windows-x86_64={self.base}appcast-windows.xml"]
        tool(*command)
        self.sign("feed", "--payload", str(self.work / "payload.json"), "--out",
                  str(release / "feed.json"))
        appcasts = {}
        for platform, key, arguments in (
                ("macos", "macos-arm64", ["--minimum-system-version", "13.3"]),
                ("windows", "windows-x86_64", ["--installer-arguments", "/SILENT /NORESTART"])):
            _, name, _ = self.files[key]
            appcasts[platform] = release / f"appcast-{platform}.xml"
            tool(str(TOOLS / "appcast.py"), "--platform", platform, "--version", VERSION,
                 "--date", DATE, "--notes", str(notes), "--file", str(release / name), "--url",
                 self.base + name, *arguments, "--out", str(appcasts[platform]))
        return {"notes": notes, **appcasts}

    def test_whole_chain(self) -> None:
        made = self.build()
        notes = made["notes"].read_text(encoding="utf-8")
        content = feed.read_feed((self.release / "feed.json").read_bytes(), [self.public])
        self.assertEqual(content["notes"], notes)
        self.assertEqual(content["version"], VERSION)
        self.assertEqual(content["channel"], "alpha")

        # SHA256SUMS: the signature checks with the public key, and its list matches the files.
        sums_bytes = (self.release / "SHA256SUMS").read_bytes()
        signature = (self.release / "SHA256SUMS.sig").read_bytes()
        self.assertEqual(len(signature), 64)
        self.assertTrue(feed.verify_signature(self.public, sums_bytes, signature))
        self.assertFalse(feed.verify_signature(self.public, sums_bytes + b" ", signature))
        tool(str(TOOLS / "sums.py"), "check", "--sums", str(self.release / "SHA256SUMS"),
             "--folder", str(self.release))

        html_notes = appcast.notes_to_html(notes)
        for platform, key in (("macos", "macos-arm64"), ("windows", "windows-x86_64")):
            _, name, data = self.files[key]
            root = ET.parse(made[platform]).getroot()
            self.assertEqual(root.tag, "rss")
            self.assertEqual(root.get("version"), "2.0")
            items = root.findall("./channel/item")
            self.assertEqual(len(items), 1)
            item = items[0]
            self.assertEqual(item.findtext("title"), f"Bloom {VERSION}")
            self.assertEqual(item.findtext("pubDate"), "Tue, 17 Nov 2026 00:00:00 +0000")
            self.assertEqual(item.findtext(SPARKLE + "version"), "0.1.0a1")
            self.assertEqual(item.findtext(SPARKLE + "shortVersionString"), VERSION)
            self.assertEqual(item.findtext("description"), appcast.notes_document(notes))
            self.assertIn(html_notes, item.findtext("description"))
            enclosure = item.find("enclosure")
            self.assertEqual(enclosure.get("url"), self.base + name)
            self.assertEqual(enclosure.get("length"), str(len(data)))
            self.assertEqual(enclosure.get("type"), "application/octet-stream")
            written = (self.release / (name + ".sig")).read_text(encoding="ascii").strip()
            self.assertEqual(enclosure.get(SPARKLE + "edSignature"), written)
            feed_entry = content["platforms"][key]["files"][0]
            self.assertEqual(feed_entry["signature"], written)
            self.assertEqual(content["platforms"][key]["appcast"],
                             f"{self.base}appcast-{platform}.xml")
            if platform == "macos":
                self.assertEqual(item.findtext(SPARKLE + "minimumSystemVersion"), "13.3")
                self.assertIsNone(enclosure.get(SPARKLE + "os"))
            else:
                self.assertEqual(enclosure.get(SPARKLE + "os"), "windows")
                self.assertEqual(enclosure.get(SPARKLE + "installerArguments"),
                                 "/SILENT /NORESTART")
                self.assertIsNone(item.find(SPARKLE + "minimumSystemVersion"))

    def test_both_signing_programs_agree(self) -> None:
        """The `openssl` program and Bloom's own tool give the same key and the same signature,
        and each accepts the other's. Skipped where the machine has only one of them."""
        programs = {"openssl": route.openssl_program(), "tool": route.built_tool()}
        if None in programs.values():
            self.skipTest("this machine has one signing program, not two: "
                          + ", ".join(sorted(kind for kind, program in programs.items()
                                             if program is not None)))
        message = self.work / "message"
        message.write_bytes(b"one message, two programs\n")
        routes = [(kind, program) for kind, program in programs.items()]
        keys = [route.public_key(self.key, chosen) for chosen in routes]
        signatures = [route.sign(self.key, message, chosen) for chosen in routes]
        self.assertEqual(keys[0], keys[1])
        self.assertEqual(keys[0], self.public)
        self.assertEqual(signatures[0], signatures[1])
        for chosen in routes:
            self.assertTrue(route.verify(self.public, message.read_bytes(), signatures[0], chosen))
            self.assertFalse(route.verify(self.public, b"another message\n", signatures[0],
                                          chosen))
            self.assertFalse(route.verify(self.public, message.read_bytes(),
                                          signatures[0][:-1], chosen))
        # An empty file is a message too.
        empty = self.work / "empty"
        empty.write_bytes(b"")
        self.assertEqual(route.sign(self.key, empty, routes[1]),
                         bytes.fromhex(fixtures.RFC_SIGNATURES[0]))

    def test_appcast_refuses_plain_http_and_a_missing_signature(self) -> None:
        name = self.files["macos-arm64"][1]
        notes = self.work / "notes.md"
        notes.write_text("### Added\n\n- A thing.\n", encoding="utf-8")
        base = [str(TOOLS / "appcast.py"), "--platform", "macos", "--version", VERSION,
                "--date", DATE, "--notes", str(notes), "--file", str(self.release / name),
                "--out", str(self.work / "appcast.xml")]
        self.assertEqual(run(*base, "--url", "https://example.invalid/a").returncode, 1)  # no .sig
        self.sign("files", str(self.release / name))
        self.assertEqual(run(*base, "--url", "http://example.invalid/a").returncode, 1)
        self.assertEqual(run(*base, "--url", "https://example.invalid/a").returncode, 0)

    def test_changed_bytes_are_refused(self) -> None:
        self.build()
        data = bytearray((self.release / self.files["macos-arm64"][1]).read_bytes())
        data[10] ^= 0x01
        (self.release / self.files["macos-arm64"][1]).write_bytes(bytes(data))
        result = run(str(TOOLS / "sums.py"), "check", "--sums", str(self.release / "SHA256SUMS"),
                     "--folder", str(self.release))
        self.assertEqual(result.returncode, 1)
        self.assertIn(self.files["macos-arm64"][1], result.stderr.decode())

        envelope = json.loads((self.release / "feed.json").read_text(encoding="ascii"))
        payload = bytearray(base64.b64decode(envelope["payload"]))
        position = payload.index(b"alpha")
        payload[position] ^= 0x20  # "alpha" becomes "Alpha": one changed byte.
        envelope["payload"] = base64.b64encode(bytes(payload)).decode("ascii")
        with self.assertRaises(ValueError):
            feed.read_feed(json.dumps(envelope).encode("utf-8"), [self.public])
        # The unchanged envelope is still accepted, so the refusal is the changed byte.
        self.assertTrue(feed.read_feed((self.release / "feed.json").read_bytes(), [self.public]))


if __name__ == "__main__":
    try:
        route.route()
    except ValueError as error:
        print(f"NotRun: the release tools' test has {error}.", file=sys.stderr)
        sys.exit(77)
    unittest.main()
