#!/usr/bin/env python3
"""Writes and reads the window layout (`.DS_Store`) of Bloom's macOS disk image.

    python3 -I tools/release/dmg_layout.py write --volume <mounted volume> [--bookmark]
    python3 -I tools/release/dmg_layout.py write-fixed --out <file>
    python3 -I tools/release/dmg_layout.py read <.DS_Store file>

Finder shows the image's background picture, its window size and its icon places only when the
volume holds a `.DS_Store` that says so. This tool makes that file the same way every time, with
no Finder step (docs/architecture/platform-support.md, "macOS Installers"):

  write        runs on the mounted, read-write image before it is compressed. It reads what the
               alias record of the background picture needs from the volume (file numbers and
               creation times) and writes `<volume>/.DS_Store`. With --bookmark (macOS only) the
               system also makes the bookmark of the picture that newer systems keep.
  write-fixed  writes the same file from fixed stand-in values, never from a volume. The unit
               test uses it for the determinism check and the round trip on every platform.
  read         prints the window, the icon size, the background and the icon places found in
               a `.DS_Store` as JSON, or says on one line that the file is not one Bloom wrote.

The file format is the buddy allocator and the B-tree Finder uses. Everything is big-endian.
Only the Python standard library is used, and the same inputs give the same bytes.
"""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import re
import struct
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

# --- The layout ------------------------------------------------------------------------------------
VOLUME_NAME = "Bloom"
BACKGROUND_FOLDER = ".background"
BACKGROUND_FILE = "dmg-background.tiff"
BACKGROUND_PATH = f"/{BACKGROUND_FOLDER}/{BACKGROUND_FILE}"
WINDOW_LEFT = 200
WINDOW_TOP = 120
WINDOW_WIDTH = 660
WINDOW_HEIGHT = 400
ICON_SIZE = 128
TEXT_SIZE = 13
# Icon centers. The picture's arrow runs from x 268 to x 390 at y 180.
ITEMS = {"Bloom.app": (180, 180), "Applications": (480, 180)}
# The alias record always names this mount point, whatever the real one is.
ALIAS_MOUNT_POINT = f"/Volumes/{VOLUME_NAME}"

# Stand-in values of write-fixed: seconds since 1904-01-01 and file numbers.
FIXED_VOLUME_DATE = 3_800_000_000
FIXED_FILE_DATE = 3_800_000_100
FIXED_FOLDER_NUMBER = 17
FIXED_FILE_NUMBER = 18

SECONDS_1904_TO_1970 = 2_082_844_800
PAGE_SIZE = 0x1000
ROOT_WIDTH = 11
NODE_WIDTH = 12


class LayoutError(Exception):
    """A problem that is reported as one line."""


# --- Buddy allocator file --------------------------------------------------------------------------
class Allocator:
    """The free lists of the file's buddy allocator, as they are after the header is taken."""

    def __init__(self) -> None:
        self.free = {width: [1 << width] for width in range(5, 31)}

    def allocate(self, width: int) -> int:
        """Offset of a block of 2**width bytes: the lowest free one, else a split of a wider one."""
        wider = width
        while wider <= 30 and not self.free.get(wider):
            wider += 1
        if wider > 30:
            raise LayoutError("the allocator has no room left")
        offset = self.free[wider].pop(0)
        while wider > width:
            wider -= 1
            self.free.setdefault(wider, []).append(offset + (1 << wider))
            self.free[wider].sort()
        return offset

    def free_lists(self) -> bytes:
        out = bytearray()
        for width in range(32):
            offsets = sorted(self.free.get(width, []))
            out += struct.pack(">I", len(offsets))
            out += b"".join(struct.pack(">I", offset) for offset in offsets)
        return bytes(out)


def block_address(offset: int, width: int) -> int:
    return offset | width


# --- Records ---------------------------------------------------------------------------------------
# A record is (file name, structure id, data type, value); long values are ints, blob values bytes.
Record = tuple


def encode_record(record: Record) -> bytes:
    name, code, kind, value = record
    units = name.encode("utf-16-be")
    out = struct.pack(">I", len(units) // 2) + units
    out += code.encode("ascii") + kind.encode("ascii")
    if kind == "long":
        out += struct.pack(">I", value)
    elif kind == "blob":
        out += struct.pack(">I", len(value)) + bytes(value)
    else:
        raise LayoutError(f"cannot write a record of type '{kind}'")
    return out


def sort_key(record: Record):
    return (record[0].lower(), record[1])


def encode_store(records: list[Record]) -> bytes:
    """The whole `.DS_Store` file for these records, in one leaf node."""
    ordered = sorted(records, key=sort_key)
    body = b"".join(encode_record(record) for record in ordered)
    node = struct.pack(">II", 0, len(ordered)) + body
    if len(node) > PAGE_SIZE:
        raise LayoutError(
            f"the {len(ordered)} records take {len(node)} bytes, more than the {PAGE_SIZE} "
            "bytes of the one node this tool writes")

    allocator = Allocator()
    root_offset = allocator.allocate(ROOT_WIDTH)
    dsdb_offset = allocator.allocate(5)
    node_offset = allocator.allocate(NODE_WIDTH)
    addresses = [block_address(root_offset, ROOT_WIDTH), block_address(dsdb_offset, 5),
                 block_address(node_offset, NODE_WIDTH)]

    padded = addresses + [0] * (-len(addresses) % 256)
    root = struct.pack(">II", len(addresses), 0)
    root += b"".join(struct.pack(">I", value) for value in padded)
    root += struct.pack(">I", 1) + bytes([4]) + b"DSDB" + struct.pack(">I", 1)
    root += allocator.free_lists()
    if len(root) > (1 << ROOT_WIDTH):
        raise LayoutError("the allocator's root block does not fit its block")

    dsdb = struct.pack(">IIIII", 2, 0, len(ordered), 1, PAGE_SIZE)

    size = max(root_offset + (1 << ROOT_WIDTH), dsdb_offset + 32, node_offset + PAGE_SIZE) + 4
    data = bytearray(size)
    data[0:4] = struct.pack(">I", 1)
    data[4:36] = (b"Bud1" + struct.pack(">III", root_offset, 1 << ROOT_WIDTH, root_offset) +
                  bytes.fromhex("0000100c000000870000200b00000000"))
    for offset, content in ((root_offset, root), (dsdb_offset, dsdb), (node_offset, node)):
        data[offset + 4:offset + 4 + len(content)] = content
    return bytes(data)


# --- The alias record ------------------------------------------------------------------------------
def pascal(text: str, field: int) -> bytes:
    raw = text.encode("utf-8")
    if len(raw) > field - 1:
        raise LayoutError(f"'{text}' is too long for an alias record")
    return bytes([len(raw)]) + raw.ljust(field - 1, b"\0")


def tagged(tag: int, data: bytes) -> bytes:
    out = struct.pack(">hH", tag, len(data)) + data
    return out + b"\0" if len(data) & 1 else out


def mac_date(value: int, what: str) -> int:
    if not 0 <= value <= 0xFFFFFFFF:
        raise LayoutError(f"the {what} is outside the range of an alias record's dates")
    return value


def mac_number(value: int, what: str) -> int:
    if not 0 <= value <= 0xFFFFFFFF:
        raise LayoutError(f"the {what} does not fit an alias record's 32-bit file number")
    return value


def build_alias(volume_date: int, folder_number: int, file_number: int, file_date: int) -> bytes:
    """The version 2 alias record of the background picture. Dates are seconds since 1904."""
    volume_date = mac_date(volume_date, "volume creation date")
    file_date = mac_date(file_date, "picture creation date")
    folder_number = mac_number(folder_number, "folder number")
    file_number = mac_number(file_number, "picture's file number")
    fixed = (struct.pack(">h", 0) + pascal(VOLUME_NAME, 28) + struct.pack(">I", volume_date) +
             b"H+" + struct.pack(">hI", 0, folder_number) + pascal(BACKGROUND_FILE, 64) +
             struct.pack(">II", file_number, file_date) + b"\0\0\0\0" + b"\0\0\0\0" +
             struct.pack(">hhI", -1, -1, 0) + b"\0\0" + b"\0" * 10)
    colon_path = f"{VOLUME_NAME}:{BACKGROUND_FOLDER}:\0{BACKGROUND_FILE}".encode("utf-8")
    items = b"".join([
        tagged(0, BACKGROUND_FOLDER.encode()),
        tagged(16, struct.pack(">Q", volume_date << 16)),
        tagged(17, struct.pack(">Q", file_date << 16)),
        tagged(1, struct.pack(">I", folder_number)),
        tagged(2, colon_path),
        tagged(14, struct.pack(">H", len(BACKGROUND_FILE)) + BACKGROUND_FILE.encode("utf-16-be")),
        tagged(15, struct.pack(">H", len(VOLUME_NAME)) + VOLUME_NAME.encode("utf-16-be")),
        tagged(18, BACKGROUND_PATH.encode()),
        tagged(19, ALIAS_MOUNT_POINT.encode()),
        struct.pack(">hH", -1, 0),
    ])
    size = 8 + len(fixed) + len(items)
    return b"\0\0\0\0" + struct.pack(">hh", size, 2) + fixed + items


def parse_alias(data: bytes) -> dict:
    """The fields of an alias record that the layout check needs."""
    if len(data) < 150:
        raise LayoutError("the background's alias record is too short")
    size, version = struct.unpack_from(">hh", data, 4)
    if version != 2:
        raise LayoutError(f"the background's alias record has version {version}, not 2")
    pos = 8
    kind, = struct.unpack_from(">h", data, pos)
    volume_pascal = data[pos + 2:pos + 30]
    volume_name = volume_pascal[1:1 + volume_pascal[0]].decode("utf-8", "replace")
    pos += 2 + 28
    volume_date, = struct.unpack_from(">I", data, pos)
    pos += 4 + 2 + 2
    folder_number, = struct.unpack_from(">I", data, pos)
    pos += 4
    file_pascal = data[pos:pos + 64]
    file_name = file_pascal[1:1 + file_pascal[0]].decode("utf-8", "replace")
    pos += 64
    file_number, file_date = struct.unpack_from(">II", data, pos)
    pos += 8 + 4 + 4 + 2 + 2 + 4 + 2 + 10
    fields = {"kind": kind, "volume_name": volume_name, "volume_date": volume_date,
              "folder_number": folder_number, "file_name": file_name,
              "file_number": file_number, "file_date": file_date, "tags": {}}
    while True:
        if pos + 4 > len(data):
            raise LayoutError("the background's alias record has no end tag")
        tag, length = struct.unpack_from(">hH", data, pos)
        pos += 4
        if tag == -1:
            break
        value = data[pos:pos + length]
        if len(value) != length:
            raise LayoutError("the background's alias record ends inside a tag")
        fields["tags"][tag] = value
        pos += length + (length & 1)
    tags = fields["tags"]
    if 15 in tags:
        fields["volume_name"] = tags[15][2:].decode("utf-16-be")
    if 14 in tags:
        fields["file_name"] = tags[14][2:].decode("utf-16-be")
    if 18 in tags:
        fields["path"] = tags[18].decode("utf-8")
    if 19 in tags:
        fields["mount_point"] = tags[19].decode("utf-8")
    return fields


# --- The layout's records --------------------------------------------------------------------------
def dump_plist(value: dict) -> bytes:
    return plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=True)


def layout_records(alias: bytes, bookmark: bytes | None) -> list[Record]:
    window = {
        "ShowPathbar": False, "ShowSidebar": False, "ShowStatusBar": False,
        "ShowTabView": False, "ShowToolbar": False, "SidebarWidth": 0,
        "ContainerShowSidebar": False, "PreviewPaneVisibility": False,
        "WindowBounds": f"{{{{{WINDOW_LEFT}, {WINDOW_TOP}}}, {{{WINDOW_WIDTH}, {WINDOW_HEIGHT}}}}}",
    }
    icons = {
        "viewOptionsVersion": 1, "backgroundType": 2, "backgroundColorRed": 1.0,
        "backgroundColorGreen": 1.0, "backgroundColorBlue": 1.0, "iconSize": float(ICON_SIZE),
        "textSize": float(TEXT_SIZE), "gridSpacing": 100.0, "gridOffsetX": 0.0,
        "gridOffsetY": 0.0, "labelOnBottom": True, "showItemInfo": False,
        "showIconPreview": True, "arrangeBy": "none", "backgroundImageAlias": alias,
    }
    records: list[Record] = [
        (".", "bwsp", "blob", dump_plist(window)),
        (".", "icvp", "blob", dump_plist(icons)),
    ]
    if bookmark is not None:
        records.append((".", "pBBk", "blob", bookmark))
    records.append((".", "vSrn", "long", 1))
    for name, (x, y) in ITEMS.items():
        records.append((name, "Iloc", "blob",
                        struct.pack(">II", x, y) + bytes.fromhex("ffffffffffff0000")))
    return records


def fixed_store(bookmark: bytes | None = None) -> bytes:
    alias = build_alias(FIXED_VOLUME_DATE, FIXED_FOLDER_NUMBER, FIXED_FILE_NUMBER, FIXED_FILE_DATE)
    return encode_store(layout_records(alias, bookmark))


# --- Reading a volume and the bookmark -------------------------------------------------------------
def birth_seconds(path: Path) -> int:
    status = os.stat(path)
    return int(getattr(status, "st_birthtime", status.st_mtime)) + SECONDS_1904_TO_1970


def volume_store(volume: Path, bookmark: bool) -> bytes:
    picture = volume / BACKGROUND_FOLDER / BACKGROUND_FILE
    for needed in (picture, volume / "Bloom.app", volume / "Applications"):
        if not (needed.exists() or needed.is_symlink()):
            raise LayoutError(f"the volume lacks {needed.relative_to(volume)}")
    alias = build_alias(birth_seconds(volume), os.stat(volume / BACKGROUND_FOLDER).st_ino,
                        os.stat(picture).st_ino, birth_seconds(picture))
    return encode_store(layout_records(alias, make_bookmark(picture) if bookmark else None))


def make_bookmark(path: Path) -> bytes:
    """The system's own bookmark of a file (macOS only), made through CoreFoundation."""
    if sys.platform != "darwin":
        raise LayoutError("--bookmark works on macOS only")
    import ctypes
    import ctypes.util

    framework = ctypes.util.find_library("CoreFoundation") or (
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    cf = ctypes.CDLL(framework)
    void_p, long_t, ulong_t = ctypes.c_void_p, ctypes.c_long, ctypes.c_ulong
    cf.CFURLCreateFromFileSystemRepresentation.argtypes = [
        void_p, ctypes.c_char_p, long_t, ctypes.c_ubyte]
    cf.CFURLCreateFromFileSystemRepresentation.restype = void_p
    cf.CFURLCreateBookmarkData.argtypes = [
        void_p, void_p, ulong_t, void_p, void_p, ctypes.POINTER(void_p)]
    cf.CFURLCreateBookmarkData.restype = void_p
    cf.CFDataGetLength.argtypes = [void_p]
    cf.CFDataGetLength.restype = long_t
    cf.CFDataGetBytePtr.argtypes = [void_p]
    cf.CFDataGetBytePtr.restype = void_p
    cf.CFErrorCopyDescription.argtypes = [void_p]
    cf.CFErrorCopyDescription.restype = void_p
    cf.CFStringGetCString.argtypes = [void_p, ctypes.c_char_p, long_t, ctypes.c_uint32]
    cf.CFStringGetCString.restype = ctypes.c_ubyte
    cf.CFRelease.argtypes = [void_p]
    cf.CFRelease.restype = None

    raw = os.fsencode(str(path))
    url = cf.CFURLCreateFromFileSystemRepresentation(None, raw, len(raw), 0)
    if not url:
        raise LayoutError(f"the system could not make a URL of {path}")
    error = void_p()
    data = None
    try:
        data = cf.CFURLCreateBookmarkData(None, url, 0, None, None, ctypes.byref(error))
        if not data:
            reason = "no reason given"
            if error:
                text = cf.CFErrorCopyDescription(error)
                if text:
                    buffer = ctypes.create_string_buffer(1024)
                    if cf.CFStringGetCString(text, buffer, len(buffer), 0x08000100):
                        reason = buffer.value.decode("utf-8", "replace")
                    cf.CFRelease(text)
            raise LayoutError(f"the system made no bookmark of {path}: {reason}")
        return ctypes.string_at(cf.CFDataGetBytePtr(data), cf.CFDataGetLength(data))
    finally:
        if data:
            cf.CFRelease(data)
        if error:
            cf.CFRelease(error)
        cf.CFRelease(url)


# --- Reading a file --------------------------------------------------------------------------------
class Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data

    def span(self, offset: int, size: int) -> bytes:
        start = offset + 4
        chunk = self.data[start:start + size]
        if offset < 0 or len(chunk) != size:
            raise LayoutError("the file ends inside a block")
        return chunk


def parse_records(data: bytes) -> list[Record]:
    if len(data) < 36 or data[0:4] != b"\0\0\0\1" or data[4:8] != b"Bud1":
        raise LayoutError("not a .DS_Store file")
    root_offset, root_size, again = struct.unpack_from(">III", data, 8)
    if root_offset != again:
        raise LayoutError("not a .DS_Store file: the root addresses differ")
    reader = Reader(data)
    root = reader.span(root_offset, root_size)
    count, = struct.unpack_from(">I", root, 0)
    pos = 8
    padded = (count + 255) & ~255
    addresses = list(struct.unpack_from(f">{padded}I", root, pos))[:count]
    pos += 4 * padded
    entries, = struct.unpack_from(">I", root, pos)
    pos += 4
    directory = {}
    for _ in range(entries):
        length = root[pos]
        name = root[pos + 1:pos + 1 + length]
        number, = struct.unpack_from(">I", root, pos + 1 + length)
        directory[name] = number
        pos += 5 + length
    if b"DSDB" not in directory:
        raise LayoutError("not a .DS_Store file: it has no DSDB directory entry")

    def block(number: int) -> bytes:
        address = addresses[number]
        return reader.span(address & ~0x1F, 1 << (address & 0x1F))

    tree_root, _height, _records, _nodes, _page = struct.unpack_from(
        ">IIIII", block(directory[b"DSDB"]), 0)
    records: list[Record] = []

    def walk(number: int, depth: int) -> None:
        if depth > 8:
            raise LayoutError("the record tree is too deep")
        node = block(number)
        right, count = struct.unpack_from(">II", node, 0)
        pos = 8
        for _ in range(count):
            child = None
            if right:
                child, = struct.unpack_from(">I", node, pos)
                pos += 4
                walk(child, depth + 1)
            record, pos = parse_record(node, pos)
            records.append(record)
        if right:
            walk(right, depth + 1)

    walk(tree_root, 0)
    return records


def parse_record(node: bytes, pos: int):
    units, = struct.unpack_from(">I", node, pos)
    pos += 4
    name = node[pos:pos + 2 * units].decode("utf-16-be")
    pos += 2 * units
    code = node[pos:pos + 4].decode("ascii", "replace")
    kind = node[pos + 4:pos + 8].decode("ascii", "replace")
    pos += 8
    if kind == "bool":
        value = node[pos] != 0
        pos += 1
    elif kind in ("long", "shor"):
        value, = struct.unpack_from(">I", node, pos)
        pos += 4
    elif kind == "blob":
        length, = struct.unpack_from(">I", node, pos)
        value = node[pos + 4:pos + 4 + length]
        if len(value) != length:
            raise LayoutError("the file ends inside a record")
        pos += 4 + length
    elif kind == "ustr":
        length, = struct.unpack_from(">I", node, pos)
        value = node[pos + 4:pos + 4 + 2 * length].decode("utf-16-be")
        pos += 4 + 2 * length
    elif kind == "type":
        value = node[pos:pos + 4]
        pos += 4
    elif kind in ("comp", "dutc"):
        value, = struct.unpack_from(">Q", node, pos)
        pos += 8
    else:
        raise LayoutError(f"a record has the unknown type '{kind}'")
    return (name, code, kind, value), pos


def number(value: float):
    return int(value) if float(value).is_integer() else value


def describe(data: bytes) -> dict:
    try:
        records = parse_records(data)
    except (struct.error, IndexError, UnicodeDecodeError) as problem:
        raise LayoutError(f"not a readable .DS_Store file ({problem})") from None
    found = {(name, code): value for name, code, _kind, value in records}

    def need(name: str, code: str):
        if (name, code) not in found:
            raise LayoutError(f"the .DS_Store has no {code} record for '{name}'")
        return found[(name, code)]

    try:
        window = plistlib.loads(need(".", "bwsp"))
        icons = plistlib.loads(need(".", "icvp"))
    except plistlib.InvalidFileException as problem:
        raise LayoutError(f"a property list in the .DS_Store cannot be read ({problem})") from None
    bounds = re.fullmatch(
        r"\{\{\s*(-?\d+),\s*(-?\d+)\s*\},\s*\{\s*(\d+),\s*(\d+)\s*\}\}",
        str(window.get("WindowBounds", "")))
    if not bounds:
        raise LayoutError("the .DS_Store's bwsp record has no readable WindowBounds")
    left, top, width, height = (int(part) for part in bounds.groups())
    if "iconSize" not in icons:
        raise LayoutError("the .DS_Store's icvp record has no iconSize")
    alias_bytes = icons.get("backgroundImageAlias")
    if not isinstance(alias_bytes, bytes):
        raise LayoutError("the .DS_Store's icvp record has no backgroundImageAlias")
    try:
        alias = parse_alias(alias_bytes)
    except (struct.error, UnicodeDecodeError) as problem:
        raise LayoutError(f"the background's alias record cannot be read ({problem})") from None
    if "path" not in alias:
        raise LayoutError("the background's alias record has no path")
    items = {}
    for name in sorted(name for name, code, *_ in records if code == "Iloc"):
        place = need(name, "Iloc")
        if len(place) < 8:
            raise LayoutError(f"the Iloc record of '{name}' is too short")
        items[name] = list(struct.unpack_from(">II", place, 0))
    if not items:
        raise LayoutError("the .DS_Store has no Iloc record")
    return {
        "window": {"width": width, "height": height, "left": left, "top": top},
        "iconSize": number(icons["iconSize"]),
        "background": {"volume": alias["volume_name"], "path": alias["path"],
                       "bookmark": (".", "pBBk") in found},
        "items": items,
    }


# --- Command line ----------------------------------------------------------------------------------
def write_file(path: Path, data: bytes) -> None:
    """Writes beside the target and renames, so that a failure leaves no half-written file."""
    handle, name = tempfile.mkstemp(dir=path.parent, prefix=".dmg_layout-")
    try:
        with os.fdopen(handle, "wb") as out:
            out.write(data)
        os.replace(name, path)
    except BaseException:
        if os.path.exists(name):
            os.unlink(name)
        raise


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    write = commands.add_parser("write", help="write <volume>/.DS_Store for a mounted image")
    write.add_argument("--volume", required=True, type=Path)
    write.add_argument("--bookmark", action="store_true", help="add the system's bookmark (macOS)")
    fixed = commands.add_parser("write-fixed", help="write the layout from stand-in values")
    fixed.add_argument("--out", required=True, type=Path)
    fixed.add_argument("--bookmark-bytes", metavar="HEX",
                       help="test only: bytes to store as the bookmark record")
    read = commands.add_parser("read", help="print a .DS_Store's layout as JSON")
    read.add_argument("file", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "write":
            if args.bookmark and sys.platform != "darwin":
                raise LayoutError("--bookmark works on macOS only")
            if not args.volume.is_dir():
                raise LayoutError(f"{args.volume} is not a folder")
            write_file(args.volume / ".DS_Store", volume_store(args.volume, args.bookmark))
        elif args.command == "write-fixed":
            try:
                bookmark = bytes.fromhex(args.bookmark_bytes) if args.bookmark_bytes else None
            except ValueError:
                raise LayoutError("--bookmark-bytes is not hexadecimal") from None
            write_file(args.out, fixed_store(bookmark))
        else:
            try:
                data = args.file.read_bytes()
            except OSError as problem:
                raise LayoutError(f"cannot read {args.file}: {problem.strerror}") from None
            print(json.dumps(describe(data)))
    except LayoutError as problem:
        print(f"dmg_layout.py: {problem}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
