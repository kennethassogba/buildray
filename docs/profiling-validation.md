# Profiling and PCH validation

Buildray 0.3 is built in C++26. Its five suites pass in Release and with ASan/UBSan
locally and in [GitHub CI at daba8ed](https://github.com/kennethassogba/buildray/actions/runs/38832751253).
The local sanitizer run disables LeakSanitizer because of the sandbox; GitHub uses
the default sanitizer configuration.

The profiling suite uses actual Clang traces and a CMake project with two C++
files, a C file, and a target declared in a subdirectory. It checks repeated-header
counts, source preservation, PCH creation and use, and applying the generated patch
outside Buildray. Bad headers and a PCH macro that makes CTest fail are rejected.
Inherited interface PCH headers are also rejected. Unit cases cover complete and
asynchronous trace events, nested costs, repeated events and unmatched events.
Trace aggregation also honors cancellation before reading the next compiler record.
The Action tests exercise both new modes and their retained outputs.

## tinyxml2

Both runs use revision `9148bdf719e997d1f474be6bcc7943881046dba1`, Clang 21, two
build jobs, three alternating clean-build pairs, and a PCH containing `<string>`
and `<vector>` on `xmltest`. Both variants build and pass `xmltest`.

| CI run | Baseline median | PCH median | Change | Verdict |
|---|---:|---:|---:|---|
| [38831417833](https://github.com/kennethassogba/buildray/actions/runs/38831417833) | 0.907889 s | 1.024971 s | +12.896% | regressed |
| [38831824796](https://github.com/kennethassogba/buildray/actions/runs/38831824796) | 0.825617 s | 0.884475 s | +7.129% | inconclusive |

Neither patch is recommended. The second run's difference falls inside its
larger noise threshold despite its higher candidate median. The profile has two
C++ compilations across two targets, with one file per target; this offers little
reuse for a target-specific PCH. Raw reports are retained in
[the first run](evidence/tinyxml2-pch-initial-report.json) and
[the later run](evidence/tinyxml2-pch-report.json).

## SentencePiece profile

[Run 38831428929](https://github.com/kennethassogba/buildray/actions/runs/38831428929)
profiles SentencePiece v0.2.2, revision
`e0cce7d37b065b5140349dbe12c6bcf6192fdd78`, using Buildray commit
`1cb29bca8210f0120972702889633875b1e85cc5`. All 236 C++ compiler invocations are
captured and the project's CTest suite passes.

`src/common.h` appears in 50 compilations, with 49.691 inclusive seconds in the
instrumented build. The `spm_test` target accounts for 24 of those compilations
and 23.131 inclusive seconds. These times overlap with nested headers and include
trace overhead; they are not predicted savings. The full rankings are retained in
that run's `sentencepiece-v0.2.2-headers` artifact. A
[top-header excerpt](evidence/sentencepiece-header-profile-excerpt.json) is checked in.
The workflow also measures a separate standard-header PCH on `sentencepiece-static`;
its report is retained in the run's `sentencepiece-v0.2.2-pch` artifact.

The profile motivates testing a `src/common.h` PCH on `spm_test` separately from
the standard-header PCH experiment on `sentencepiece-static`.

## SentencePiece PCH result

[Run 38832287678](https://github.com/kennethassogba/buildray/actions/runs/38832287678)
uses Buildray commit `b2f78c509e2c1de22469a3e7a0333fdad52f92cb`, the same pinned
SentencePiece v0.2.2 input, Clang 21, two jobs and three alternating clean-build pairs.
`SPM_BUILD_TEST=ON`, `SPM_ENABLE_SHARED=OFF`, and `SPM_ENABLE_TCMALLOC=OFF` are shared
by both variants. Only `spm_test` receives a private PCH containing `src/common.h`.
The measurement covers the entire default build, including dependencies, tools,
the test executable and PCH generation. Both variants pass the CTest suite.

| Variant | Raw clean-build seconds | Median | MAD |
|---|---|---:|---:|
| Baseline, no PCH | 97.010516, 97.315543, 96.035739 | 97.010516 s | 0.305026 s |
| Candidate | 87.902786, 88.090022, 89.770651 | 88.090022 s | 0.187236 s |

The measured reduction is **9.195%**, or 8.920494 seconds. It exceeds the run's
1.940210-second noise threshold, so Buildray reports `improved` and recommends
the validated patch. This is a clean-build result for this configuration with
tests enabled; it is not a prediction for builds without `spm_test`, other
platforms or incremental builds. Three pairs and a heuristic threshold do not
establish statistical significance.

The [raw report](evidence/sentencepiece-common-pch-report.json) and
[CMake patch](evidence/sentencepiece-common-pch.patch) are retained. The patch also
passes `git apply --check` against the pinned revision's original `CMakeLists.txt`.

Reproduce after checking out SentencePiece v0.2.2 in `fixtures/sentencepiece`:

```bash
buildray pch --source fixtures/sentencepiece --compiler clang++-21 \
  --target spm_test --header src/common.h --runs 3 --jobs 2 \
  -- -DSPM_BUILD_TEST=ON -DSPM_ENABLE_SHARED=OFF -DSPM_ENABLE_TCMALLOC=OFF
```

## Upstream include cleanup

The [SentencePiece draft PR](https://github.com/google/sentencepiece/pull/1350)
targets current `master` at `ab49d75a87c17affa43a158c3396eb237d2422ed`. Most of the
older include proposals no longer apply. It removes only `<map>` from
`src/unigram_model.cc` and `model_factory.h` from `src/trainer_interface.cc`.
Clang-tidy 21 confirms both as unused using the original files and the current
compilation database through a read-only VFS overlay.

The candidate completes the full Release build and all four CTest suites on Linux
with Clang 21, `SPM_BUILD_TEST=ON`, `SPM_ENABLE_SHARED=OFF`, and
`SPM_ENABLE_TCMALLOC=OFF`. The default Lite targets are included. This is separate
from the v0.2.2 PCH benchmarks and makes no compilation speedup claim. Google's
CLA check requires the contributor's agreement to be signed or associated before
the draft could be merged.
