"""Reports every program and library a packaged Bloom loads when it runs.

cmake/package/check_package.cmake runs this script with the package's own bloom-cli
(`bloom-cli run package_loaded_images.py`), so it runs in the carried interpreter, and judges
what it reports: everything loaded must come from inside the package or be a declared part of the
operating system.

The script

  1. imports Bloom's package, the bundled package set and every standard-library module that is
     made of a library the package carries, and uses each once, so that their libraries are
     loaded and known to work;
  2. writes the path of every image loaded into this process;
  3. when BLOOM_PACKAGE_CHECK_APPLICATION names the packaged application, starts it without a
     window (Qt's offscreen platform), waits until it has loaded its platform plugin and has
     stopped loading more, writes the path of every image loaded into that process, and stops
     it. The application is stopped, not asked to quit, so it saves no window state.

Output (BLOOM_PACKAGE_CHECK_OUTPUT), one record per line:

  cli <path>            an image loaded into bloom-cli
  application <path>    an image loaded into the application
  process cli <number>  the process of bloom-cli, and likewise of the application
  done cli              step 2 finished
  done application      step 3 finished
  error <text>          something did not work; the check fails with this text
"""

import os
import subprocess
import sys
import time

OUTPUT = os.environ["BLOOM_PACKAGE_CHECK_OUTPUT"]
APPLICATION = os.environ.get("BLOOM_PACKAGE_CHECK_APPLICATION", "")
# How long the application may take to load its platform plugin, and how long its list of
# images must stay the same before it is taken as complete.
START_SECONDS = 120.0
SETTLE_SECONDS = 4.0


def use_carried_modules():
    import bz2
    import ctypes
    import decimal
    import hashlib
    import lzma
    import sqlite3
    import ssl
    import zlib
    from compression import zstd
    from xml.parsers import expat

    import numpy
    import numpy.fft
    import numpy.linalg
    import numpy.random

    import bloom

    data = bytes(range(256)) * 64
    assert zlib.decompress(zlib.compress(data)) == data
    assert bz2.decompress(bz2.compress(data)) == data
    assert lzma.decompress(lzma.compress(data)) == data
    assert zstd.decompress(zstd.compress(data)) == data
    assert len(hashlib.sha256(data).digest()) == 32
    assert len(hashlib.sha3_256(data).digest()) == 32
    assert len(hashlib.blake2b(data).digest()) == 64
    ssl.create_default_context()
    connection = sqlite3.connect(":memory:")
    assert connection.execute("select 6 * 7").fetchone() == (42,)
    connection.close()
    assert decimal.Decimal("0.1") + decimal.Decimal("0.2") == decimal.Decimal("0.3")
    parser = expat.ParserCreate()
    parser.Parse("<bloom/>", True)
    assert ctypes.sizeof(ctypes.c_void_p) == 8
    assert abs(numpy.linalg.det(2.0 * numpy.eye(3)) - 8.0) < 1e-9
    assert numpy.random.default_rng(7).integers(0, 10, 4).shape == (4,)
    assert numpy.fft.fft(numpy.ones(8))[0] == 8
    assert bloom.__name__ == "bloom"


def linux_images(process):
    images = set()
    with open(f"/proc/{process}/maps", encoding="utf-8", errors="replace") as maps:
        for line in maps:
            fields = line.split(None, 5)
            # Executable mappings of files: programs and libraries, not data files or fonts.
            if len(fields) == 6 and "x" in fields[1] and fields[5].startswith("/"):
                path = fields[5].rstrip("\n")
                if path.endswith(" (deleted)"):
                    path = path[: -len(" (deleted)")]
                images.add(path)
    return images


def macos_own_images():
    import ctypes

    system = ctypes.CDLL(None)
    system._dyld_image_count.restype = ctypes.c_uint32
    system._dyld_get_image_name.restype = ctypes.c_char_p
    system._dyld_get_image_name.argtypes = [ctypes.c_uint32]
    images = set()
    for index in range(system._dyld_image_count()):
        name = system._dyld_get_image_name(index)
        if name:
            images.add(os.fsdecode(name))
    return images


def macos_logged_images(log):
    # The loader's own record of what it loaded (DYLD_PRINT_LIBRARIES):
    # "dyld[<process>]: <identifier> /path/to/image".
    images = set()
    with open(log, encoding="utf-8", errors="replace") as lines:
        for line in lines:
            if line.startswith("dyld[") and "> /" in line:
                images.add(line[line.index("> /") + 2 :].rstrip("\n"))
    return images


def windows_images(handle):
    import ctypes
    from ctypes import wintypes

    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.EnumProcessModulesEx.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.HMODULE),
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        wintypes.DWORD,
    ]
    psapi.EnumProcessModulesEx.restype = wintypes.BOOL
    psapi.GetModuleFileNameExW.argtypes = [
        wintypes.HANDLE,
        wintypes.HMODULE,
        wintypes.LPWSTR,
        wintypes.DWORD,
    ]
    psapi.GetModuleFileNameExW.restype = wintypes.DWORD
    modules = (wintypes.HMODULE * 4096)()
    needed = wintypes.DWORD()
    every_module = 0x03
    if not psapi.EnumProcessModulesEx(
        handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), every_module
    ):
        # The process is still being set up; the caller asks again.
        return set()
    images = set()
    name = ctypes.create_unicode_buffer(32768)
    for index in range(min(needed.value // ctypes.sizeof(wintypes.HMODULE), len(modules))):
        if psapi.GetModuleFileNameExW(handle, modules[index], name, len(name)):
            images.add(name.value)
    return images


def own_images():
    if sys.platform.startswith("linux"):
        return linux_images("self")
    if sys.platform == "darwin":
        return macos_own_images()
    import ctypes
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    return windows_images(kernel.GetCurrentProcess())


def application_images(record):
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    log = OUTPUT + ".application-log"
    if sys.platform == "darwin":
        environment["DYLD_PRINT_LIBRARIES"] = "1"
    with open(log, "wb") as output:
        process = subprocess.Popen(
            [APPLICATION],
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=environment,
        )
    record(f"process application {process.pid}")
    try:
        started = time.monotonic()
        images = set()
        unchanged_since = started
        while True:
            if process.poll() is not None:
                record(f"error the application exited with {process.returncode} while starting")
                return
            if sys.platform.startswith("linux"):
                now = linux_images(process.pid)
            elif sys.platform == "darwin":
                now = macos_logged_images(log)
            else:
                now = windows_images(int(process._handle))
            moment = time.monotonic()
            if now != images:
                images = now
                unchanged_since = moment
            platform_loaded = any("qoffscreen" in os.path.basename(image) for image in images)
            if platform_loaded and moment - unchanged_since >= SETTLE_SECONDS:
                break
            if moment - started > START_SECONDS:
                record(
                    "error the application did not load its platform plugin within "
                    f"{int(START_SECONDS)} seconds"
                )
                return
            time.sleep(0.25)
        for image in sorted(images):
            record(f"application {image}")
        record("done application")
    finally:
        process.kill()
        process.wait()


def main():
    with open(OUTPUT, "w", encoding="utf-8") as output:

        def record(line):
            output.write(line + "\n")
            output.flush()

        try:
            use_carried_modules()
            record(f"process cli {os.getpid()}")
            for image in sorted(own_images()):
                record(f"cli {image}")
            record("done cli")
            if APPLICATION:
                application_images(record)
        except BaseException as error:  # noqa: BLE001 - every failure is reported, then raised
            record(f"error {type(error).__name__}: {error}")
            raise


main()
