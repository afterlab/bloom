"""The carried interpreter never writes into its own home (issue #677).

Run by the carried interpreter inside Bloom (`bloom-cli run`) with the per-user cache directory
pointed at BLOOM_PYTHON_CACHE_ROOT. A package is not the user's to write to: the compiled form
of an imported module goes under Bloom's per-user cache, in `python-bytecode`.
"""

import importlib
import importlib.util
import os
import sys

root = os.path.realpath(os.environ["BLOOM_PYTHON_CACHE_ROOT"])
home = os.path.realpath(sys.prefix)

assert sys.pycache_prefix, "the interpreter has no bytecode cache directory of its own"
prefix = os.path.realpath(sys.pycache_prefix)
assert prefix.startswith(root + os.sep), f"{prefix} is not under the per-user cache {root}"
assert os.path.basename(prefix) == "python-bytecode", prefix
assert not prefix.startswith(home + os.sep), f"{prefix} is inside the interpreter home {home}"

# A module of the standard library that nothing has imported yet is compiled now.
name = "colorsys"
assert name not in sys.modules
module = importlib.import_module(name)
source = os.path.realpath(module.__file__)
assert source.startswith(home + os.sep), f"{source} is not the carried standard library"
cached = os.path.realpath(importlib.util.cache_from_source(module.__file__))
assert cached.startswith(prefix + os.sep), f"{cached} is not under {prefix}"
assert os.path.isfile(cached), f"{cached} was not written"
beside = os.path.join(os.path.dirname(source), "__pycache__")
written = [
    entry
    for entry in (os.listdir(beside) if os.path.isdir(beside) else [])
    if entry.startswith(name + ".")
    and os.path.getmtime(os.path.join(beside, entry)) >= os.path.getmtime(cached) - 1
]
assert not written, f"compiled files were written into the interpreter home: {written}"
print("bytecode cache:", prefix)
