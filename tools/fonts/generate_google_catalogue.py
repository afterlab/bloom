#!/usr/bin/env python3
"""Regenerates src/runtime/google_catalogue_data.cpp, the Google Fonts index baked into a release.

The index lists every family of the public google/fonts repository with its category, styles,
variable axes and family version; it holds no font file. Run it before a release:

    git clone --depth 1 --filter=blob:none --no-checkout https://github.com/google/fonts.git fonts
    git -C fonts sparse-checkout set --no-cone '/ofl/*/METADATA.pb' '/apache/*/METADATA.pb' \
        '/ufl/*/METADATA.pb'
    git -C fonts checkout
    python3 tools/fonts/generate_google_catalogue.py --fonts-repo fonts

The family version is read from the keyless stylesheet endpoint (the `/vNN/` segment of the font
file address), in batches; pass --no-versions to leave it empty when offline. The repository
revision and the index date are written into the file and shown by `bloom fonts` and the picker.
Bloom never fetches the family list at run time.
"""

import argparse
import datetime
import pathlib
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

CATEGORIES = {
    "SANS_SERIF": "sans",
    "SERIF": "serif",
    "DISPLAY": "display",
    "HANDWRITING": "handwriting",
    "MONOSPACE": "monospace",
}
STYLESHEET = "https://fonts.googleapis.com/css2"
AGENT = "bloom-catalogue-tool"
CHUNK_BYTES = 7000


def parse_metadata(text):
    """Returns (name, category, [(weight, italic)], [(tag, min, max)]) or None."""
    name = re.search(r'^name:\s*"([^"]+)"', text, re.M)
    category = re.search(r'^category:\s*"([A-Z_]+)"', text, re.M)
    if not name or not category or category.group(1) not in CATEGORIES:
        return None
    styles = set()
    for block in re.finditer(r"^fonts\s*\{(.*?)^\}", text, re.M | re.S):
        style = re.search(r'style:\s*"(\w+)"', block.group(1))
        weight = re.search(r"weight:\s*(\d+)", block.group(1))
        if style and weight:
            styles.add((int(weight.group(1)), style.group(1) == "italic"))
    axes = []
    for block in re.finditer(r"^axes\s*\{(.*?)^\}", text, re.M | re.S):
        tag = re.search(r'tag:\s*"(\w+)"', block.group(1))
        low = re.search(r"min_value:\s*(-?[\d.]+)", block.group(1))
        high = re.search(r"max_value:\s*(-?[\d.]+)", block.group(1))
        if tag and low and high:
            axes.append((tag.group(1), float(low.group(1)), float(high.group(1))))
    weight_axis = next((a for a in axes if a[0] == "wght"), None)
    if weight_axis:
        for italic in {italic for _, italic in styles} or {False}:
            for weight in range(100, 1000, 100):
                if weight_axis[1] <= weight <= weight_axis[2]:
                    styles.add((weight, italic))
    if not styles:
        styles.add((400, False))
    return name.group(1), CATEGORIES[category.group(1)], sorted(styles), axes


def number(value):
    return str(int(value)) if value == int(value) else repr(value)


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": AGENT})
    with urllib.request.urlopen(request, timeout=60) as reply:
        return reply.read().decode("utf-8")


def versions_of(css):
    found = {}
    for block in re.finditer(r"@font-face\s*\{(.*?)\}", css, re.S):
        family = re.search(r"font-family:\s*'([^']+)'", block.group(1))
        url = re.search(r"url\(([^)]+)\)", block.group(1))
        if not family or not url:
            continue
        version = re.search(r"/(v\d+)/", url.group(1)) or re.search(r"[?&]v=(v\d+)", url.group(1))
        if version:
            found.setdefault(family.group(1), version.group(1))
    return found


def family_versions(families, first_weights):
    result = {}
    names = sorted(families)
    for start in range(0, len(names), 25):
        batch = names[start:start + 25]
        query = "&".join("family=" + urllib.parse.quote_plus(name) for name in batch)
        try:
            result.update(versions_of(fetch(f"{STYLESHEET}?{query}")))
        except urllib.error.HTTPError:
            for name in batch:
                for suffix in ("", f":wght@{first_weights[name]}"):
                    try:
                        css = fetch(f"{STYLESHEET}?family={urllib.parse.quote_plus(name)}{suffix}")
                        result.update(versions_of(css))
                        break
                    except urllib.error.HTTPError:
                        continue
        time.sleep(0.2)
        print(f"versions {min(start + 25, len(names))}/{len(names)}", file=sys.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fonts-repo", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2] / "src/runtime/google_catalogue_data.cpp")
    parser.add_argument("--index-date", default=datetime.date.today().isoformat())
    parser.add_argument("--no-versions", action="store_true")
    args = parser.parse_args()

    revision = subprocess.run(["git", "-C", str(args.fonts_repo), "rev-parse", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
    families = {}
    for path in sorted(args.fonts_repo.glob("*/*/METADATA.pb")):
        parsed = parse_metadata(path.read_text(encoding="utf-8"))
        if parsed:
            families[parsed[0]] = parsed
    first = {name: min(w for w, _ in data[2]) for name, data in families.items()}
    versions = {} if args.no_versions else family_versions(families, first)

    if not args.no_versions:
        # A family the keyless stylesheet endpoint does not serve could never be downloaded.
        families = {name: data for name, data in families.items() if name in versions}

    lines = [f"#index-date={args.index_date}", f"#source-revision={revision}"]
    for name in sorted(families, key=str.lower):
        _, category, styles, axes = families[name]
        style_text = ",".join(f"{weight}{'i' if italic else ''}" for weight, italic in styles)
        axes_text = ",".join(f"{tag}:{number(low)}:{number(high)}" for tag, low, high in axes)
        lines.append("\t".join([name, category, versions.get(name, ""), style_text, axes_text]))

    chunks, current = [], ""
    for line in lines:
        if current and len(current) + len(line) + 1 > CHUNK_BYTES:
            chunks.append(current)
            current = ""
        current += line + "\n"
    chunks.append(current)

    out = [
        "// Generated by tools/fonts/generate_google_catalogue.py from the public google/fonts",
        f"// repository at revision {revision}; do not edit.",
        "// Index: family, category, family version, styles, variable axes. No font files.",
        "",
        "#include <bloom/runtime/google_catalogue.hpp>",
        "",
        "namespace bloom::runtime::fonts::detail {",
        "namespace {",
        f"// {len(families)} families, in pieces short enough for every compiler's string literal limit.",
        "constexpr std::string_view kChunks[] = {",
    ]
    for chunk in chunks:
        out.append('R"bloomfonts(' + chunk + ')bloomfonts",')
    out += ["};", "} // namespace", "",
            "std::span<const std::string_view> googleCatalogueChunks() noexcept { return kChunks; }",
            "", "} // namespace bloom::runtime::fonts::detail", ""]
    args.output.write_text("\n".join(out), encoding="utf-8")
    print(f"wrote {args.output}: {len(families)} families, {len(chunks)} chunks, revision {revision[:12]}")


if __name__ == "__main__":
    main()
