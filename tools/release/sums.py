#!/usr/bin/env python3
"""Writes and checks SHA256SUMS, the checksum list of a Bloom release.

    sums.py write --out SHA256SUMS FILE ...
        writes one line per file in the text form of `sha256sum`: the SHA-256 in hexadecimal, two
        spaces and the file's name without its folder. Lines are sorted by name and end with LF.
        Two files with one name are refused. `sha256sum -c SHA256SUMS` accepts the result.
    sums.py check --sums SHA256SUMS --folder DIR [FILE ...]
        checks that every listed file in DIR (or only each named FILE) has the listed SHA-256.
        The first file that does not match is named, and the exit status is 1.

The list is then signed by the maintainer (tools/release/sign_release detached). This file never
reads a private key. Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

LINE = re.compile(r"^([0-9a-f]{64}) [ *]([^\r\n]+)$")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def sums_text(paths: list[Path]) -> str:
    """The text of SHA256SUMS for the files."""
    entries: dict[str, str] = {}
    for path in paths:
        name = path.name
        if "\\" in name or "\n" in name or "\r" in name:
            raise ValueError(f"'{name}' cannot be written in a checksum list")
        if name in entries:
            raise ValueError(f"two files are named '{name}'")
        entries[name] = sha256_of(path)
    return "".join(f"{entries[name]}  {name}\n" for name in sorted(entries))


def read_sums(text: str) -> dict[str, str]:
    """The checksums of a SHA256SUMS text by file name."""
    entries: dict[str, str] = {}
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        match = LINE.match(line)
        if not match:
            raise ValueError(f"line {number} is not '<sha256>  <name>'")
        if match.group(2) in entries:
            raise ValueError(f"line {number}: '{match.group(2)}' is listed twice")
        entries[match.group(2)] = match.group(1)
    return entries


def check(text: str, folder: Path, names: list[str]) -> None:
    """Raises ValueError naming the first file that is missing or does not match."""
    entries = read_sums(text)
    for name in names or sorted(entries):
        if name not in entries:
            raise ValueError(f"'{name}' is not in the checksum list")
        path = folder / name
        if not path.is_file():
            raise ValueError(f"'{name}' is missing from {folder}")
        if sha256_of(path) != entries[name]:
            raise ValueError(f"'{name}' does not match its checksum")


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Writes and checks SHA256SUMS.")
    commands = parser.add_subparsers(dest="command", required=True)
    write = commands.add_parser("write")
    write.add_argument("--out", required=True, type=Path)
    write.add_argument("files", nargs="+", type=Path)
    verify = commands.add_parser("check")
    verify.add_argument("--sums", required=True, type=Path)
    verify.add_argument("--folder", required=True, type=Path)
    verify.add_argument("files", nargs="*")
    options = parser.parse_args(arguments)
    try:
        if options.command == "write":
            options.out.write_bytes(sums_text(options.files).encode("utf-8"))
        else:
            check(options.sums.read_text(encoding="utf-8"), options.folder, options.files)
            print(f"{options.sums}: every file matches")
    except (OSError, ValueError) as error:
        print(f"sums.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
