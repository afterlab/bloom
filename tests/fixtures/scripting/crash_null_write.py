"""Crashes bloom-cli on purpose (issue #736): a write through a null pointer, from ctypes.

The crash test (src/crash/tests/CMakeLists.txt, bloom.crash.cli-headless) runs it with
`bloom-cli run` and checks that the crash handler left a minidump and a text report.

The write is made by ctypes' own value setter, not by a foreign function: on Windows ctypes
guards every foreign call and turns an access violation inside one into OSError.
"""

import ctypes

ctypes.c_int.from_address(0).value = 1
