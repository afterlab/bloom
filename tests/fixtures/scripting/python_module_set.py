"""The carried interpreter has the same modules on every platform (issue #677).

Run by the carried interpreter inside Bloom (`bloom-cli run`). It lists every module of the
standard library the interpreter can find, and the bundled packages with their versions, and
compares the list with python_module_set.txt, the one table committed for all three platforms.
A line of the table is a module name, optionally followed by the platforms that have it (a
module CPython itself limits to one system, such as `winreg` or `termios`); a name without
platforms is on all of them.
"""

import importlib
import importlib.util
import os
import sys

PLATFORM = {"linux": "linux", "darwin": "macos", "win32": "windows"}[sys.platform]

# Bundled third-party packages: the name and the attribute that holds the version.
BUNDLED = {"numpy": "__version__"}

# Modules a script relies on are imported for real, not only found: a module whose library
# failed to link would be found and still not work.
REQUIRED = [
    "ssl", "hashlib", "lzma", "compression.zstd", "zlib", "bz2", "sqlite3", "ctypes", "pyexpat",
    "decimal", "json", "csv", "struct", "array", "math", "cmath", "statistics", "random", "re",
    "datetime", "zoneinfo", "pathlib", "tempfile", "shutil", "zipfile", "tarfile", "gzip",
    "subprocess", "threading", "multiprocessing", "concurrent.futures", "asyncio", "socket",
    "select", "selectors", "urllib.request", "http.client", "xml.etree.ElementTree", "uuid",
    "dbm", "dbm.sqlite3", "unicodedata", "locale", "logging", "argparse", "pickle", "mmap",
]


def found(name: str) -> bool:
    try:
        return importlib.util.find_spec(name) is not None
    except (ImportError, ValueError):
        return False


def present() -> list[str]:
    lines = sorted(name for name in sys.stdlib_module_names if found(name))
    for name, attribute in sorted(BUNDLED.items()):
        module = importlib.import_module(name)
        lines.append(f"{name}=={getattr(module, attribute)}")
    return lines


def expected(path: str) -> list[str]:
    lines = []
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            fields = line.split("#", 1)[0].split()
            if fields and (len(fields) == 1 or PLATFORM in fields[1:]):
                lines.append(fields[0])
    return sorted(lines, key=lambda name: ("==" in name, name))


def foreign_callback() -> None:
    """C is called through ctypes and calls back: the carried libffi works both ways."""
    import ctypes

    library = ctypes.cdll.msvcrt if sys.platform == "win32" else ctypes.CDLL(None)
    pointer = ctypes.POINTER(ctypes.c_int)
    compare = ctypes.CFUNCTYPE(ctypes.c_int, pointer, pointer)
    values = (ctypes.c_int * 5)(5, 3, 1, 4, 2)
    library.qsort.restype = None
    library.qsort(values, 5, ctypes.sizeof(ctypes.c_int), compare(lambda a, b: a[0] - b[0]))
    assert list(values) == [1, 2, 3, 4, 5], list(values)


def main() -> None:
    for name in REQUIRED:
        importlib.import_module(name)
    foreign_callback()
    have = present()
    print(f"{len(have)} modules and bundled packages on {PLATFORM}, python {sys.version.split()[0]}")
    # The test names the committed table in the environment; without it the script prints the
    # list, which is how the table is written.
    table = os.environ.get("BLOOM_PYTHON_MODULE_SET")
    if not table:
        print("\n".join(have))
        return
    want = expected(table)
    missing = sorted(set(want) - set(have))
    extra = sorted(set(have) - set(want))
    assert not missing and not extra, f"module set differs: missing {missing}, not in the table {extra}"
    print("same module set as committed")


main()
