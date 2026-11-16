#!/usr/bin/env python3
"""The program the release tools make and check Ed25519 signatures with.

A release is signed with the `openssl` program (OpenSSL 3 or later), and that is the route
wherever the machine has one. Where it has none (the `openssl` of macOS is another program, which
cannot sign with Ed25519), the route is `bloom-ed25519-tool`, a build tool of Bloom made from the
same library Bloom verifies updates with (src/update/ed25519_tool_main.cpp). The environment
variable BLOOM_ED25519_TOOL names it; the tests and the end-to-end update test set it. Both
routes make the same bytes: an Ed25519 signature has one value for a key and a message.

Only tools/release/sign_release gives this module a private key file, and this module only hands
the file's name to the program. Only the Python standard library is used.
"""

from __future__ import annotations

import base64
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

# The DER form of an Ed25519 public key is this prefix and the key's 32 bytes.
PUBLIC_KEY_DER_PREFIX = bytes.fromhex("302a300506032b6570032100")
TOOL_VARIABLE = "BLOOM_ED25519_TOOL"


def openssl_program() -> str | None:
    """The `openssl` program when it is OpenSSL 3 or later, else nothing."""
    program = shutil.which("openssl")
    if program is None:
        return None
    result = subprocess.run([program, "version"], capture_output=True, text=True, check=False)
    words = result.stdout.split()
    if result.returncode != 0 or len(words) < 2 or words[0] != "OpenSSL":
        return None
    major = words[1].split(".", 1)[0]
    return program if major.isdigit() and int(major) >= 3 else None


def built_tool() -> str | None:
    """Bloom's own program, when the environment names one that exists."""
    program = os.environ.get(TOOL_VARIABLE, "")
    return program if program and Path(program).is_file() else None


def route() -> tuple[str, str]:
    """("openssl", program) or ("tool", program); ValueError when the machine has neither."""
    program = openssl_program()
    if program is not None:
        return ("openssl", program)
    program = built_tool()
    if program is not None:
        return ("tool", program)
    raise ValueError("no program for Ed25519 signatures: the `openssl` program (OpenSSL 3) is "
                     f"not on PATH, and {TOOL_VARIABLE} names no bloom-ed25519-tool")


def run(command: list[str]) -> bytes:
    result = subprocess.run(command, capture_output=True, check=False)
    if result.returncode != 0:
        raise ValueError(f"{Path(command[0]).name} {command[1]} failed: "
                         f"{result.stderr.decode('utf-8', 'replace').strip()}")
    return result.stdout


def key_form(key: Path) -> str:
    """How the key file is written, as `openssl` names it."""
    with key.open("rb") as file:
        return "PEM" if file.read(5) == b"-----" else "DER"


def public_key(key: Path, chosen: tuple[str, str] | None = None) -> bytes:
    """The 32 bytes of the public half of the private key in the file."""
    kind, program = chosen or route()
    if kind == "tool":
        public = base64.b64decode(run([program, "public", str(key)]).strip(), validate=True)
        if len(public) != 32:
            raise ValueError("the key is not an Ed25519 key")
        return public
    der = run([program, "pkey", "-in", str(key), "-inform", key_form(key), "-pubout",
               "-outform", "DER"])
    if len(der) != len(PUBLIC_KEY_DER_PREFIX) + 32 or not der.startswith(PUBLIC_KEY_DER_PREFIX):
        raise ValueError("the key is not an Ed25519 key")
    return der[len(PUBLIC_KEY_DER_PREFIX):]


def sign(key: Path, path: Path, chosen: tuple[str, str] | None = None) -> bytes:
    """The Ed25519 signature of the file's bytes, by the private key in the key file."""
    kind, program = chosen or route()
    with tempfile.TemporaryDirectory() as work:
        out = Path(work) / "signature"
        if kind == "tool":
            run([program, "sign", str(key), str(path), str(out)])
        else:
            run([program, "pkeyutl", "-sign", "-inkey", str(key), "-keyform", key_form(key),
                 "-rawin", "-in", str(path), "-out", str(out)])
        signature = out.read_bytes()
    if len(signature) != 64:
        raise ValueError(f"the signature of {path} is not 64 bytes")
    return signature


def verify(public: bytes, message: bytes, signature: bytes,
           chosen: tuple[str, str] | None = None) -> bool:
    """Whether the signature is the public key's over the message. The message goes through a
    file, never a pipe."""
    kind, program = chosen or route()
    with tempfile.TemporaryDirectory() as work:
        folder = Path(work)
        (folder / "message").write_bytes(message)
        (folder / "signature").write_bytes(signature)
        if kind == "tool":
            command = [program, "verify", base64.b64encode(public).decode("ascii"),
                       str(folder / "message"), str(folder / "signature")]
        else:
            pem = ("-----BEGIN PUBLIC KEY-----\n"
                   + base64.b64encode(PUBLIC_KEY_DER_PREFIX + public).decode("ascii")
                   + "\n-----END PUBLIC KEY-----\n")
            (folder / "key.pem").write_text(pem, encoding="ascii")
            command = [program, "pkeyutl", "-verify", "-pubin", "-inkey",
                       str(folder / "key.pem"), "-rawin", "-in", str(folder / "message"),
                       "-sigfile", str(folder / "signature")]
        return subprocess.run(command, capture_output=True, check=False).returncode == 0
