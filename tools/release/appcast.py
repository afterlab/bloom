#!/usr/bin/env python3
"""Writes the appcast the macOS and Windows updaters read (Sparkle and WinSparkle).

Both read the Sparkle appcast form: one RSS 2.0 channel with one item for the release.

    appcast.py --platform (macos|windows) --version V --date YYYY-MM-DD --notes FILE
               --file PATH --url URL [--minimum-system-version X]
               [--installer-arguments "..."] --out FILE

    --notes FILE   the release notes (tools/release/changelog.py notes), Markdown; the item's
                   description is the same text as small HTML.
    --file PATH    the update file (the macOS zip, the Windows installer). Its size is the
                   enclosure length, and PATH.sig (tools/release/sign_release files) holds the
                   Ed25519 signature the updater checks.
    --url URL      where the updater downloads PATH; it must be an HTTPS address.
    --minimum-system-version X   macOS only: the oldest system the update runs on.
    --installer-arguments "..."  Windows only: arguments the installer is started with.

sparkle:version is the compact form of V (0.1.0a1 for 0.1.0-alpha.1; the same mapping as
bloom_build_version in cmake/BloomVersion.cmake), which those updaters rank below 0.1.0;
sparkle:shortVersionString is V.

The notes use a small part of Markdown: ### headings, list items (nested by indentation),
paragraphs, `code`, **bold** and [links](https://...). Anything else is shown as text.

This file never reads a private key. Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import base64
import datetime
import html
import re
import sys
from pathlib import Path

NAMESPACE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
PLATFORMS = ("macos", "windows")
DAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
PRE_RELEASE = re.compile(r"^([0-9]+\.[0-9]+\.[0-9]+)-(alpha|beta|rc)\.([0-9]+)$")
SHORT = {"alpha": "a", "beta": "b", "rc": "rc"}


def build_version(label: str) -> str:
    """0.1.0-alpha.1 -> 0.1.0a1, -beta.N -> bN, -rc.N -> rcN; any other label is unchanged."""
    found = PRE_RELEASE.match(label)
    return f"{found.group(1)}{SHORT[found.group(2)]}{found.group(3)}" if found else label


def rfc822(date: str) -> str:
    """`2026-11-17` as `Tue, 17 Nov 2026 00:00:00 +0000`."""
    day = datetime.date.fromisoformat(date)
    return (f"{DAYS[day.weekday()]}, {day.day:02d} {MONTHS[day.month - 1]} {day.year} "
            "00:00:00 +0000")


def inline(text: str) -> str:
    """One line of notes as HTML: `code`, **bold** and [links](https://...); the rest is text."""
    out = []
    for index, piece in enumerate(re.split(r"`([^`]+)`", text)):
        if index % 2:
            out.append(f"<code>{html.escape(piece)}</code>")
            continue
        piece = html.escape(piece)
        piece = re.sub(r"\[([^\]]+)\]\((https?://[^\s)]+)\)", r'<a href="\2">\1</a>', piece)
        out.append(re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", piece))
    return "".join(out)


def notes_to_html(notes: str) -> str:
    """The notes' Markdown as small HTML: h3, p, ul, li, code, strong and a."""
    out: list[str] = []
    paragraph: list[str] = []
    levels: list[int] = []  # The indentation of each open list.

    def close_lists(down_to: int = 0) -> None:
        while len(levels) > down_to:
            out.append("</li></ul>")
            levels.pop()

    def end_paragraph() -> None:
        if paragraph:
            out.append("<p>" + inline(" ".join(paragraph)) + "</p>")
            paragraph.clear()

    for raw in notes.splitlines():
        line = raw.rstrip()
        item = re.match(r"^( *)[-*] (.*)$", line)
        heading = re.match(r"^### (.+)$", line)
        if not line.strip():
            end_paragraph()
            close_lists()
        elif heading:
            end_paragraph()
            close_lists()
            out.append(f"<h3>{inline(heading.group(1))}</h3>")
        elif item:
            end_paragraph()
            indent = len(item.group(1))
            while levels and indent < levels[-1]:
                out.append("</li></ul>")
                levels.pop()
            if levels and indent == levels[-1]:
                out.append("</li>")
            elif not levels or indent > levels[-1]:
                out.append("<ul>")
                levels.append(indent)
            out.append("<li>" + inline(item.group(2)))
        elif levels and raw.startswith(" "):
            out[-1] += " " + inline(line.strip())  # The continuation of a list item.
        else:
            close_lists()
            paragraph.append(line.strip())
    end_paragraph()
    close_lists()
    return re.sub(r"\n(</li>|</ul>)", r"\1", "\n".join(out))


def read_signature(path: Path) -> str:
    text = path.read_text(encoding="ascii").strip()
    if len(base64.b64decode(text, validate=True)) != 64:
        raise ValueError(f"{path} does not hold an Ed25519 signature")
    return text


def cdata(text: str) -> str:
    return "<![CDATA[" + text.replace("]]>", "]]]]><![CDATA[>") + "]]>"


def appcast_text(platform: str, version: str, date: str, notes: str, size: int, signature: str,
                 url: str, minimum_system_version: str | None = None,
                 installer_arguments: str | None = None) -> str:
    if platform not in PLATFORMS:
        raise ValueError(f"'{platform}' is not a platform with an appcast")
    if not url.startswith("https://"):
        raise ValueError(f"{url} is not an HTTPS address")
    if minimum_system_version and platform != "macos":
        raise ValueError("--minimum-system-version is for macOS")
    if installer_arguments and platform != "windows":
        raise ValueError("--installer-arguments is for Windows")
    attribute = html.escape
    enclosure = [f'url="{attribute(url, True)}"', f'length="{size}"',
                 'type="application/octet-stream"',
                 f'sparkle:edSignature="{attribute(signature, True)}"']
    if platform == "windows":
        enclosure.append('sparkle:os="windows"')
        if installer_arguments:
            enclosure.append(f'sparkle:installerArguments="{attribute(installer_arguments, True)}"')
    item = [
        f"      <title>Bloom {html.escape(version)}</title>",
        f"      <pubDate>{rfc822(date)}</pubDate>",
        f"      <sparkle:version>{html.escape(build_version(version))}</sparkle:version>",
        f"      <sparkle:shortVersionString>{html.escape(version)}"
        "</sparkle:shortVersionString>",
    ]
    if minimum_system_version:
        item.append("      <sparkle:minimumSystemVersion>"
                    f"{html.escape(minimum_system_version)}</sparkle:minimumSystemVersion>")
    item.append(f"      <description>{cdata(notes_to_html(notes))}</description>")
    item.append("      <enclosure " + " ".join(enclosure) + "/>")
    return "\n".join([
        '<?xml version="1.0" encoding="utf-8"?>',
        f'<rss version="2.0" xmlns:sparkle="{NAMESPACE}">',
        "  <channel>",
        "    <title>Bloom</title>",
        "    <item>",
        *item,
        "    </item>",
        "  </channel>",
        "</rss>",
        ""])


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Writes the appcast of a Bloom release.")
    parser.add_argument("--platform", required=True, choices=PLATFORMS)
    parser.add_argument("--version", required=True)
    parser.add_argument("--date", required=True)
    parser.add_argument("--notes", required=True, type=Path)
    parser.add_argument("--file", required=True, type=Path)
    parser.add_argument("--url", required=True)
    parser.add_argument("--minimum-system-version")
    parser.add_argument("--installer-arguments")
    parser.add_argument("--out", required=True, type=Path)
    options = parser.parse_args(arguments)
    try:
        text = appcast_text(
            options.platform, options.version, options.date,
            options.notes.read_text(encoding="utf-8"), options.file.stat().st_size,
            read_signature(options.file.with_name(options.file.name + ".sig")), options.url,
            options.minimum_system_version, options.installer_arguments)
        options.out.write_bytes(text.encode("utf-8"))
    except (OSError, ValueError) as error:
        print(f"appcast.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
