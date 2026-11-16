#!/usr/bin/env python3
"""Tests dmg_layout.py, the tool that writes the macOS disk image's window layout (`.DS_Store`).

    python3 -I tools/release/test_dmg_layout.py

It needs nothing but Python and runs the same on Linux, macOS and Windows: the layout is written
from fixed stand-in values and read back, so no Mac and no Finder are involved. That Finder shows
the layout is checked on a Mac, by bloom.package.installers.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("bloom_dmg_layout", TOOLS / "dmg_layout.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)

BOOKMARK = bytes.fromhex("626f6f6b0000000000000000000000000000000000000000")


def run(*args: str) -> tuple[int, str, str]:
    """Runs the tool's command line in this process: status, standard output and error."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = tool.main(list(args))
    return status, out.getvalue(), err.getvalue()


def read_json(path: Path) -> dict:
    status, out, err = run("read", str(path))
    assert status == 0, err
    return json.loads(out)


def records_of(data: bytes) -> list[tuple]:
    """The records in the file's one node, read here without the tool's reader."""
    root_offset, root_size, _ = struct.unpack_from(">III", data, 8)
    root = data[root_offset + 4:root_offset + 4 + root_size]
    count, = struct.unpack_from(">I", root, 0)
    addresses = struct.unpack_from(f">{count}I", root, 8)
    node_offset = addresses[2] & ~0x1F
    node = data[node_offset + 4:node_offset + 4 + 4096]
    right, total = struct.unpack_from(">II", node, 0)
    assert right == 0
    pos = 8
    records = []
    for _ in range(total):
        units, = struct.unpack_from(">I", node, pos)
        name = node[pos + 4:pos + 4 + 2 * units].decode("utf-16-be")
        pos += 4 + 2 * units
        code, kind = node[pos:pos + 4].decode(), node[pos + 4:pos + 8].decode()
        pos += 8
        if kind == "long":
            value, = struct.unpack_from(">I", node, pos)
            pos += 4
        else:
            assert kind == "blob"
            length, = struct.unpack_from(">I", node, pos)
            value = node[pos + 4:pos + 4 + length]
            pos += 4 + length
        records.append((name, code, kind, value))
    return records


def decode_alias(data: bytes) -> dict:
    """An alias record decoded field by field, as the Alias Manager's layout says."""
    size, version = struct.unpack_from(">hh", data, 4)
    kind, = struct.unpack_from(">h", data, 8)
    volume = data[10:38]
    volume_date, = struct.unpack_from(">I", data, 38)
    fs_type = data[42:44]
    disk_type, folder, = struct.unpack_from(">hI", data, 44)
    name = data[50:114]
    number, date = struct.unpack_from(">II", data, 114)
    creator, type_code = data[122:126], data[126:130]
    levels_from, levels_to, attributes = struct.unpack_from(">hhI", data, 130)
    fs_id, reserved = data[138:140], data[140:150]
    fields = {
        "size": size, "version": version, "kind": kind,
        "volume": volume[1:1 + volume[0]].decode(), "volume_pad": volume[1 + volume[0]:],
        "volume_date": volume_date, "fs_type": fs_type, "disk_type": disk_type,
        "folder_number": folder, "name": name[1:1 + name[0]].decode(),
        "name_pad": name[1 + name[0]:], "file_number": number, "file_date": date,
        "creator": creator, "type": type_code, "levels": (levels_from, levels_to),
        "attributes": attributes, "fs_id": fs_id, "reserved": reserved, "tags": [],
    }
    pos = 150
    while True:
        tag, length = struct.unpack_from(">hH", data, pos)
        pos += 4
        if tag == -1:
            assert length == 0
            break
        fields["tags"].append((tag, data[pos:pos + length]))
        pos += length + (length & 1)
    assert pos == len(data) == size
    return fields


class FixedFileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.path = Path(self.dir.name) / "layout.ds"

    def write(self, *extra: str) -> bytes:
        status, _, err = run("write-fixed", "--out", str(self.path), *extra)
        self.assertEqual(status, 0, err)
        return self.path.read_bytes()

    def test_same_inputs_give_the_same_bytes(self) -> None:
        first = self.write()
        self.path.unlink()
        self.assertEqual(first, self.write())
        self.assertEqual(first, tool.fixed_store())

    def test_read_gives_back_the_layout(self) -> None:
        self.write()
        self.assertEqual(read_json(self.path), {
            "window": {"width": 660, "height": 400, "left": 200, "top": 120},
            "iconSize": 128,
            "background": {"volume": "Bloom", "path": "/.background/dmg-background.tiff",
                           "bookmark": False},
            "items": {"Applications": [480, 180], "Bloom.app": [180, 180]},
        })

    def test_the_fixed_parts_are_where_the_format_says(self) -> None:
        data = self.write()
        self.assertEqual(data[0:4], b"\0\0\0\1")
        self.assertEqual(data[4:8], b"Bud1")
        root_offset, root_size, again = struct.unpack_from(">III", data, 8)
        self.assertEqual((root_offset, root_size, again), (2048, 2048, 2048))
        self.assertEqual(data[20:36].hex(), "0000100c000000870000200b00000000")
        root = data[root_offset + 4:root_offset + 4 + root_size]
        count, zero = struct.unpack_from(">II", root, 0)
        self.assertEqual((count, zero), (3, 0))
        addresses = struct.unpack_from(">256I", root, 8)
        self.assertEqual(addresses[:3], (2048 | 11, 32 | 5, 4096 | 12))
        self.assertEqual(set(addresses[3:]), {0})
        directory = 8 + 4 * 256
        self.assertEqual(root[directory:directory + 4 + 1 + 4 + 4],
                         struct.pack(">I", 1) + b"\4DSDB" + struct.pack(">I", 1))
        # Everything the file says it holds is inside it, and the file ends with the node.
        self.assertEqual(len(data), 4 + 4096 + 4096)
        dsdb = data[32 + 4:32 + 4 + 20]
        self.assertEqual(struct.unpack(">IIIII", dsdb), (2, 0, 5, 1, 4096))
        # The free lists: 32 of them after the directory; widths 5, 11 and 12 are used.
        pos = directory + 13
        free = {}
        for width in range(32):
            entries, = struct.unpack_from(">I", root, pos)
            free[width] = list(struct.unpack_from(f">{entries}I", root, pos + 4))
            pos += 4 + 4 * entries
        self.assertEqual(free[5], [])
        self.assertEqual(free[11], [])
        self.assertEqual(free[12], [])
        self.assertEqual(free[10], [1024])
        self.assertEqual(free[13], [8192])
        self.assertEqual(free[30], [1 << 30])
        self.assertEqual(free[31], [])

    def test_the_alias_record_decodes_to_what_was_given(self) -> None:
        data = self.write()
        icons = tool.plistlib.loads(
            next(value for name, code, _, value in records_of(data)
                 if (name, code) == (".", "icvp")))
        alias = decode_alias(icons["backgroundImageAlias"])
        tags = dict(alias["tags"])
        self.assertEqual(alias["version"], 2)
        self.assertEqual(alias["kind"], 0)
        self.assertEqual(alias["volume"], "Bloom")
        self.assertEqual(alias["volume_date"], tool.FIXED_VOLUME_DATE)
        self.assertEqual(alias["fs_type"], b"H+")
        self.assertEqual(alias["disk_type"], 0)
        self.assertEqual(alias["folder_number"], tool.FIXED_FOLDER_NUMBER)
        self.assertEqual(alias["name"], "dmg-background.tiff")
        self.assertEqual(alias["file_number"], tool.FIXED_FILE_NUMBER)
        self.assertEqual(alias["file_date"], tool.FIXED_FILE_DATE)
        self.assertEqual((alias["creator"], alias["type"]), (b"\0" * 4, b"\0" * 4))
        self.assertEqual(alias["levels"], (-1, -1))
        self.assertEqual(alias["attributes"], 0)
        self.assertEqual(alias["fs_id"], b"\0\0")
        self.assertEqual(alias["reserved"], b"\0" * 10)
        self.assertEqual(set(alias["volume_pad"]), {0} if alias["volume_pad"] else set())
        self.assertEqual(set(alias["name_pad"]), {0} if alias["name_pad"] else set())
        self.assertEqual(tags[0], b".background")
        self.assertEqual(tags[1], struct.pack(">I", tool.FIXED_FOLDER_NUMBER))
        self.assertEqual(tags[2].replace(b"\0", b""), b"Bloom:.background:dmg-background.tiff")
        self.assertEqual(tags[14], struct.pack(">H", 19) + "dmg-background.tiff".encode("utf-16-be"))
        self.assertEqual(tags[15], struct.pack(">H", 5) + "Bloom".encode("utf-16-be"))
        self.assertEqual(tags[18], b"/.background/dmg-background.tiff")
        self.assertEqual(tags[19], b"/Volumes/Bloom")

    def test_the_records_hold_the_window_and_the_icons(self) -> None:
        records = records_of(self.write())
        self.assertEqual([(name, code) for name, code, _, _ in records], [
            (".", "bwsp"), (".", "icvp"), (".", "vSrn"),
            ("Applications", "Iloc"), ("Bloom.app", "Iloc")])
        by_key = {(name, code): value for name, code, _, value in records}
        window = tool.plistlib.loads(by_key[(".", "bwsp")])
        self.assertEqual(window["WindowBounds"], "{{200, 120}, {660, 400}}")
        for key in ("ShowPathbar", "ShowSidebar", "ShowStatusBar", "ShowTabView", "ShowToolbar",
                    "ContainerShowSidebar", "PreviewPaneVisibility"):
            self.assertIs(window[key], False, key)
        icons = tool.plistlib.loads(by_key[(".", "icvp")])
        self.assertEqual((icons["iconSize"], icons["textSize"]), (128.0, 13.0))
        self.assertEqual(icons["backgroundType"], 2)
        self.assertEqual(by_key[(".", "vSrn")], 1)
        self.assertEqual(by_key[("Bloom.app", "Iloc")], struct.pack(">II", 180, 180) +
                         bytes.fromhex("ffffffffffff0000"))
        self.assertEqual(by_key[("Applications", "Iloc")], struct.pack(">II", 480, 180) +
                         bytes.fromhex("ffffffffffff0000"))

    def test_a_bookmark_has_its_place_and_is_reported(self) -> None:
        records = records_of(self.write("--bookmark-bytes", BOOKMARK.hex()))
        self.assertEqual([(name, code) for name, code, _, _ in records][:4], [
            (".", "bwsp"), (".", "icvp"), (".", "pBBk"), (".", "vSrn")])
        self.assertEqual(records[2][3], BOOKMARK)
        self.assertTrue(read_json(self.path)["background"]["bookmark"])

    def test_records_too_large_for_one_node_are_refused(self) -> None:
        with self.assertRaises(tool.LayoutError):
            tool.encode_store([(".", "bwsp", "blob", b"x" * 5000)])
        # Just inside the node still works.
        tool.encode_store([(".", "bwsp", "blob", b"x" * 3000)])

    def test_read_refuses_what_is_not_a_ds_store(self) -> None:
        for content in (b"", b"hello", b"\0\0\0\1Bud1" + b"\0" * 40, b"\0" * 8192):
            self.path.write_bytes(content)
            status, out, err = run("read", str(self.path))
            self.assertEqual(status, 1)
            self.assertEqual(out, "")
            self.assertEqual(len(err.strip().splitlines()), 1, err)
        status, _, err = run("read", str(self.path.with_name("missing")))
        self.assertEqual(status, 1)
        self.assertEqual(len(err.strip().splitlines()), 1, err)

    def test_read_names_the_missing_record(self) -> None:
        self.path.write_bytes(tool.encode_store([("Bloom.app", "Iloc", "blob", b"\0" * 16)]))
        status, _, err = run("read", str(self.path))
        self.assertEqual(status, 1)
        self.assertIn("bwsp", err)

    def test_a_truncated_file_is_refused_in_one_line(self) -> None:
        data = self.write()
        self.path.write_bytes(data[:3000])
        status, _, err = run("read", str(self.path))
        self.assertEqual(status, 1)
        self.assertEqual(len(err.strip().splitlines()), 1, err)

    def test_bookmark_bytes_must_be_hex(self) -> None:
        status, _, err = run("write-fixed", "--out", str(self.path), "--bookmark-bytes", "xyz")
        self.assertEqual(status, 1)
        self.assertIn("hexadecimal", err)


class VolumeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.volume = Path(self.dir.name) / "Bloom"
        (self.volume / ".background").mkdir(parents=True)
        (self.volume / ".background" / "dmg-background.tiff").write_bytes(b"II*\0")
        (self.volume / "Bloom.app").mkdir()
        (self.volume / "Applications").mkdir()

    def test_a_volume_gets_a_layout_that_read_accepts(self) -> None:
        status, _, err = run("write", "--volume", str(self.volume))
        self.assertEqual(status, 0, err)
        layout = read_json(self.volume / ".DS_Store")
        self.assertEqual(layout["window"], {"width": 660, "height": 400, "left": 200, "top": 120})
        self.assertEqual(layout["items"]["Bloom.app"], [180, 180])
        self.assertEqual(layout["background"]["volume"], "Bloom")
        self.assertEqual([path.name for path in self.volume.iterdir() if path.name.startswith(".dmg_layout")], [])

    def test_the_alias_carries_the_numbers_of_the_volume(self) -> None:
        run("write", "--volume", str(self.volume))
        data = (self.volume / ".DS_Store").read_bytes()
        icons = tool.plistlib.loads(
            next(value for name, code, _, value in records_of(data)
                 if (name, code) == (".", "icvp")))
        alias = decode_alias(icons["backgroundImageAlias"])
        picture = self.volume / ".background" / "dmg-background.tiff"
        self.assertEqual(alias["file_number"], picture.stat().st_ino)
        self.assertEqual(alias["folder_number"], picture.parent.stat().st_ino)

    def test_a_volume_that_lacks_a_part_is_refused(self) -> None:
        for part in (".background/dmg-background.tiff", "Bloom.app", "Applications"):
            target = self.volume / part
            if target.is_dir():
                target.rmdir()
            else:
                target.unlink()
            status, _, err = run("write", "--volume", str(self.volume))
            self.assertEqual(status, 1, part)
            self.assertIn(part.split("/")[-1], err)
            self.assertFalse((self.volume / ".DS_Store").exists(), part)
            if part == "Bloom.app" or part == "Applications":
                target.mkdir()
            else:
                target.write_bytes(b"II*\0")

    def test_a_folder_that_is_not_there_is_refused(self) -> None:
        status, _, err = run("write", "--volume", str(self.volume / "missing"))
        self.assertEqual(status, 1)
        self.assertEqual(len(err.strip().splitlines()), 1, err)

    @unittest.skipIf(sys.platform == "darwin", "the bookmark works on macOS")
    def test_a_bookmark_is_refused_off_macOS(self) -> None:
        status, _, err = run("write", "--volume", str(self.volume), "--bookmark")
        self.assertEqual(status, 1)
        self.assertIn("macOS", err)
        self.assertFalse((self.volume / ".DS_Store").exists())


if __name__ == "__main__":
    unittest.main()
