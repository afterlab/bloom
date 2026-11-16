#!/usr/bin/env python3
"""Writes and checks Bloom's update feed (docs/decisions/0041-self-update.md).

The feed is one small file a release publishes for its channel. It is a DSSE envelope: the
payload type, the payload in Base64, and Ed25519 signatures over both. The payload is JSON
(schemas/update/feed-1.schema.json): the version, the channel, the date, the release notes, and
for each platform the release files with their size, SHA-256 and Ed25519 signature.

    feed.py payload --version V --date YYYY-MM-DD --notes FILE --out FILE
            --file PLATFORM=KIND=PATH=URL ... [--appcast PLATFORM=URL ...]
        writes the payload. Each PATH needs PATH.sig beside it (tools/release/sign_release).
    feed.py envelope --payload FILE --signature KEYID=FILE ... --out FILE
        wraps a payload and its signatures (each FILE holds the Base64 signature).
    feed.py signed-bytes --payload FILE --out FILE
        writes the bytes a feed signature covers.
    feed.py verify --feed FILE --public-key BASE64
        checks a published feed with a public key (the program of ed25519_route.py).

This file never reads a private key. Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
import re
import sys
from pathlib import Path

PAYLOAD_TYPE = "application/vnd.bloom.update-feed+json"
SCHEMA = "bloom.update-feed/1"
PLATFORMS = ("linux-x86_64", "macos-arm64", "windows-x86_64")
VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
                     r"(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$")


def channel_of(version: str) -> str:
    """`alpha` for 0.1.0-alpha.1; `stable` for a version with no pre-release part."""
    if not VERSION.match(version):
        raise ValueError(f"'{version}' is not a version such as 0.1.0 or 0.1.0-alpha.1")
    if "-" not in version:
        return "stable"
    word = version.split("-", 1)[1].split(".", 1)[0]
    return word.lower() if word.isalpha() else "stable"


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_signature(path: Path) -> str:
    """The Base64 signature in a .sig file, checked to be 64 bytes."""
    text = path.read_text(encoding="ascii").strip()
    if len(base64.b64decode(text, validate=True)) != 64:
        raise ValueError(f"{path} does not hold an Ed25519 signature")
    return text


def file_entry(kind: str, path: Path, url: str) -> dict:
    if not url.startswith("https://") and not url.startswith("http://127.0.0.1"):
        raise ValueError(f"{url} is not an HTTPS address")
    return {
        "kind": kind,
        "url": url,
        "size": path.stat().st_size,
        "sha256": sha256_of(path),
        "signature": read_signature(path.with_name(path.name + ".sig")),
    }


def payload_bytes(version: str, date: str, notes: str, platforms: dict) -> bytes:
    """The payload, in the one written form `canonical_json` gives: sorted names, no white space,
    UTF-8, a final newline."""
    if not re.match(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$", date):
        raise ValueError(f"'{date}' is not a date such as 2026-01-31")
    for name, platform in platforms.items():
        if name not in PLATFORMS:
            raise ValueError(f"'{name}' is not a platform a release is published for")
        if not platform.get("files"):
            raise ValueError(f"platform {name} has no files")
    return canonical_json({
        "schema": SCHEMA,
        "channel": channel_of(version),
        "version": version,
        "published": date,
        "notes": notes,
        "platforms": platforms,
    })


def canonical_json(content: dict) -> bytes:
    text = json.dumps(content, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return text.encode("utf-8") + b"\n"


def signed_bytes(payload: bytes, payload_type: str = PAYLOAD_TYPE) -> bytes:
    """DSSE's pre-authentication encoding: what a feed signature is made over."""
    kind = payload_type.encode("utf-8")
    return b"DSSEv1 %d %s %d %s" % (len(kind), kind, len(payload), payload)


def key_id(public_key: bytes) -> str:
    """The name a signature uses for its key: the SHA-256 of the key's 32 bytes."""
    if len(public_key) != 32:
        raise ValueError("an Ed25519 public key is 32 bytes")
    return hashlib.sha256(public_key).hexdigest()


def envelope_bytes(payload: bytes, signatures: list[tuple[str, str]]) -> bytes:
    """The published file. `signatures` are (key id, Base64 signature) pairs."""
    if not signatures:
        raise ValueError("a feed needs a signature")
    envelope = {
        "payloadType": PAYLOAD_TYPE,
        "payload": base64.b64encode(payload).decode("ascii"),
        "signatures": [{"keyid": key, "sig": signature} for key, signature in signatures],
    }
    return json.dumps(envelope, indent=2).encode("ascii") + b"\n"


def load_route_module():
    spec = importlib.util.spec_from_file_location(
        "bloom_release_ed25519_route", Path(__file__).resolve().parent / "ed25519_route.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_signature(public_key: bytes, message: bytes, signature: bytes) -> bool:
    """Whether the signature is the public key's over the message (ed25519_route.py)."""
    return load_route_module().verify(public_key, message, signature)


def read_feed(data: bytes, public_keys: list[bytes]) -> dict:
    """The content of a published feed, after its signature by one of the keys is checked."""
    envelope = json.loads(data.decode("utf-8"))
    if envelope.get("payloadType") != PAYLOAD_TYPE:
        raise ValueError("the file is not an update feed")
    payload = base64.b64decode(envelope["payload"], validate=True)
    message = signed_bytes(payload)
    names = {key_id(key): key for key in public_keys}
    for entry in envelope.get("signatures", []):
        key = names.get(entry.get("keyid"))
        if key is not None and verify_signature(
                key, message, base64.b64decode(entry["sig"], validate=True)):
            return json.loads(payload.decode("utf-8"))
    raise ValueError("the feed is not signed by the given key")


def split(argument: str, parts: int, form: str) -> list[str]:
    values = argument.split("=", parts - 1)
    if len(values) != parts or not all(values):
        raise ValueError(f"'{argument}' is not {form}")
    return values


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Writes and checks Bloom's update feed.")
    commands = parser.add_subparsers(dest="command", required=True)
    payload = commands.add_parser("payload")
    payload.add_argument("--version", required=True)
    payload.add_argument("--date", required=True)
    payload.add_argument("--notes", required=True, type=Path)
    payload.add_argument("--file", action="append", default=[])
    payload.add_argument("--appcast", action="append", default=[])
    payload.add_argument("--out", required=True, type=Path)
    envelope = commands.add_parser("envelope")
    envelope.add_argument("--payload", required=True, type=Path)
    envelope.add_argument("--signature", action="append", default=[])
    envelope.add_argument("--out", required=True, type=Path)
    signed = commands.add_parser("signed-bytes")
    signed.add_argument("--payload", required=True, type=Path)
    signed.add_argument("--out", required=True, type=Path)
    verify = commands.add_parser("verify")
    verify.add_argument("--feed", required=True, type=Path)
    verify.add_argument("--public-key", action="append", required=True)
    options = parser.parse_args(arguments)
    try:
        if options.command == "payload":
            platforms: dict = {}
            for argument in options.file:
                platform, kind, path, url = split(argument, 4, "PLATFORM=KIND=PATH=URL")
                platforms.setdefault(platform, {"files": []})["files"].append(
                    file_entry(kind, Path(path), url))
            for argument in options.appcast:
                platform, url = split(argument, 2, "PLATFORM=URL")
                if platform not in platforms:
                    raise ValueError(f"an appcast is given for {platform}, which has no files")
                platforms[platform]["appcast"] = url
            notes = options.notes.read_text(encoding="utf-8")
            options.out.write_bytes(payload_bytes(options.version, options.date, notes, platforms))
        elif options.command == "envelope":
            pairs = []
            for argument in options.signature:
                key, path = split(argument, 2, "KEYID=FILE")
                pairs.append((key, read_signature(Path(path))))
            options.out.write_bytes(envelope_bytes(options.payload.read_bytes(), pairs))
        elif options.command == "signed-bytes":
            options.out.write_bytes(signed_bytes(options.payload.read_bytes()))
        else:
            keys = [base64.b64decode(key, validate=True) for key in options.public_key]
            content = read_feed(options.feed.read_bytes(), keys)
            print(f"{options.feed}: version {content['version']}, channel {content['channel']}, "
                  f"{len(content['platforms'])} platforms; the signature matches")
    except (OSError, ValueError, KeyError) as error:
        print(f"feed.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
