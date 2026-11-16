#!/usr/bin/env python3
"""Reads and checks Bloom's changelog (CHANGELOG.md, the Keep a Changelog format).

    changelog.py notes --version V [--file CHANGELOG.md] --out FILE
        writes the body of V's section (without its heading line, trimmed, one final newline).
        This text is the release notes, used unchanged for the GitHub release, the update feed
        (feed.py payload --notes) and the appcasts (appcast.py --notes).
    changelog.py check --version V [--file CHANGELOG.md]
        checks the format and that V has a dated section. Each problem is printed with its line
        number and the rule it breaks; the exit status is 1 when there is any.

The rules: the title; an Unreleased section first; a dated section for V; versions in descending
order; only the six standard group headings; no empty group; every entry a list item; no entry
that reads like a commit subject; no issue or pull request numbers; a link reference for every
version.

This file never reads a private key. Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

GROUPS = ("Added", "Changed", "Deprecated", "Removed", "Fixed", "Security")
COMMIT_TYPES = ("feat", "fix", "perf", "refactor", "test", "docs", "build", "ci", "chore", "style",
                "revert")
SECTION = re.compile(r"^## \[([^\]]+)\](?: - (\S+))?\s*$")
LINK_REFERENCE = re.compile(r"^\[([^\]]+)\]:\s*(\S+)\s*$")
VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
                     r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$")
DATE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
COMMIT_SUBJECT = re.compile(r"^(?:%s)(?:\([^)]*\))?!?: " % "|".join(COMMIT_TYPES))
ISSUE_NUMBER = re.compile(r"(?<![\w&#])#[0-9]+\b")
DEFAULT_FILE = Path(__file__).resolve().parents[2] / "CHANGELOG.md"


def precedence(version: str) -> tuple:
    """A key that sorts versions by the Semantic Versioning rule (a release above its alphas)."""
    found = VERSION.match(version)
    if not found:
        raise ValueError(f"'{version}' is not a version such as 0.1.0 or 0.1.0-alpha.1")
    numbers = tuple(int(found.group(index)) for index in (1, 2, 3))
    if found.group(4) is None:
        return numbers, (1,)
    parts = tuple((0, int(part), "") if part.isdigit() else (1, 0, part)
                  for part in found.group(4).split("."))
    return numbers, (0, parts)


def sections(lines: list[str]) -> list[tuple[str, int, int]]:
    """Each `## [name]` section as (name, index of its heading line, index after its last line).
    A section ends at the next section heading or at the first link reference."""
    heads = [index for index, line in enumerate(lines) if SECTION.match(line)]
    found = []
    for position, head in enumerate(heads):
        end = heads[position + 1] if position + 1 < len(heads) else len(lines)
        for index in range(head + 1, end):
            if LINK_REFERENCE.match(lines[index]):
                end = index
                break
        found.append((SECTION.match(lines[head]).group(1), head, end))
    return found


def section(text: str, version: str) -> str:
    """The body of the section for `version`: trimmed, with one final newline."""
    lines = text.splitlines()
    for name, head, end in sections(lines):
        if name == version:
            body = "\n".join(lines[head + 1:end]).strip("\n")
            if not body.strip():
                raise ValueError(f"the section for {version} is empty")
            return body.rstrip() + "\n"
    raise ValueError(f"there is no section for {version}")


def problems(text: str, version: str) -> list[tuple[int, str, str]]:
    """What is wrong with the changelog, as (line number, rule, message); empty when it is right."""
    found: list[tuple[int, str, str]] = []

    def add(line: int, rule: str, message: str) -> None:
        found.append((line, rule, message))

    lines = text.splitlines()
    first = next((index for index, line in enumerate(lines) if line.strip()), None)
    if first is None or lines[first].rstrip() != "# Changelog":
        add((first or 0) + 1, "title", "the file must start with the title '# Changelog'")
    parts = sections(lines)
    if not parts or parts[0][0] != "Unreleased":
        add(parts[0][1] + 1 if parts else 1, "unreleased-first",
            "the first section must be '## [Unreleased]'")
    elif SECTION.match(lines[parts[0][1]]).group(2) is not None:
        add(parts[0][1] + 1, "unreleased-first", "the Unreleased section has no date")
    if not any(name == version for name, _, _ in parts):
        add(1, "version-section", f"there is no section for {version}")

    released: list[tuple[str, int]] = []
    for name, head, end in parts:
        date = SECTION.match(lines[head]).group(2)
        if name != "Unreleased":
            if not VERSION.match(name):
                add(head + 1, "version-section", f"'{name}' is not a version such as 0.1.0")
            else:
                released.append((name, head + 1))
            if date is None or not DATE.match(date):
                add(head + 1, "date", f"the section for {name} needs a date such as - 2026-01-31")
        body_lines = range(head + 1, end)
        group = None
        entries = 0
        for index in body_lines:
            line = lines[index]
            number = index + 1
            if line.startswith("#"):
                heading = re.match(r"^### (.+?)\s*$", line)
                if group is not None and entries == 0:
                    add(group[1], "group-empty", f"the group '{group[0]}' has no entry")
                if not heading or heading.group(1) not in GROUPS:
                    add(number, "group-heading",
                        f"'{line.strip()}' is not one of: " + ", ".join(GROUPS))
                    group = None
                else:
                    group = (heading.group(1), number)
                entries = 0
                continue
            if not line.strip():
                continue
            if group is not None:
                if line.startswith((" ", "\t")):
                    pass  # A nested entry or the continuation of one.
                elif re.match(r"^[-*] \S", line):
                    entries += 1
                    if COMMIT_SUBJECT.match(line[2:]):
                        add(number, "commit-subject",
                            "an entry must not read like a commit subject (type(scope): ...)")
                else:
                    add(number, "list-item", "every entry in a group must be a list item")
            if ISSUE_NUMBER.search(line):
                add(number, "issue-number", "write what changed, not an issue or pull request number")
        if group is not None and entries == 0:
            add(group[1], "group-empty", f"the group '{group[0]}' has no entry")

    for (name, number), (later, _) in zip(released[1:], released):
        if precedence(name) >= precedence(later):
            add(number, "order", f"{name} must come below {later}: newest version first")

    references = {}
    for index, line in enumerate(lines):
        match = LINK_REFERENCE.match(line)
        if match:
            references[match.group(1)] = (index + 1, match.group(2))
    for name, head, _ in parts:
        reference = references.get(name)
        if reference is None:
            add(head + 1, "link-reference", f"there is no link reference '[{name}]: URL'")
        elif not reference[1].startswith("https://"):
            add(reference[0], "link-reference", f"the link reference for {name} is not an HTTPS URL")
    return sorted(found)


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Reads and checks Bloom's changelog.")
    commands = parser.add_subparsers(dest="command", required=True)
    notes = commands.add_parser("notes")
    check = commands.add_parser("check")
    for command in (notes, check):
        command.add_argument("--version", required=True)
        command.add_argument("--file", type=Path, default=DEFAULT_FILE)
    notes.add_argument("--out", required=True, type=Path)
    options = parser.parse_args(arguments)
    try:
        text = options.file.read_text(encoding="utf-8")
        if options.command == "notes":
            options.out.write_bytes(section(text, options.version).encode("utf-8"))
            return 0
        found = problems(text, options.version)
    except (OSError, ValueError) as error:
        print(f"changelog.py: {error}", file=sys.stderr)
        return 1
    for line, rule, message in found:
        print(f"{options.file.name}:{line}: {rule}: {message}", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
