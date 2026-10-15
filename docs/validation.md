# Local validation — 2026-10-16

Buildray 0.1.0 was compiled in C++26 mode with Clang/LLVM 21.1.8, CMake 4.2.3
and Ninja 1.13.2 on Linux x86_64. The Release CLI was installed into a local
prefix and that installed executable was used for the real-project run below.

## Real project

[tinyxml2 11.0.0](https://github.com/leethomason/tinyxml2/tree/9148bdf719e997d1f474be6bcc7943881046dba1),
commit `9148bdf719e997d1f474be6bcc7943881046dba1`, with its original CTest suite enabled.
The input checkout was unmodified. No includes were added to manufacture findings.

```bash
git clone --depth 1 --branch 11.0.0 https://github.com/leethomason/tinyxml2.git /tmp/buildray-tinyxml2
buildray optimize \
  --source /tmp/buildray-tinyxml2 \
  --work-dir /tmp/tinyxml2-result \
  --compiler /usr/bin/clang++-21 \
  --clang-tidy /usr/bin/clang-tidy-21 \
  --runs 7 --jobs 2 \
  -- -Dtinyxml2_BUILD_TESTING=ON \
     -DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/15
git -C /tmp/buildray-tinyxml2 apply --check /tmp/tinyxml2-result/includes.patch
python3 tests/check_real_project.py /tmp/tinyxml2-result/report.json
```

The `--gcc-install-dir` flag is specific to this host: Clang otherwise selected
an incomplete GCC 16 installation that lacked the C++ library. GCC 15 supplies
the installed headers and library. Omit this flag on a correctly configured host.
Buildray preserves compiler driver names when resolving explicit paths:
`clang++` can be a symlink to `clang`, but invoking it under the latter name
would change C++ linking behavior.

The actual local work directory is recorded in the
[raw report](evidence/tinyxml2-report.json). The commands above use a portable
output location; measurements have not been rewritten.

| Check | Observed result |
| --- | --- |
| C++ translation units analyzed and measured | 2 |
| Includes removed | 3, all in `xmltest.cpp` |
| Removed headers | `<cerrno>`, `<sys/stat.h>`, `<sys/types.h>` |
| Baseline validation | Cleanly configured build; 1 CTest test passed |
| Candidate validation | Clean rebuild; the same CTest test passed |
| Original checkout | Unchanged |
| Unified patch | `git apply --check` passed against the original checkout |
| Samples | 7 clean builds per variant, 2 jobs, alternating order |
| Baseline median / MAD | 1.746646321 s / 0.453194294 s |
| Candidate median / MAD | 1.766281941 s / 0.441715329 s |
| Median wall-time change | +1.124% |
| Performance result | `inconclusive` |
| Patch validation / recommendation | Validated; not recommended as a measured speedup |

These removals passed the configured build and tests. **This run did not
demonstrate a build-time gain.** The variation is much larger than the median
difference. The first milestone establishes a working measurement and validation
loop; identifying unused directives does not guarantee that their removal saves
wall time, especially when headers remain transitively included.

Evidence: [report and all raw samples](evidence/tinyxml2-report.json),
[generated patch](evidence/tinyxml2.patch). Full compiler records, tool logs,
CTest XML and both source/build variants remain in the local work directory.

## CLI tests

The final Release and ASan/UBSan builds each passed both CTest suites:

- Unit suite: structured YAML fixes, replacement bounds and overlap rejection,
  CRLF preservation, IWYU protection, statistics, process status and literal argv.
- Integration suite: real Clang include removal and Git patch application,
  unchanged input sources, no-op behavior, measurement-only mode, multiple
  compilation configurations, build failures, runtime test failures, missing and
  skipped tests, launcher coverage, PCH/unity overrides, symlinks and interruption.

Analyzer stubs are used only to force two negative cases: a proposed deletion
that breaks compilation, and a deletion of a registration header that compiles
but makes the test fail. Neither is used for successful cleanup or benchmark data.

```bash
ctest --test-dir build-release --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

The local sandbox prevents LeakSanitizer from inspecting processes under ptrace.
Leak detection was therefore disabled for the local sanitizer run; address and
undefined-behavior checks remained enabled. No leak-detection result is claimed.
The GitHub workflow leaves sanitizer defaults enabled.

The installed binary also passed `buildray --version` and the complete tinyxml2
run above. C++ formatting and Python/CI YAML syntax checks passed locally.
