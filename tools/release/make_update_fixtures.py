#!/usr/bin/env python3
"""Writes the fixtures of the update tests (src/update/tests/fixtures) into a folder.

    python3 tools/release/make_update_fixtures.py [FOLDER]     default: src/update/tests/fixtures

Every file is made with the release tools themselves (feed.py and sign_release beside this file,
which sign with the `openssl` program where the machine has it), so the C++ reader is tested
against what a release really publishes. Ed25519 signatures are deterministic: the same keys and content give the same bytes on
every machine, and the release tools' test makes them again and compares.

The keys are not secrets. Their private halves are the test keys printed in RFC 8032, section
7.1; a build made with BLOOM_UPDATE_TEST_BUILD uses the first one and is refused by the package
check for a release. They are written for the signing program as DER files in a temporary folder.
"""

from __future__ import annotations

import base64
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parents[1]
FIXTURES = ROOT / "src" / "update" / "tests" / "fixtures"
# RFC 8032, section 7.1, tests 1 to 3: the private keys.
SEEDS = (
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
    "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
)
# The same section's messages for those keys, and the signatures it prints for them.
RFC_MESSAGES = ("", "72", "af82")
RFC_SIGNATURES = (
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9"
    "b46bd25bf5f0595bbe24655141438e7a100b",
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f1"
    "1d8c387b2eaeb4302aeeb00d291612bb0c00",
    "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984d"
    "c6594a7c15e9716ed28dc027beceea1ec40a",
)
# The order of the Ed25519 base point.
ORDER = 2**252 + 27742317777372353535851937790883648493
PRIME = 2**255 - 19


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


feed = load("bloom_release_feed", TOOLS / "feed.py")


def write_key(folder: Path, index: int) -> Path:
    """A PKCS#8 DER file of a test key: the fixed Ed25519 header and the 32-byte private key."""
    path = folder / f"key{index}.der"
    path.write_bytes(bytes.fromhex("302e020100300506032b657004220420") + bytes.fromhex(SEEDS[index]))
    return path


def tool(*arguments: str) -> bytes:
    result = subprocess.run([sys.executable, str(TOOLS / "sign_release"), *arguments],
                            capture_output=True, check=False)
    if result.returncode != 0:
        raise SystemExit(result.stderr.decode("utf-8", "replace"))
    return result.stdout


def sign(key: Path, work: Path, message: bytes) -> bytes:
    (work / "message").write_bytes(message)
    tool("detached", "--key-file", str(key), "--out", str(work / "signature"),
         str(work / "message"))
    return (work / "signature").read_bytes()


def not_a_point() -> bytes:
    """The smallest y above 1 for which no x puts (x, y) on the curve."""
    d = (-121665 * pow(121666, PRIME - 2, PRIME)) % PRIME
    y = 2
    while True:
        xx = (y * y - 1) * pow(d * y * y + 1, PRIME - 2, PRIME) % PRIME
        if xx != 0 and pow(xx, (PRIME - 1) // 2, PRIME) != 1:
            return y.to_bytes(32, "little")
        y += 1


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else FIXTURES
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        keys = [write_key(work, index) for index in range(len(SEEDS))]
        public = [base64.b64decode(tool("public-key", "--key-file", str(key)).strip())
                  for key in keys]
        (out / "trusted-key.txt").write_bytes(base64.b64encode(public[0]) + b"\n")
        (out / "other-key.txt").write_bytes(base64.b64encode(public[1]) + b"\n")

        # Signature cases: public key, message, signature, and whether it must be accepted.
        cases = []
        for index, message in enumerate(RFC_MESSAGES):
            body = bytes.fromhex(message)
            signature = bytes.fromhex(RFC_SIGNATURES[index])
            # `openssl pkeyutl` signs no empty file, so the first case is the RFC's text alone;
            # the others are also made here and must be the RFC's.
            if body and sign(keys[index], work, body) != signature:
                raise SystemExit(f"the signing program does not give RFC 8032's signature for test {index + 1}")
            cases.append((f"rfc8032-{index + 1}", public[index], body, signature, True))
        long_message = hashlib.sha512(b"bloom update fixture").digest() * 4
        long_signature = sign(keys[0], work, long_message)
        cases.append(("long-message", public[0], long_message, long_signature, True))
        flipped = bytearray(long_signature)
        flipped[5] ^= 0x10
        cases.append(("signature-bit-changed", public[0], long_message, bytes(flipped), False))
        flipped = bytearray(long_signature)
        flipped[40] ^= 0x01
        cases.append(("signature-second-half-changed", public[0], long_message, bytes(flipped),
                      False))
        cases.append(("message-changed", public[0], long_message[:-1] + b"\x00", long_signature,
                      False))
        cases.append(("message-shortened", public[0], long_message[:-1], long_signature, False))
        cases.append(("other-key", public[1], long_message, long_signature, False))
        # The same signature with the order added to its second half: equal as a number modulo
        # the order, and refused because that half must be below the order.
        second = int.from_bytes(long_signature[32:], "little") + ORDER
        cases.append(("second-half-not-reduced", public[0], long_message,
                      long_signature[:32] + second.to_bytes(32, "little"), False))
        cases.append(("key-not-on-curve", not_a_point(), long_message, long_signature, False))
        cases.append(("all-zero", bytes(32), b"", bytes(64), False))
        cases.append(("empty-message-other-signature", public[0], b"",
                      bytes.fromhex(RFC_SIGNATURES[1]), False))
        lines = ["# name public-key message signature accepted (hexadecimal; '-' is empty)"]
        for name, key, message, signature, accepted in cases:
            lines.append(" ".join([name, key.hex(), message.hex() or "-", signature.hex(),
                                   "yes" if accepted else "no"]))
        # (Bytes, not text: the line ends are the same on every system.)
        (out / "ed25519-cases.txt").write_bytes(("\n".join(lines) + "\n").encode("ascii"))

        # A stand-in for a release file, signed by each key.
        update = b"".join(hashlib.sha256(b"bloom update file %d" % block).digest()
                          for block in range(300))[:9001]
        (out / "update.bin").write_bytes(update)
        (out / "update.bin.sha256").write_bytes(
            (hashlib.sha256(update).hexdigest() + "\n").encode("ascii"))
        tool("files", "--key-file", str(keys[0]), str(out / "update.bin"))
        (work / "update.bin").write_bytes(update)
        tool("files", "--key-file", str(keys[1]), str(work / "update.bin"))
        (out / "update.bin.other-key.sig").write_bytes((work / "update.bin.sig").read_bytes())

        # The delta update's release file (see src/update/tests/reference/README.md) and the
        # reference tool's control file for it: their checksums and signatures, as a feed
        # would give them. The file itself is made again by the test.
        new = b"".join(hashlib.sha256(b"bloom zsync fixture %d" % index).digest()
                       for index in range(9375))
        control = (ROOT / "src" / "update" / "tests" / "reference" / "zsync-0.6.6.zsync")
        for name, content in (("zsync-new.bin", new), ("zsync-control", control.read_bytes())):
            (work / name).write_bytes(content)
            tool("files", "--key-file", str(keys[0]), str(work / name))
            (out / (name + ".sig")).write_bytes((work / (name + ".sig")).read_bytes())
            (out / (name + ".sha256")).write_bytes(
                (hashlib.sha256(content).hexdigest() + "\n").encode("ascii"))

        base = "https://github.com/afterlab/bloom/releases/download/v0.2.0-alpha.3/"

        def entry(kind: str, name: str, address: str = base) -> dict:
            return feed.file_entry(kind, out / "update.bin", address + name)

        def platforms(address: str = base) -> dict:
            return {
                "linux-x86_64": {"files": [
                    entry("tar", "Bloom-0.2.0-alpha.3-linux-x86_64.tar.xz", address),
                    entry("appimage", "Bloom-0.2.0-alpha.3-linux-x86_64.AppImage", address),
                    entry("zsync", "Bloom-0.2.0-alpha.3-linux-x86_64.AppImage.zsync", address)]},
                "macos-arm64": {
                    "appcast": address + "appcast-macos.xml",
                    "files": [entry("zip", "Bloom-0.2.0-alpha.3-macos-arm64.zip", address),
                              entry("dmg", "Bloom-0.2.0-alpha.3-macos-arm64.dmg", address),
                              entry("pkg", "Bloom-0.2.0-alpha.3-macos-arm64.pkg", address)]},
                "windows-x86_64": {
                    "appcast": address + "appcast-windows.xml",
                    "files": [
                        entry("installer", "Bloom-Setup-0.2.0-alpha.3-windows-x86_64.exe",
                              address),
                        entry("zip", "Bloom-0.2.0-alpha.3-windows-x86_64.zip", address)]},
            }

        notes = "### Added\n\n- A fixture note with an accent: café.\n"
        valid = feed.payload_bytes("0.2.0-alpha.3", "2026-11-30", notes, platforms())

        def publish(name: str, payload: bytes, signers: tuple[int, ...] = (0,)) -> None:
            signatures = []
            for index in signers:
                signature = sign(keys[index], work, feed.signed_bytes(payload))
                signatures.append((feed.key_id(public[index]),
                                   base64.b64encode(signature).decode("ascii")))
            (out / name).write_bytes(feed.envelope_bytes(payload, signatures))

        def altered(change) -> bytes:
            content = json.loads(valid.decode("utf-8"))
            change(content)
            return feed.canonical_json(content)

        publish("feed.json", valid)
        publish("feed-other-key.json", valid, (1,))
        publish("feed-two-keys.json", valid, (1, 0))
        publish("feed-beta.json",
                feed.payload_bytes("0.2.0-beta.1", "2026-11-30", notes, platforms()))
        publish("feed-loopback.json",
                feed.payload_bytes("0.2.0-alpha.3", "2026-11-30", notes,
                                   platforms("http://127.0.0.1:8123/")))
        publish("feed-plain-http.json", altered(lambda content: content["platforms"][
            "linux-x86_64"]["files"][0].update(url="http://example.invalid/Bloom.tar.xz")))
        publish("feed-no-version.json", altered(lambda content: content.pop("version")))
        publish("feed-version-of-other-channel.json",
                altered(lambda content: content.update(version="0.2.0")))
        publish("feed-other-schema.json",
                altered(lambda content: content.update(schema="bloom.update-feed/2")))
        publish("feed-unknown-fields.json", altered(lambda content: (
            content.update(later="a field a later version adds"),
            content["platforms"]["linux-x86_64"]["files"].append(
                dict(content["platforms"]["linux-x86_64"]["files"][0], kind="later-kind")))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
