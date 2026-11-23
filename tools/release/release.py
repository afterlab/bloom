#!/usr/bin/env python3
"""The steps of a Bloom release that are the same on every machine.

The release workflow (.github/workflows/release.yml) calls them in this order, and the end-to-end
update test (tools/release/update_e2e.py) calls `publish` to make a release it serves itself.

    release.py version (--tag TAG | --rehearsal) [--source DIR]
        checks that TAG is a version tag (v0.1.0-alpha.1), that the source tree is that version
        (the default of BLOOM_PACKAGE_VERSION in CMakeLists.txt) and that the changelog has a
        dated section for it. Prints `version=`, `date=` and `tag=` lines. A rehearsal has no
        tag yet: the version is the source tree's, the changelog is checked the same way, and
        the tag printed is the one that version will get.

    release.py validation --statuses FILE [--context NAME]
        FILE holds the commit's statuses as GitHub lists them (JSON, newest first). The newest
        status named NAME (default `bloom/hardware-validation`) must be `success`: a commit whose
        full validation on Linux, macOS and Windows hardware was not recorded is refused.

    release.py settings --public-keys KEYS --feed-url URL
        checks the two build settings of a release before anything is built.

    release.py candidate --version V --files DIR --public-keys KEYS --base-url URL --out DIR
                         [--source DIR] [--cmake PROGRAM]
        DIR holds the files the three platforms built. Checks that every release file is there,
        copies them to --out and adds the release notes (release-notes.md), the install script
        (install.sh), the Arch Linux package description (PKGBUILD and .SRCINFO) and SHA256SUMS.
        Nothing here needs the signing key.

    release.py publish --version V --date YYYY-MM-DD --candidate DIR --public-keys KEYS
                       --base-url URL --channel-url URL (--key-variable NAME | --key-file KEY)
                       --out DIR [--source DIR]
        signs the candidate with tools/release/sign_release (the one program that reads the key;
        this file only passes on where the key is) and writes into --out:
            release/   every file of the versioned release: the candidate's files,
                       SHA256SUMS.sig, the feed (alpha.json) and the two appcasts
            signatures/   the signature of each release file, which the feed and the appcasts
                       carry; these are not published as files
            channel/   the files of the fixed `alpha-channel` release: alpha.json, the appcasts,
                       install.sh and the AppImage's control file with the full address of the
                       AppImage
        The key must be one of KEYS, the feed must verify with it, and the install script's own
        check (`install.sh --from`) is not run here; the workflow runs it.

BASE-URL is the address the versioned release's files are downloaded from, CHANNEL-URL that of the
fixed release. Only the Python standard library is used, and `cmake` for the two generated files.
"""

from __future__ import annotations

import argparse
import base64
import importlib.util
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parents[1]
TAG = re.compile(r"^v(\d+\.\d+\.\d+(?:-(?:alpha|beta|rc)\.\d+)?)$")
DEFAULT_VERSION = re.compile(r'^set\(BLOOM_PACKAGE_VERSION "([^"]+)" CACHE STRING', re.MULTILINE)
VALIDATION_CONTEXT = "bloom/hardware-validation"
# How WinSparkle starts the Windows installer of an update (packaging/windows/bloom.iss, #788):
# its progress window only, titled "Updating Bloom", from Bloom's close until the new version
# opens (/SILENT); no question and no cancel (/SUPPRESSMSGBOXES, /NOCANCEL); it never closes a
# Bloom (/NOCLOSEAPPLICATIONS); and it starts the new version when it is done (/RELAUNCH=1).
INSTALLER_ARGUMENTS = ("/SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /NOCLOSEAPPLICATIONS "
                       "/RELAUNCH=1")


def load(name: str):
    spec = importlib.util.spec_from_file_location(f"bloom_release_{name}", TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(command: list[str]) -> None:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise ValueError(f"{Path(command[0]).name} failed: "
                         f"{(result.stdout + result.stderr).strip()}")


def tool(name: str, *arguments: str) -> None:
    run([sys.executable, "-I", str(TOOLS / name), *arguments])


def release_files(version: str) -> dict[str, tuple[str, str]]:
    """Every file the platforms build, as name -> (platform, the feed's kind for it)."""
    stem = f"Bloom-{version}"
    return {
        f"{stem}-linux-x86_64.tar.xz": ("linux-x86_64", "tar"),
        f"{stem}-linux-x86_64.AppImage": ("linux-x86_64", "appimage"),
        f"{stem}-linux-x86_64.AppImage.zsync": ("linux-x86_64", "zsync"),
        f"{stem}-macos-arm64.zip": ("macos-arm64", "zip"),
        f"{stem}-macos-arm64.dmg": ("macos-arm64", "dmg"),
        f"{stem}-macos-arm64.pkg": ("macos-arm64", "pkg"),
        f"{stem}-windows-x86_64-setup.exe": ("windows-x86_64", "installer"),
        f"{stem}-windows-x86_64.zip": ("windows-x86_64", "zip"),
    }


def parse_keys(text: str) -> list[bytes]:
    keys = []
    for entry in text.split(","):
        try:
            key = base64.b64decode(entry, validate=True)
        except ValueError as error:
            raise ValueError("the public keys are not Base64") from error
        if len(key) != 32:
            raise ValueError("a public key is 32 bytes")
        keys.append(key)
    if not keys:
        raise ValueError("there is no public key")
    return keys


def check_address(url: str, what: str) -> None:
    if not re.match(r"^https://[^/\s]+/\S*$", url):
        raise ValueError(f"{what} must be an HTTPS address")


def tag_version(tag: str | None, source: Path) -> tuple[str, str]:
    """The version and its changelog date; `tag` is None for a rehearsal, which has none yet."""
    declared = DEFAULT_VERSION.search((source / "CMakeLists.txt").read_text(encoding="utf-8"))
    if not declared:
        raise ValueError("CMakeLists.txt does not declare BLOOM_PACKAGE_VERSION")
    if tag is None:
        version = declared.group(1)
        if not TAG.match(f"v{version}"):
            raise ValueError(f"the source tree's version '{version}' is not a release version "
                             f"such as 0.1.0-alpha.1")
    else:
        found = TAG.match(tag)
        if not found:
            raise ValueError(f"'{tag}' is not a version tag such as v0.1.0-alpha.1")
        version = found.group(1)
        if declared.group(1) != version:
            raise ValueError(f"the tag is {version} and the source tree is {declared.group(1)}")
    changelog = load("changelog")
    text = (source / "CHANGELOG.md").read_text(encoding="utf-8")
    found_problems = changelog.problems(text, version)
    if found_problems:
        line, rule, message = found_problems[0]
        raise ValueError(f"CHANGELOG.md:{line}: {message} ({rule})")
    for name, head, _ in changelog.sections(text.splitlines()):
        if name == version:
            date = changelog.SECTION.match(text.splitlines()[head]).group(2)
            return version, date
    raise ValueError(f"CHANGELOG.md has no section for {version}")


def validated(statuses: list, context: str) -> None:
    """GitHub lists a commit's statuses newest first; the newest of the context decides."""
    for status in statuses:
        if isinstance(status, dict) and status.get("context") == context:
            if status.get("state") == "success":
                return
            raise ValueError(f"the newest '{context}' status of the commit is "
                             f"'{status.get('state')}', not 'success'")
    raise ValueError(f"the commit has no '{context}' status: its full validation on Linux, macOS "
                     "and Windows hardware was not recorded")


def macos_minimum_system(source: Path) -> str:
    lock = json.loads((source / "dependencies" / "dependencies.lock.json").read_text("utf-8"))
    for profile in lock.get("profiles", []):
        target = profile.get("consumerAbi", {}).get("appleDeploymentTarget")
        if target:
            return str(target)
    raise ValueError("the dependency lock names no macOS deployment target")


def make_candidate(version: str, files: Path, public_keys: str, base_url: str, out: Path,
                   source: Path, cmake: str) -> None:
    parse_keys(public_keys)
    check_address(base_url, "the release's address")
    names = release_files(version)
    missing = [name for name in names if not (files / name).is_file()]
    if missing:
        raise ValueError("the release lacks " + ", ".join(missing))
    out.mkdir(parents=True, exist_ok=True)
    for name in names:
        if (files / name).resolve() != (out / name).resolve():
            shutil.copyfile(files / name, out / name)
    tool("changelog.py", "notes", "--version", version, "--file", str(source / "CHANGELOG.md"),
         "--out", str(out / "release-notes.md"))
    run([cmake, f"-DVERSION={version}", f"-DBASE_URL={base_url}", f"-DPUBLIC_KEYS={public_keys}",
         f"-DOUTPUT={out / 'install.sh'}", "-P",
         str(source / "cmake" / "package" / "make_install_script.cmake")])
    sums = load("sums")
    tar = out / f"Bloom-{version}-linux-x86_64.tar.xz"
    run([cmake, f"-DVERSION={version}", f"-DSHA256={sums.sha256_of(tar)}", f"-DOUTPUT_DIR={out}",
         "-P", str(source / "cmake" / "package" / "make_pkgbuild.cmake")])
    listed = sorted([*names, "install.sh", "PKGBUILD"])
    tool("sums.py", "write", "--out", str(out / "SHA256SUMS"), *[str(out / name) for name in listed])


def channel_control_file(control: bytes, url: str) -> bytes:
    """The AppImage's control file with the full address of the AppImage in its URL line."""
    header, separator, blocks = control.partition(b"\n\n")
    if not separator:
        raise ValueError("the AppImage's control file has no header")
    lines = header.split(b"\n")
    places = [index for index, line in enumerate(lines) if line.startswith(b"URL: ")]
    if len(places) != 1:
        raise ValueError("the AppImage's control file does not name one address")
    lines[places[0]] = b"URL: " + url.encode("ascii")
    return b"\n".join(lines) + separator + blocks


def publish(version: str, date: str, candidate: Path, public_keys: str, base_url: str,
            channel_url: str, key: list[str], out: Path, source: Path) -> None:
    keys = parse_keys(public_keys)
    check_address(base_url, "the release's address")
    check_address(channel_url, "the channel's address")
    names = release_files(version)
    extras = ["release-notes.md", "install.sh", "PKGBUILD", ".SRCINFO", "SHA256SUMS"]
    missing = [name for name in [*names, *extras] if not (candidate / name).is_file()]
    if missing:
        raise ValueError("the candidate lacks " + ", ".join(missing))
    tool("sums.py", "check", "--sums", str(candidate / "SHA256SUMS"), "--folder", str(candidate))

    release = out / "release"
    channel = out / "channel"
    for folder in (release, channel):
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
    for name in [*names, *extras]:
        shutil.copyfile(candidate / name, release / name)
    shutil.copymode(candidate / "install.sh", release / "install.sh")

    signer = str(TOOLS / "sign_release")
    signed = subprocess.run([sys.executable, "-I", signer, "public-key", *key],
                            capture_output=True, text=True, check=False)
    if signed.returncode != 0:
        raise ValueError(f"sign_release failed: {(signed.stdout + signed.stderr).strip()}")
    signing_key = base64.b64decode(signed.stdout.strip(), validate=True)
    if signing_key not in keys:
        raise ValueError("the signing key is not one of the release's public keys: the builds "
                         "would refuse what it signs")

    tool("sign_release", "files", *key, *[str(release / name) for name in names])
    tool("sign_release", "detached", *key, "--out", str(release / "SHA256SUMS.sig"),
         str(release / "SHA256SUMS"))

    base = base_url.rstrip("/")
    notes = str(release / "release-notes.md")
    macos_zip = f"Bloom-{version}-macos-arm64.zip"
    windows_setup = f"Bloom-{version}-windows-x86_64-setup.exe"
    tool("appcast.py", "--platform", "macos", "--version", version, "--date", date, "--notes",
         notes, "--file", str(release / macos_zip), "--url", f"{base}/{macos_zip}",
         "--minimum-system-version", macos_minimum_system(source), "--out",
         str(release / "appcast-macos.xml"))
    tool("appcast.py", "--platform", "windows", "--version", version, "--date", date, "--notes",
         notes, "--file", str(release / windows_setup), "--url", f"{base}/{windows_setup}",
         "--installer-arguments", INSTALLER_ARGUMENTS, "--out",
         str(release / "appcast-windows.xml"))

    payload = out / "feed-payload.json"
    arguments = ["payload", "--version", version, "--date", date, "--notes", notes, "--out",
                 str(payload)]
    for name, (platform, kind) in names.items():
        arguments += ["--file", f"{platform}={kind}={release / name}={base}/{name}"]
    arguments += ["--appcast", f"macos-arm64={base}/appcast-macos.xml",
                  "--appcast", f"windows-x86_64={base}/appcast-windows.xml"]
    tool("feed.py", *arguments)
    tool("sign_release", "feed", *key, "--payload", str(payload), "--out",
         str(release / "alpha.json"))
    payload.unlink()
    tool("feed.py", "verify", "--feed", str(release / "alpha.json"), "--public-key",
         base64.b64encode(signing_key).decode("ascii"))
    # The feed and the appcasts carry each file's signature, so the signature files are not
    # files of the release. They are kept beside it.
    signatures = out / "signatures"
    if signatures.exists():
        shutil.rmtree(signatures)
    signatures.mkdir()
    for name in names:
        shutil.move(str(release / f"{name}.sig"), str(signatures / f"{name}.sig"))

    for name in ("alpha.json", "appcast-macos.xml", "appcast-windows.xml", "install.sh"):
        shutil.copyfile(release / name, channel / name)
    shutil.copymode(release / "install.sh", channel / "install.sh")
    appimage = f"Bloom-{version}-linux-x86_64.AppImage"
    control = (release / f"{appimage}.zsync").read_bytes()
    (channel / f"{appimage}.zsync").write_bytes(
        channel_control_file(control, f"{base}/{appimage}"))


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description="The steps of a Bloom release.")
    commands = parser.add_subparsers(dest="command", required=True)

    version = commands.add_parser("version")
    which = version.add_mutually_exclusive_group(required=True)
    which.add_argument("--tag")
    which.add_argument("--rehearsal", action="store_true")
    version.add_argument("--source", type=Path, default=ROOT)

    validation = commands.add_parser("validation")
    validation.add_argument("--statuses", required=True, type=Path)
    validation.add_argument("--context", default=VALIDATION_CONTEXT)

    settings = commands.add_parser("settings")
    settings.add_argument("--public-keys", required=True)
    settings.add_argument("--feed-url", required=True)

    candidate = commands.add_parser("candidate")
    candidate.add_argument("--version", required=True)
    candidate.add_argument("--files", required=True, type=Path)
    candidate.add_argument("--public-keys", required=True)
    candidate.add_argument("--base-url", required=True)
    candidate.add_argument("--out", required=True, type=Path)
    candidate.add_argument("--source", type=Path, default=ROOT)
    candidate.add_argument("--cmake", default="cmake")

    publication = commands.add_parser("publish")
    publication.add_argument("--version", required=True)
    publication.add_argument("--date", required=True)
    publication.add_argument("--candidate", required=True, type=Path)
    publication.add_argument("--public-keys", required=True)
    publication.add_argument("--base-url", required=True)
    publication.add_argument("--channel-url", required=True)
    key = publication.add_mutually_exclusive_group(required=True)
    key.add_argument("--key-variable")
    key.add_argument("--key-file")
    publication.add_argument("--out", required=True, type=Path)
    publication.add_argument("--source", type=Path, default=ROOT)

    options = parser.parse_args(arguments)
    try:
        if options.command == "version":
            label, date = tag_version(None if options.rehearsal else options.tag, options.source)
            print(f"version={label}")
            print(f"date={date}")
            print(f"tag=v{label}")
        elif options.command == "validation":
            statuses = json.loads(options.statuses.read_text(encoding="utf-8"))
            if not isinstance(statuses, list):
                raise ValueError("the statuses are not a list")
            validated(statuses, options.context)
            print(f"the commit has a successful '{options.context}' status")
        elif options.command == "settings":
            parse_keys(options.public_keys)
            check_address(options.feed_url, "the feed's address")
        elif options.command == "candidate":
            make_candidate(options.version, options.files, options.public_keys, options.base_url,
                           options.out, options.source, options.cmake)
        else:
            where = (["--key-variable", options.key_variable] if options.key_variable
                     else ["--key-file", options.key_file])
            publish(options.version, options.date, options.candidate, options.public_keys,
                    options.base_url, options.channel_url, where, options.out, options.source)
    except (OSError, ValueError) as error:
        print(f"release.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
