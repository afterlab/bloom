# numpy License And Feature Review

Reviewed: 2026-11-12

## License

- SPDX expression for what this build compiles and installs:
  `BSD-3-Clause AND 0BSD AND MIT AND Zlib AND CC0-1.0`, the expression numpy's own metadata
  gives for a source build with nothing added. The texts are bound beside this file, named after
  their place in the archive:

  | File here | Part | License |
  | --- | --- | --- |
  | `LICENSE.txt` | numpy | BSD-3-Clause |
  | `_core-include-numpy-libdivide-LICENSE.txt` | libdivide | Zlib |
  | `_core-src-common-pythoncapi-compat-COPYING` | pythoncapi-compat | 0BSD |
  | `_core-src-highway-LICENSE` | Highway | Apache-2.0 or BSD-3-Clause; taken as BSD-3-Clause |
  | `_core-src-multiarray-dragon4_LICENSE.txt` | Dragon4 number printing | MIT |
  | `_core-src-npysort-x86-simd-sort-LICENSE.md` | x86-simd-sort | BSD-3-Clause |
  | `fft-pocketfft-LICENSE.md` | pocketfft | BSD-3-Clause |
  | `ma-LICENSE` | masked arrays | BSD-3-Clause |
  | `linalg-lapack_lite-LICENSE.txt` | the LAPACK subset numpy carries | BSD-3-Clause |
  | `random-LICENSE.md`, `random-src-*-LICENSE.md` | the random number generators | BSD-3-Clause, MIT, CC0-1.0 |
  | `_core-src-umath-svml-LICENSE` | Intel SVML | BSD-3-Clause; bound for completeness, not compiled (see below) |

- Not reached: Intel SVML (`-Ddisable-svml=true`), and the libraries numpy's published wheels
  add (OpenBLAS, libgfortran, libquadmath). None is built, linked or installed.
- The Meson copy in the archive (Apache-2.0) is a build tool run from the unpacked archive;
  nothing of it is installed.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Here

Issue #677 asks for a locked set of third-party Python packages that is the same on every
platform. numpy is the set: it is the one third-party module the repository's own scripts
import, and array arithmetic is what an artist's script needs first. Bloom's C++ code does not
use it.

## Reached Scope And Minimization

- Built from source on every profile by `dependencies/superbuild/projects/numpy.cmake` with the
  carried interpreter, the archive's own Meson and the locked Cython; no pip, no wheel, no
  network. The options are the lines of `numpy.configure-arguments`:
  - `-Dblas=none -Dlapack=none -Dallow-noblas=true`: no BLAS or LAPACK library. Matrix products
    and `numpy.linalg` use the routines numpy carries. Results then do not depend on a system
    or vendor library (macOS would otherwise use Accelerate), and they are slower than an
    accelerated build. Adding an accelerated library would be a new lock component.
  - `-Ddisable-svml=true`: Intel's vector math library exists for x86-64 Linux only.
  - `-Ddisable-optimization=true`: numpy's hand-written kernels for particular instruction sets
    are not built, and it does not choose among them when it starts. Those kernels fuse a
    multiplication and an addition where the processor can (always on arm64; on x86-64 only on
    processors that have the instruction), which gave `einsum`, complex multiplication and other
    results different last bits on different computers (measured between x86-64 Linux and arm64
    macOS). The portable C loops compute the same everywhere and are slower on large arrays.
    The sorting kernels of x86-simd-sort and Highway are among the code left out; their license
    texts stay bound because the archive carries them.
  - The compilers are told not to fuse a multiplication and an addition (`-ffp-contract=off`),
    so arm64 and x86-64 compute the same; see `python.cmake`.
- Installed into the carried interpreter's `site-packages` with Meson's `runtime` and
  `python-runtime` install tags, which leave numpy's test packages out.
- `numpy-install.cmake` imports it under the carried interpreter and fails the build unless the
  version is the locked one, its build record names no BLAS or LAPACK library, and it reports no
  instruction set it was built for or can choose.
- What still follows each system: the functions of the system's math library other than the
  square root (the transcendental functions, and `hypot`, which is also the absolute value of a
  complex number). They can differ in the last bit between platforms.
- On Linux its C++ parts load the C++ runtime (`libstdc++.so.6`, `libgcc_s.so.1`), as Bloom's own
  programs do; the package rules treat it the same way.
- Recorded roles: `library` (the extension modules), `data` (the Python files), `license`.
