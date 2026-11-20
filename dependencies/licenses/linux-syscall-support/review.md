# linux-syscall-support License And Feature Review

Reviewed: 2026-11-20

## License

- Whole-component SPDX expression: `BSD-3-Clause`. The archive's `LICENSE` (Google LLC) is bound
  beside this file, and `linux_syscall_support.h` carries the same notice.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Required

Crashpad's Linux client makes raw system calls from its signal handler, where the C library's
wrappers are not safe to call. This header supplies them.

## Reached Scope And Minimization

- One header, `linux_syscall_support.h`, built into Crashpad's Linux libraries. The recipe copies
  the archive into Crashpad's `third_party/lss/lss` directory on Linux only; it is not fetched for
  the macOS and Windows profiles. Its tests are not built.
