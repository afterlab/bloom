#!/usr/bin/env python3
"""Import the web-platform-tests WebVTT parsing corpus into tests/fixtures/subtitles/wpt-webvtt/.

Usage:
  python3 tools/subtitles/import_wpt_webvtt.py          # write the fixtures
  python3 tools/subtitles/import_wpt_webvtt.py --check  # verify the checked-in fixtures

Python 3 standard library only, plus `node` on PATH: the JavaScript assertions in each
upstream `.test` file are executed by node against a recording stand-in for the TextTrackCueList
so the expected values come from running the assertions, not from guessing.
"""
import hashlib
import json
import re
import subprocess
import sys
import urllib.request
import warnings
from pathlib import Path

REVISION = "6852422b49e7dc8037d7f130ee07cf991fb73f05"
DATE_ACQUIRED = "2026-11-21"
RAW = "https://raw.githubusercontent.com/web-platform-tests/wpt/" + REVISION + "/"
API = "https://api.github.com/repos/web-platform-tests/wpt/contents/"
FILE_PARSING = "webvtt/parsing/file-parsing/"
CUE_TEXT = "webvtt/parsing/cue-text-parsing/dat/"
OUT = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "subtitles" / "wpt-webvtt"
COMMAND = "python3 tools/subtitles/import_wpt_webvtt.py"

RECORDER = r"""
const input = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const records = [];
class Ref { constructor(path) { this.path = path; } }
function mk(path) {
  return new Proxy(new Ref(path), {
    get(target, prop) {
      if (prop === '__path') return target.path;
      if (typeof prop === 'symbol') return undefined;
      return mk(target.path.concat([/^\d+$/.test(prop) ? Number(prop) : prop]));
    }
  });
}
function pathOf(x) { return (x !== null && typeof x === 'object' && x.__path) ? x.__path : null; }
const cues = new Proxy(new Array(input.cueCount), {
  get(target, prop) {
    if (prop === 'length') return input.cueCount;
    if (typeof prop === 'symbol') return target[prop];
    if (/^\d+$/.test(prop)) return mk([Number(prop)]);
    return target[prop];
  }
});
function assert_equals(actual, expected) {
  const a = pathOf(actual), b = pathOf(expected);
  if (a && b) records.push({path: a, sameAs: b});
  else if (a) records.push({path: a, value: expected});
  else throw new Error('assert_equals actual is not a recorded cue property');
}
function assert_not_equals(actual, expected) {
  const a = pathOf(actual), b = pathOf(expected);
  if (!(a && b)) throw new Error('assert_not_equals needs two recorded values');
  records.push({path: a, differsFrom: b});
}
function assert_true(actual) {
  const a = pathOf(actual);
  if (!a) throw new Error('assert_true actual is not a recorded cue property');
  records.push({path: a, value: true});
}
function assert_false(actual) {
  const a = pathOf(actual);
  if (!a) throw new Error('assert_false actual is not a recorded cue property');
  records.push({path: a, value: false});
}
function assert_nonnull(actual) {
  const a = pathOf(actual);
  if (!a) throw new Error('assert_nonnull actual is not a recorded cue property');
  records.push({path: a, nonNull: true});
}
new Function('cues', 'assert_equals', 'assert_not_equals', 'assert_true', 'assert_false',
             'assert_nonnull', input.js)(cues, assert_equals, assert_not_equals, assert_true,
                                         assert_false, assert_nonnull);
process.stdout.write(JSON.stringify(records));
"""


def fetch(url):
    last = None
    for _ in range(4):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "bloom-fixture-importer"})
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.read()
        except Exception as error:  # noqa: BLE001 - retry any transient network failure
            last = error
    raise RuntimeError("download failed: " + url + ": " + str(last))


def list_dir(path):
    entries = json.loads(fetch(API + path.rstrip("/") + "?ref=" + REVISION))
    return sorted(e["name"] for e in entries if e["type"] == "file")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def decode_like_build_py(vtt_text):
    # webvtt/parsing/file-parsing/tools/build.py: bytes(test_vtt, "utf-8").decode("unicode_escape"),
    # then written with open(path, "w"), i.e. UTF-8 on the platform that generated upstream.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        decoded = bytes(vtt_text, "utf-8").decode("unicode_escape")
    return decoded.encode("utf-8")


def split_test(raw):
    # build.py reads with open(path, "r"): universal newlines.
    source = raw.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
    parts = source.split("\n\n", 1)
    if len(parts) != 2:
        raise ValueError("missing blank line between header and body")
    header, body = parts
    title = header.split("\n", 1)[0]
    parts = body.split("\n===\n", 1)
    if len(parts) != 2:
        raise ValueError("missing === separator")
    return title, parts[0], parts[1]


def record_assertions(js):
    count_match = re.search(r"^assert_equals\(cues\.length, (\d+)(?:, [^)]*)?\);\s*$", js, re.M)
    cue_count = int(count_match.group(1)) if count_match else None
    body = re.sub(r"^assert_equals\(cues\.length, \d+(?:, [^)]*)?\);\s*$", "", js, flags=re.M)
    body = re.sub(r"assert_true\(!!(\w+),", r"assert_nonnull(\1,", body)
    document_lines = [ln.strip() for ln in body.split("\n")
                      if "document." in ln and ln.strip().startswith("assert")]
    if document_lines:
        body = "\n".join(ln for ln in body.split("\n") if ln.strip() not in document_lines)
    result = subprocess.run(["node", "-e", RECORDER],
                            input=json.dumps({"js": body, "cueCount": cue_count or 0}),
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError("assertion execution failed:\n" + result.stderr)
    records = json.loads(result.stdout)

    cues = {}
    distinct = []
    for rec in records:
        path = rec["path"]
        if len(path) < 2 or not isinstance(path[0], int):
            raise ValueError("unexpected assertion path: " + repr(path))
        cue = cues.setdefault(path[0], {})
        if "differsFrom" in rec:
            distinct.append([path[0], rec["differsFrom"][0]])
            continue
        if path[1] == "region" and len(path) == 2:
            if "sameAs" in rec:
                if rec["sameAs"][1:] != ["region"]:
                    raise ValueError("unexpected region identity: " + repr(rec))
                put(cue, "regionSameAsCue", rec["sameAs"][0])
            elif "nonNull" in rec:
                put(cue, "regionPresent", True)
            else:
                put(cue, "region", rec["value"])
        elif path[1] == "region" and len(path) == 3:
            region = cue.setdefault("region", {})
            if not isinstance(region, dict):
                raise ValueError("region both null and object")
            put(region, path[2], rec["value"])
        elif len(path) == 2 and "value" in rec:
            put(cue, path[1], rec["value"])
        else:
            raise ValueError("unsupported assertion: " + repr(rec))
    cue_list = []
    if cues:
        for index in range(max(cues) + 1):
            cue_list.append(cues.get(index, {}))
    return cue_count, cue_list, distinct, document_lines


def put(target, key, value):
    if key in target and target[key] != value:
        raise ValueError("conflicting assertions for " + key)
    target[key] = value


def build():
    """Return ({relative path: bytes}, {upstream path: sha256}, summary)."""
    files = {}
    upstream = {}

    def get(path):
        data = fetch(RAW + path)
        upstream[path] = sha256(data)
        return data

    license_bytes = get("LICENSE.md")
    files["LICENSE.md"] = license_bytes

    names = list_dir(FILE_PARSING + "support")
    tests = [n for n in names if n.endswith(".test")]
    vtts = [n for n in names if n.endswith(".vtt")]
    if len(tests) + len(vtts) != len(names):
        raise RuntimeError("unexpected file kinds in support/")
    get(FILE_PARSING + "tools/build.py")

    invalid_html = get(FILE_PARSING + "signature-invalid.html").decode("utf-8")
    titles = {file: "signature, " + title
              for title, file in re.findall(r"\['([^']+)',\s+'([^']+)'\]", invalid_html)}
    generated = set(list_dir(FILE_PARSING + "tests/support"))

    cases = []
    summary = {"hand": [], "unconverted": [], "generated_mismatch": []}
    for name in sorted(tests):
        case = name[:-5]
        raw = get(FILE_PARSING + "support/" + name)
        title, js, vtt = split_test(raw)
        payload = decode_like_build_py(vtt)
        files["file-parsing/" + case + ".vtt"] = payload
        if case + ".vtt" in generated:
            upstream_generated = fetch(RAW + FILE_PARSING + "tests/support/" + case + ".vtt")
            if upstream_generated != payload:
                summary["generated_mismatch"].append(case)
        cue_count, cue_list, distinct, document_lines = record_assertions(js)
        entry = {"case": case, "title": title, "signatureInvalid": False}
        if cue_count is not None:
            entry["cueCount"] = cue_count
        entry["cues"] = cue_list
        if distinct:
            entry["regionsDistinct"] = distinct
        entry["unconverted"] = document_lines
        if document_lines:
            summary["unconverted"].append(case)
        cases.append(entry)
    for name in sorted(vtts):
        case = name[:-4]
        if name not in titles:
            raise RuntimeError("no title in signature-invalid.html for " + name)
        data = get(FILE_PARSING + "support/" + name)
        files["file-parsing/" + case + ".vtt"] = data
        cases.append({"case": case, "title": titles[name], "signatureInvalid": True,
                      "cues": [], "unconverted": []})
    cases.sort(key=lambda c: c["case"])
    if len({c["case"] for c in cases}) != len(cases):
        raise RuntimeError("duplicate case name")
    files["file-parsing/expected.json"] = (json.dumps(cases, indent=2) + "\n").encode("utf-8")

    dat_counts = {}
    for name in list_dir(CUE_TEXT):
        if not name.endswith(".dat"):
            raise RuntimeError("unexpected file in dat/: " + name)
        data = get(CUE_TEXT + name)
        files["cue-text/" + name] = data
        dat_counts[name] = sum(1 for line in data.split(b"\n") if line == b"#data")
    files["cue-text/cases.json"] = (json.dumps(dat_counts, indent=2, sort_keys=True) + "\n").encode()

    upstream["LICENSE.md"] = sha256(license_bytes)
    files["PROVENANCE.md"] = provenance(upstream, tests, vtts, dat_counts, summary).encode("utf-8")
    summary["cases"] = len(cases)
    summary["dat_counts"] = dat_counts
    return files, summary


def provenance(upstream, tests, vtts, dat_counts, summary):
    lines = [
        "# web-platform-tests WebVTT parsing corpus provenance",
        "",
        "- Source repository: https://github.com/web-platform-tests/wpt",
        "- Revision: `" + REVISION + "` (`master` HEAD when resolved)",
        "- Date acquired: " + DATE_ACQUIRED,
        "- License: see `LICENSE.md` in this directory (verbatim copy of the repository `LICENSE.md`).",
        "",
        "## Layout",
        "",
        "- `cue-text/*.dat`: byte-exact copies of `webvtt/parsing/cue-text-parsing/dat/*.dat`.",
        "  `cue-text/cases.json` maps each file to its number of `#data` cases.",
        "- `file-parsing/<case>.vtt`: for each upstream `support/<case>.test`, the WebVTT payload after",
        "  the `===` line, decoded exactly as upstream `tools/build.py` does",
        "  (`bytes(text, \"utf-8\").decode(\"unicode_escape\")`, written as UTF-8). Upstream",
        "  `support/*.vtt` files (invalid-signature cases) are copied byte-exact.",
        "- `file-parsing/expected.json`: one entry per case. `signatureInvalid` is true only for the",
        "  upstream `support/*.vtt` files, which upstream expects to fire an error event; every `.test`",
        "  file expects a load event (its harness fails on error).",
        "",
        "## Conversion of the JavaScript assertions",
        "",
        "The assertions in each `.test` file are executed by `node` against a recording stand-in for",
        "the cue list, so loops and helpers are evaluated rather than parsed. Only asserted",
        "properties are present. `cueCount` is `assert_equals(cues.length, N)`. Cue entries are",
        "positional (index = cue index; an unasserted cue is `{}`). Region assertions are expressed",
        "inline: `\"region\": null`, `\"region\": {\"lines\": 2}` (asserted region properties),",
        "`regionPresent` (`assert_true(!!region)`), `regionSameAsCue` (the cue's region is the same",
        "object as that cue's region) and top-level `regionsDistinct` (`assert_not_equals` pairs of",
        "cue indices). Property values keep their JavaScript number or string type.",
        "",
        "- Hand-converted cases: none.",
    ]
    if summary["unconverted"]:
        lines.append("- Unconverted (raw assertion kept in `unconverted`): "
                     + ", ".join(summary["unconverted"])
                     + " (asserts `document.styleSheets`, which is not a cue property).")
    else:
        lines.append("- Unconverted cases: none.")
    lines += [
        "",
        "## Upstream files used (SHA-256 of the upstream bytes)",
        "",
    ]
    for path in sorted(upstream):
        lines.append("- `" + path + "`  " + upstream[path])
    lines += [
        "",
        "## Regeneration",
        "",
        "Requires `python3` and `node` on PATH and network access.",
        "",
        "    " + COMMAND,
        "    " + COMMAND + " --check",
        "",
    ]
    return "\n".join(lines)


def main(argv):
    check = "--check" in argv
    files, summary = build()
    if summary["generated_mismatch"]:
        sys.stderr.write("decoded payload differs from upstream generated tests/support file: "
                         + ", ".join(summary["generated_mismatch"]) + "\n")
        return 1
    if check:
        bad = []
        for rel, data in sorted(files.items()):
            path = OUT / rel
            if not path.exists() or path.read_bytes() != data:
                bad.append(rel)
        if OUT.exists():
            bad += [str(p.relative_to(OUT)) for p in OUT.rglob("*")
                    if p.is_file() and str(p.relative_to(OUT)).replace("\\", "/") not in files]
        if bad:
            sys.stderr.write("out of date: " + ", ".join(bad) + "\n")
            return 1
        return 0
    if OUT.exists():
        for p in OUT.rglob("*"):
            if p.is_file():
                p.unlink()
    for rel, data in files.items():
        path = OUT / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    print("wrote %d files; %d file-parsing cases; cue-text cases %s" % (
        len(files), summary["cases"], summary["dat_counts"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
