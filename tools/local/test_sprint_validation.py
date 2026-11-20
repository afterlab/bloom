"""Small, toolchain-free checks for the local validation runner."""

from __future__ import annotations

from argparse import Namespace
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import sprint_validation as runner


class SprintValidationTests(unittest.TestCase):
    def test_source_identity_includes_all_worktree_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "config", "user.name", "Local Test"], check=True)
            subprocess.run(["git", "-C", str(root), "config", "user.email", "local@test.invalid"], check=True)
            tracked = root / "tracked.txt"
            tracked.write_text("base\n")
            subprocess.run(["git", "-C", str(root), "add", "tracked.txt"], check=True)
            subprocess.run(["git", "-C", str(root), "commit", "-qm", "test: fixture"], check=True)
            clean = runner.source_identity(root)
            self.assertFalse(clean["dirty"])
            subprocess.run(["git", "-C", str(root), "commit", "--allow-empty", "-qm", "test: new head"],
                           check=True)
            new_head = runner.source_identity(root)
            self.assertNotEqual(clean["head"], new_head["head"])
            self.assertEqual(clean["diffSha256"], new_head["diffSha256"])
            self.assertNotEqual(clean, new_head)
            tracked.write_text("changed\n")
            modified = runner.source_identity(root)
            self.assertTrue(modified["dirty"])
            self.assertNotEqual(modified["diffSha256"], clean["diffSha256"])
            subprocess.run(["git", "-C", str(root), "add", "tracked.txt"], check=True)
            staged = runner.source_identity(root)
            self.assertEqual(staged["diffSha256"], modified["diffSha256"])
            untracked = root / "new.txt"
            untracked.write_text("one\n")
            first = runner.source_identity(root)
            untracked.write_text("two\n")
            second = runner.source_identity(root)
            self.assertNotEqual(first["diffSha256"], second["diffSha256"])
            self.assertEqual(second["untrackedFiles"], 1)

    def test_lock_rejects_concurrent_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / "validation.lock"
            with runner.exclusive_lock(lock, 0):
                with self.assertRaisesRegex(runner.ValidationError, "busy"):
                    with runner.exclusive_lock(lock, 0):
                        pass
            with runner.exclusive_lock(lock, 0):
                pass

    def test_build_cache_must_match_checkout_and_capability(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            root = parent / "source"
            other = parent / "other"
            build = parent / "build"
            root.mkdir()
            other.mkdir()
            build.mkdir()
            cache = build / "CMakeCache.txt"
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={other}\nBUILD_TESTING:BOOL=ON\n"
                             "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n")
            with self.assertRaisesRegex(runner.ValidationError, "different source"):
                runner.check_cache(build, root, None, None, "RelWithDebInfo")
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={root}\nBUILD_TESTING:BOOL=ON\n"
                             "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
                             "BLOOM_BUILD_DESKTOP:BOOL=OFF\nBLOOM_RENDER_ENABLE_VULKAN:BOOL=OFF\n")
            with self.assertRaisesRegex(runner.ValidationError, "BLOOM_BUILD_DESKTOP"):
                runner.check_cache(build, root, "desktop", None, "RelWithDebInfo")
            with self.assertRaisesRegex(runner.ValidationError, "BLOOM_RENDER_ENABLE_VULKAN"):
                runner.check_cache(build, root, "native", None, "RelWithDebInfo")
            with self.assertRaisesRegex(runner.ValidationError, "CMAKE_BUILD_TYPE"):
                runner.check_cache(build, root, None, None, "Debug")
            cache.write_text(cache.read_text() + "CMAKE_CXX_COMPILER_LAUNCHER:STRING=sccache\n")
            with self.assertRaisesRegex(runner.ValidationError, "launcher"):
                runner.check_cache(build, root, None, None, "RelWithDebInfo")

    def test_local_environment_isolates_desktop(self):
        with patch.dict(os.environ, {"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-0",
                                      "QT_QPA_PLATFORM": "xcb", "QT_QPA_PLATFORMTHEME": "system",
                                      "QT_STYLE_OVERRIDE": "Windows"}):
            env = runner.local_environment(2, None)
        self.assertNotIn("DISPLAY", env)
        self.assertNotIn("WAYLAND_DISPLAY", env)
        self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")
        self.assertEqual(env["QT_QPA_PLATFORMTHEME"], "")
        self.assertEqual(env["QT_STYLE_OVERRIDE"], "Fusion")
        self.assertEqual(env["CMAKE_BUILD_PARALLEL_LEVEL"], "2")
        self.assertEqual(env["CCACHE_REMOTE_STORAGE"], "")
        self.assertEqual(env["CCACHE_PREFIX"], "")
        self.assertEqual(env["CCACHE_PREFIX_CPP"], "")

    def test_junit_skip_is_a_failure_even_when_ctest_exits_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            python = Path(sys.executable).as_posix()
            (root / "CTestTestfile.cmake").write_text(
                f'add_test(good "{python}" "-c" "pass")\n'
                f'add_test(skip "{python}" "-c" "import sys; sys.exit(77)")\n'
                'set_tests_properties(skip PROPERTIES SKIP_RETURN_CODE 77)\n')
            junit = root / "out.xml"
            result = subprocess.run(["ctest", "--test-dir", str(root), "--output-junit", str(junit)],
                                    capture_output=True, check=False)
            self.assertEqual(result.returncode, 0)
            evidence = runner.parse_junit(junit, {"good", "skip"})
            self.assertEqual(evidence["status"], "Failed")
            self.assertEqual(evidence["skipped"], ["skip"])

    def test_configuration_is_offline_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "prefix"
            prefix.mkdir()
            args = Namespace(dependency_mode="qualified", generator="Ninja", config="RelWithDebInfo",
                             level="native", python_executable=sys.executable)
            command = runner.configure_command(args, root, root / "build", prefix, "ccache")
            self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", command)
            self.assertIn("-DBLOOM_RENDER_ENABLE_VULKAN=ON", command)
            self.assertIn("-DBLOOM_DEPENDENCY_PREFIX=" + str(prefix), command)
            self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache", command)
            self.assertIn("-DPython3_EXECUTABLE=" + str(Path(sys.executable).resolve()), command)
            self.assertNotIn("--parallel", command)

    def test_command_failure_and_timeout_are_recorded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = {"commands": []}
            env = runner.local_environment(1, None)
            self.assertEqual(runner.execute(
                report, [sys.executable, "-c", "raise SystemExit(7)"], root, env,
                root / "failure.log", 5), 7)
            self.assertFalse(report["commands"][-1]["timedOut"])
            self.assertEqual(runner.execute(
                report, [sys.executable, "-c", "import time; time.sleep(30)"], root, env,
                root / "timeout.log", 1), 124)
            self.assertTrue(report["commands"][-1]["timedOut"])

    def test_unavailable_native_cannot_pass_on_zero_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = Namespace(level="native", jobs=1, config="RelWithDebInfo",
                             build_timeout_seconds=5, test_timeout_seconds=5)
            report = {"native": {"status": "NotRun"}}

            def unavailable(*unused_args):
                (root / "native-gpu.json").write_text('{"native": {"status": "NotRun"}}')
                return 0

            with patch.object(runner, "execute", side_effect=unavailable):
                with self.assertRaisesRegex(runner.ValidationError, "on hardware"):
                    runner.run_milestone(args, report, {}, root, root / "build")
            self.assertEqual(report["native"]["status"], "NotRun")

    def test_configure_registers_compositor_tests_on_request(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = Namespace(dependency_mode="developer-system", generator="Ninja",
                             config="RelWithDebInfo", level="desktop", python_executable=None,
                             compositor_tests=True)
            command = runner.configure_command(args, root, root / "build", None, None)
            self.assertIn("-DBLOOM_ENABLE_COMPOSITOR_TESTS=ON", command)
            args.compositor_tests = False
            command = runner.configure_command(args, root, root / "build", None, None)
            self.assertNotIn("-DBLOOM_ENABLE_COMPOSITOR_TESTS=ON", command)

    def test_compositor_label_is_split_from_the_offscreen_suite(self):
        build = Path("build")
        offscreen = runner.ctest_command(build, "Release", 2, Path("all.xml"), None,
                                         exclude_label=runner.COMPOSITOR_LABEL)
        self.assertEqual(offscreen[offscreen.index("-LE") + 1], "^compositor$")
        session = runner.ctest_command(build, "Release", 2, Path("c.xml"), None,
                                       label=runner.COMPOSITOR_LABEL)
        self.assertEqual(session[session.index("-L") + 1], "^compositor$")
        self.assertNotIn("-LE", session)

    def test_compositor_tests_without_a_session_are_not_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = Namespace(config="Release", jobs=1, test_timeout_seconds=5)
            report = {"commands": [], "tests": []}

            def never(*unused_args):
                raise AssertionError("no test may run without a compositor session")

            with patch.object(runner, "execute", side_effect=never):
                with self.assertRaises(runner.SessionUnavailable):
                    runner.run_compositor_tests(args, report, {}, root, root / "build",
                                                ["bloom.ui.example"],
                                                weston=str(root / "missing-weston"))
            self.assertEqual(report["compositor"]["status"], "NotRun")
            self.assertTrue(report["compositor"]["reason"])
            self.assertEqual(report["tests"], [])

    def test_a_run_whose_compositor_session_is_missing_is_not_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            build.mkdir()
            (build / "CMakeCache.txt").write_text(
                f"CMAKE_HOME_DIRECTORY:INTERNAL={runner.ROOT}\nBUILD_TESTING:BOOL=ON\n"
                "CMAKE_BUILD_TYPE:STRING=Release\nBLOOM_BUILD_DESKTOP:BOOL=ON\n"
                "BLOOM_DEPENDENCY_MODE:STRING=developer-system\n")
            output = root / "report.json"

            def unavailable(*unused_args):
                raise runner.SessionUnavailable("weston is not installed")

            with patch.object(runner, "run_milestone", side_effect=unavailable):
                code = runner.main(["milestone", "--level", "desktop", "--all", "--build-dir",
                                    str(build), "--config", "Release", "--dependency-mode",
                                    "developer-system", "--lock-file", str(root / "lock"),
                                    "--gpu-lock-held", "--output", str(output)])
            report = json.loads(output.read_text())
            self.assertNotEqual(code, 0)
            self.assertEqual(report["status"], "NotRun")
            self.assertIn("compositor session", report["error"])

    @unittest.skipUnless(sys.platform.startswith("linux"),
                         "the private compositor session is provided on Linux only")
    def test_private_session_is_its_own_and_ends_with_the_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = root / "weston-args.json"
            weston = root / "weston"
            # A stand-in that opens its socket the way Weston does and then runs until stopped.
            weston.write_text(
                f"#!{sys.executable}\n"
                "import json, os, pathlib, sys, time\n"
                "socket = next(a.split('=', 1)[1] for a in sys.argv if a.startswith('--socket='))\n"
                f"pathlib.Path({str(record)!r}).write_text(json.dumps("
                "{'argv': sys.argv[1:], 'pid': os.getpid(), "
                "'display': os.environ.get('DISPLAY'), "
                "'wayland': os.environ.get('WAYLAND_DISPLAY')}))\n"
                "(pathlib.Path(os.environ['XDG_RUNTIME_DIR']) / socket).touch()\n"
                "time.sleep(600)\n")
            weston.chmod(0o755)
            inherited = {"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-0", "PATH": os.environ["PATH"]}
            with runner.private_compositor_session(root, inherited, str(weston)) as env:
                runtime = Path(env["XDG_RUNTIME_DIR"])
                self.assertEqual(runtime.stat().st_mode & 0o777, 0o700)
                self.assertTrue((runtime / env["WAYLAND_DISPLAY"]).exists())
                self.assertNotEqual(env["WAYLAND_DISPLAY"], "wayland-0")
                self.assertNotIn("DISPLAY", env)
                self.assertEqual(env["QT_QPA_PLATFORM"], "wayland")
                started = json.loads(record.read_text())
            self.assertIn("--backend=headless", started["argv"])
            self.assertIsNone(started["display"])
            self.assertIsNone(started["wayland"])
            self.assertFalse(runtime.exists())
            with self.assertRaises(ProcessLookupError):
                os.kill(started["pid"], 0)

    def test_session_folder_fits_a_socket_address_under_a_long_temporary_folder(self):
        with tempfile.TemporaryDirectory() as directory:
            long = Path(directory) / ("x" * 120)
            long.mkdir()
            short = Path(directory) / "run"
            short.mkdir()
            socket = "bloom-123456"
            with patch.object(runner.tempfile, "gettempdir", return_value=str(long)):
                parent = runner.session_runtime_parent({"XDG_RUNTIME_DIR": str(short)}, socket)
            if len(str(short)) + runner.SESSION_FOLDER_LENGTH + 1 + len(socket) <= 107:
                self.assertEqual(parent, str(short))
            else:
                self.assertEqual(parent, "/tmp")
            self.assertLessEqual(len(parent) + runner.SESSION_FOLDER_LENGTH + 1 + len(socket), 107)

    @unittest.skipUnless(sys.platform.startswith("linux"),
                         "the private compositor session is provided on Linux only")
    def test_a_compositor_that_exits_at_once_is_no_session(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            weston = root / "weston"
            weston.write_text(f"#!{sys.executable}\nraise SystemExit(3)\n")
            weston.chmod(0o755)
            with self.assertRaisesRegex(runner.SessionUnavailable, "exited with 3"):
                with runner.private_compositor_session(root, {"PATH": os.environ["PATH"]},
                                                       str(weston)):
                    pass


if __name__ == "__main__":
    unittest.main()
