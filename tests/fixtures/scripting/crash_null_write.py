"""Crashes bloom-cli on purpose (issue #736): a write through a null pointer, from ctypes.

The crash test (src/crash/tests/CMakeLists.txt, bloom.crash.cli-headless) runs it with
`bloom-cli run` and checks that the crash handler left a minidump and a text report.

The write is made by ctypes' own value setter, not by a foreign function: on Windows ctypes
guards every foreign call and turns an access violation inside one into OSError.

Before the write it names, in the file BLOOM_TEST_CRASH_THREADS gives, the threads it knows are
alive at the crash (#756): the main thread, which crashes, and a held thread that waits until the
program ends. The check asserts on those, not on a thread the runtime may be ending at that moment.
The ids are the operating system's, the ones the minidump gives its threads.
"""

import ctypes
import os
import threading

threads_file = os.environ.get("BLOOM_TEST_CRASH_THREADS", "")


def record_alive(label: str) -> None:
    if threads_file:
        with open(threads_file, "a", encoding="utf-8") as stream:
            stream.write(f"{label} {threading.get_native_id()}\n")


started = threading.Event()


def hold() -> None:
    record_alive("held thread")
    started.set()
    threading.Event().wait()


threading.Thread(target=hold, name="held", daemon=True).start()
started.wait()
record_alive("main thread")
record_alive("crashing thread")
ctypes.c_int.from_address(0).value = 1
