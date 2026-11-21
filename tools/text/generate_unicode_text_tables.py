#!/usr/bin/env python3
"""Generate src/render/unicode_text_tables.inc from the Unicode Character Database.

Usage:
  python3 tools/text/generate_unicode_text_tables.py          # write the file
  python3 tools/text/generate_unicode_text_tables.py --check  # verify the checked-in file

The file holds two range tables the text layout reads:
  - the UAX #14 line-break class of every code point (LineBreak.txt), with the LB1 resolution of
    SA applied here: SA marks (general category Mn or Mc, from UnicodeData.txt) become CM and the
    rest AL; AI, SG, XX and unlisted code points are AL; CJ is NS;
  - the scripts Bloom cannot lay out without shaping or bidi (Scripts.txt), each range with the
    script's name.

The UCD version is pinned and each downloaded file's SHA-256 is recorded in the output, so any
upstream drift shows up as a diff. See src/render/UNICODE-PROVENANCE.md.
"""
import hashlib
import sys
import urllib.request
from pathlib import Path

VERSION = "16.0.0"
BASE = "https://www.unicode.org/Public/" + VERSION + "/ucd/"
OUTPUT = Path(__file__).resolve().parents[2] / "src" / "render" / "unicode_text_tables.inc"

# The classes the layout distinguishes, in the order of render::LineBreakClass.
CLASSES = [
    "AL", "BA", "BB", "BK", "B2", "CB", "CL", "CM", "CP", "CR", "EB", "EM", "EX", "GL", "H2",
    "H3", "HL", "HY", "ID", "IN", "IS", "JL", "JT", "JV", "LF", "NL", "NS", "NU", "OP", "PO",
    "PR", "QU", "RI", "SP", "SY", "WJ", "ZW", "ZWJ", "AK", "AP", "AS", "VF", "VI",
]

# The enumerator a class is spelled as in C++ where its two-letter name would read as a Qt type
# to the repository's architecture check.
ENUMERATORS = {"QU": "Quotation"}

# Scripts whose text needs shaping or bidi to read correctly (#345 is not built yet).
SHAPED_SCRIPTS = [
    "Arabic", "Hebrew", "Syriac", "Thaana", "Nko", "Samaritan", "Mandaic", "Adlam", "Hanifi_Rohingya",
    "Devanagari", "Bengali", "Gurmukhi", "Gujarati", "Oriya", "Tamil", "Telugu", "Kannada",
    "Malayalam", "Sinhala", "Thai", "Lao", "Tibetan", "Myanmar", "Khmer", "Mongolian", "Balinese",
    "Javanese", "Sundanese", "Tai_Tham", "Tai_Viet", "New_Tai_Lue", "Buginese", "Batak", "Lepcha",
    "Limbu", "Chakma", "Cham", "Meetei_Mayek", "Ol_Chiki", "Saurashtra", "Kaithi", "Sharada",
    "Takri", "Tirhuta", "Grantha", "Newa", "Ahom", "Dogra", "Masaram_Gondi", "Gunjala_Gondi",
    "Nandinagari", "Old_South_Arabian", "Phoenician", "Imperial_Aramaic", "Nabataean", "Palmyrene",
    "Hatran", "Avestan", "Manichaean", "Psalter_Pahlavi", "Inscriptional_Pahlavi",
    "Inscriptional_Parthian", "Old_Sogdian", "Sogdian", "Elymaic", "Chorasmian", "Yezidi",
    "Old_Uyghur", "Rohingya",
]


def fetch(name):
    with urllib.request.urlopen(BASE + name, timeout=120) as response:
        return response.read()


def ranges_of(text):
    for line in text.split("\n"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        span, value = [part.strip() for part in line.split(";")[:2]]
        if ".." in span:
            first, last = span.split("..")
        else:
            first = last = span
        yield int(first, 16), int(last, 16), value


def marks(unicode_data):
    result = set()
    previous = None
    for line in unicode_data.split("\n"):
        if not line:
            continue
        fields = line.split(";")
        code = int(fields[0], 16)
        category = fields[2]
        if fields[1].endswith(", First>"):
            previous = (code, category)
            continue
        if fields[1].endswith(", Last>") and previous is not None:
            if previous[1] in ("Mn", "Mc"):
                result.update(range(previous[0], code + 1))
            previous = None
            continue
        if category in ("Mn", "Mc"):
            result.add(code)
    return result


def merged(entries):
    entries.sort()
    out = []
    for first, last, value in entries:
        if out and out[-1][2] == value and out[-1][1] + 1 == first:
            out[-1] = (out[-1][0], last, value)
        else:
            out.append((first, last, value))
    return out


def generate():
    files = {name: fetch(name) for name in ("LineBreak.txt", "UnicodeData.txt", "Scripts.txt")}
    mark = marks(files["UnicodeData.txt"].decode("utf-8"))
    breaks = []
    for first, last, value in ranges_of(files["LineBreak.txt"].decode("utf-8")):
        if value == "SA":
            for code in range(first, last + 1):
                breaks.append((code, code, "CM" if code in mark else "AL"))
            continue
        if value in ("AI", "SG", "XX"):
            value = "AL"
        if value == "CJ":
            value = "NS"
        if value not in CLASSES:
            raise SystemExit("unknown line break class " + value)
        breaks.append((first, last, value))
    breaks = [entry for entry in merged(breaks) if entry[2] != "AL"]
    scripts = [(f, l, v) for f, l, v in ranges_of(files["Scripts.txt"].decode("utf-8"))
               if v in SHAPED_SCRIPTS]
    scripts = merged(scripts)

    lines = [
        "// Generated by tools/text/generate_unicode_text_tables.py from the Unicode Character",
        "// Database " + VERSION + "; do not edit. See src/render/UNICODE-PROVENANCE.md.",
    ]
    for name, data in files.items():
        lines.append("// " + name + " sha256 " + hashlib.sha256(data).hexdigest())
    lines.append("// clang-format off")
    lines.append("inline constexpr char kUnicodeTextTablesVersion[] = \"" + VERSION + "\";")
    lines.append("// Every code point not covered below is AL.")
    lines.append("inline constexpr LineBreakRange kLineBreakRanges[] = {")
    for first, last, value in breaks:
        lines.append("    {0x%X, 0x%X, LineBreakClass::%s}," %
                     (first, last, ENUMERATORS.get(value, value)))
    lines.append("};")
    lines.append("inline constexpr std::string_view kShapedScriptNames[] = {")
    for name in SHAPED_SCRIPTS:
        lines.append("    \"" + name.replace("_", " ") + "\",")
    lines.append("};")
    lines.append("inline constexpr ScriptRange kShapedScriptRanges[] = {")
    for first, last, value in scripts:
        lines.append("    {0x%X, 0x%X, %d}," % (first, last, SHAPED_SCRIPTS.index(value)))
    lines.append("};")
    lines.append("// clang-format on")
    return "\n".join(lines) + "\n"


def main():
    text = generate()
    if "--check" in sys.argv:
        if OUTPUT.read_text(encoding="utf-8") != text:
            print(str(OUTPUT) + " is out of date", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
