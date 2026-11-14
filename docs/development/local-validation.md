# Local sprint validation

Status: working

Bloom's sprint runner is a direct local CLI: `tools/local/sprint_validation.py`. It starts no
service, runner registration, or GitHub Actions job. It uses the current source checkout and an
incremental CMake build directory whose `CMakeCache.txt` names that same checkout. A build directory
from another worktree cannot be reused. An existing dependency prefix can be supplied separately;
the runner never builds or downloads dependencies.

The default lock lives in the repository's common Git directory, so invocations from its worktrees
serialize configure, build, and test work. Use `--lock-file` or `BLOOM_LOCAL_LOCK_FILE` to share one
lock across independent clones on the same machine. The lock is fail-fast by default; set
`--lock-timeout-seconds` to wait. All cooperating runs must use the same lock path. Build and test
parallelism is `BLOOM_LOCAL_JOBS` or `--jobs` (default 2, accepted range 1–8); it never expands to
the machine's full core count automatically.

## Focused iteration

Focused runs require a build directory configured for the current source. They build only named
targets and run only exact CTest names. A cold build may still take time, so use focused runs after
the relevant build has been prepared at a milestone.

```sh
python3 tools/local/sprint_validation.py focused \
  --build-dir build/sprint-core --jobs 2 \
  --target bloom_runtime_result_ownership_test \
  --test bloom.runtime.result_ownership
```

Repeat `--target` and `--test` for a small set of related checks. A focused run may also be
target-only or test-only. Broad CMake targets such as `all` and `test` are refused in this mode.
CTest names are checked against the configured inventory before execution.

### Device tests

A CTest case labelled `device` touches the GPU (for example the Python and embedding scripting
tests, which render through the GPU export provider). The runner refuses to execute one unless the
machine GPU lock is serialized: pass `--gpu-lock-file` (or set `BLOOM_LOCAL_GPU_LOCK_FILE`) and the
runner takes that lock before the build lock -- the same order as wrapping a native run in the GPU
lock, so concurrent runs cannot deadlock -- or pass `--gpu-lock-held` when the caller already holds
it (never both: a second acquisition from inside a wrapper would wait on itself). Every run already
removes `DISPLAY` and `WAYLAND_DISPLAY` and uses the offscreen Qt platform.

## Explicit milestone gates

For `core` and `desktop`, a milestone normally builds the named union of `--target` values once,
then runs the exact named union of `--test` values. This is the intended M1 owning-target gate in
the [sprint plan](../plans/performance-reliability-sprint.md). `--all` is the separate, explicit
request for a full build and every CTest case. `native` builds and executes the existing bounded
GPU qualification gate with `--require-device`; it accepts neither `--all` nor named targets/tests.
A skipped or unavailable native device fails the milestone and remains `NotRun` in the nested GPU
evidence. The native gate is bounded evidence; it does not itself claim full cross-platform or
all-route GPU qualification.

Configure only when explicitly requested with `--configure`. For a new tree, provide an existing
locked prefix through `--dependency-prefix` or `BLOOM_LOCAL_DEPENDENCY_PREFIX`. The configure step
keeps CMake FetchContent disconnected and uses a local ccache launcher; `--no-ccache` is an explicit
fallback if ccache is unavailable. Subsequent milestones reuse the same build directory and cache.

```sh
export BLOOM_LOCAL_DEPENDENCY_PREFIX=/path/to/existing/dependency-prefix
python3 tools/local/sprint_validation.py milestone \
  --level core --build-dir build/sprint-core --configure --jobs 2 \
  --target bloom_runtime_result_ownership_test \
  --test bloom.runtime.result_ownership
python3 tools/local/sprint_validation.py milestone \
  --level desktop --build-dir build/sprint-desktop --configure --all --jobs 2
python3 tools/local/sprint_validation.py milestone \
  --level native --build-dir build/sprint-native --configure --jobs 2
```

The default mode is CMake `qualified`, which currently reports
`Qualified-pending-manifest` provenance rather than claiming completed dependency qualification.
Use `--dependency-mode developer-system` explicitly when working without a prefix; that result is
reported `Unqualified`. A later run without `--configure` must use a build directory with the same
configured dependency mode. For single-configuration generators, `CMAKE_BUILD_TYPE` must match
`--config`; use a separate build directory for another build type. On multi-configuration generators,
pass `--generator` when configuring and `--config` when running.

## Evidence and failure rules

Each invocation writes a unique JSON report below `<build-dir>/local-validation/` unless `--output`
is supplied. Its sibling log directory contains command output, CTest JUnit files, and nested GPU
evidence. The report records the actual Git HEAD, branch, dirty state and SHA-256 of tracked changes
plus non-ignored untracked file contents, before and after validation. A source change during a run
invalidates the result. It also records each runner command, exact arguments, exit code, duration,
timeout state, build configuration and dependency provenance. The native gate's own JSON records its
per-test native statuses.

The runner removes inherited `DISPLAY` and `WAYLAND_DISPLAY` and forces Qt offscreen with Fusion
styling. It does not currently launch a private compositor. A GUI gate that needs one requires a
separately controlled environment and an explicit runner extension; the user's desktop display is
never selected implicitly.

Missing tests, missing JUnit evidence, failed commands, skipped CTest cases and native `NotRun`
cannot produce `Passed`. A lock conflict or unavailable configured build is `NotRun` and exits
nonzero. Timeouts terminate the process group. The runner has no remote execution or Actions
dispatch path: it rejects non-ccache compiler launchers and known remote compiler wrappers, and
clears ccache remote-storage and prefix-command settings for commands it launches.

## What belongs in the default suite

A test proves one behavior at the smallest size that shows it. CPU/GPU parity is checked on a few
small frames; a large size is used only where the size itself is the behavior. The default suite
has no stress runs, no benchmarks, no assertions on elapsed wall time and no test whose result
depends on the memory this machine happens to have free. A test that fails only sometimes is
testing the wrong thing: test the logic directly with the budget, clock or asynchronous step
injected.

`bloom_add_test` (`cmake/BloomTesting.cmake`) registers three kinds of test only when asked:

| Argument | Label | Configure option | For |
|---|---|---|---|
| `STRESS` | `stress` | `BLOOM_ENABLE_STRESS_TESTS` | large sizes, long fills, soaks, real-machine memory |
| `BENCH` | `bench` | `BLOOM_ENABLE_BENCH_TESTS` | benchmark programs kept runnable at a smoke size |
| `COMPOSITOR` | `compositor` | `BLOOM_ENABLE_COMPOSITOR_TESTS` | tests that need a private compositor session |

All three options are off by default and the programs still build. A `COMPOSITOR` test would only
exit as skipped without its session, so it is not registered at all unless the session is provided:
a green default run says nothing about those tests, and does not pretend to.

Shared test support lives in `tests/support`:

- `fixed_memory_budget.hpp`: a constant host memory reading (16 GiB physical, 8 GiB available) in
  the shape of the `HostMemorySampler` seam, and the size of the card a small GPU test plans for.
- `await.hpp` and `await_qt.hpp`: `awaitCondition(name, predicate, limit)` and
  `awaitQtCondition`. The result must be used; a timeout prints the condition's name and counts as
  a failure through `awaitFailureCount()`.
- `manual_clock.hpp`: a clock that moves only when the test moves it, for code that takes its time
  source as a callable.

A test that starts one of Bloom's own programs (`bloom-mcp`, `bloom-cli`, the Python module) cannot
hand it a sampler. Those programs read one test-only environment variable instead, once, at their
first memory sample:

```text
BLOOM_TEST_FIXED_MEMORY_READING=<physical bytes>,<available bytes>,<device bytes>
```

With it set, the host memory sample is that physical and available memory with no process limit,
and every GPU device is created as a card of the declared size, so admission does not move with
what else is running. Without it nothing changes; nothing in the product sets it, no setting maps
to it, and a value that does not parse is reported on stderr and ignored. `bloom_add_test` sets it
(16 GiB, 8 GiB available, an 8 GiB card) for every test whose command names one of those programs
or the built Python module, and for a test registered with `FIXED_MEMORY` because its own code
starts one. Stress tests never get it. The declaration is
`src/core/include/bloom/core/test_fixed_memory_reading.hpp`.

The runner itself has a small local test suite:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools/local -p 'test_*.py'
```
