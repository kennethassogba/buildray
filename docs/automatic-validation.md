# Automatic PCH and include-cleanup validation

Buildray 0.5 remains C++26. All six suites pass locally in Release (202.24 s)
and Debug with ASan/UBSan (243.81 s). The local sandbox requires
`ASAN_OPTIONS=detect_leaks=0`; GitHub CI uses its default sanitizer configuration.
Both native jobs also pass at `23a36d7` in
[run 38985618121](https://github.com/kennethassogba/buildray/actions/runs/38985618121).

Fixtures cover deterministic automatic selection, target restriction, absent
candidates, source preservation, work directories inside the input tree, and a
PCH that changes program behavior and fails CTest. Action tests check inputs and
retained selection artifacts. Include-cleanup tests exercise deletion proposals
that fail compilation, independent valid files, excluded paths, linking failures
and runtime failures. The namespace-only false-positive fixture supplies an
analyzer proposal explicitly; it does not depend on a particular clang-tidy
version reproducing that diagnostic.

## XGrammar include cleanup

Input: `mlc-ai/xgrammar@7e269d56c74c89de52bc8c31e42191155416371f`, with the
pinned DLPack and GoogleTest submodules described in
[the dependency evaluation](dependency-validation.md#xgrammar). Root
`config.cmake` enables C++ tests and disables Python bindings.

```bash
CXXFLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/15 \
buildray optimize --source /path/to/xgrammar \
  --compiler clang++-21 --clang-tidy clang-tidy-21 \
  --exclude 3rdparty --runs 3 --jobs 2 \
  --work-dir /tmp/xgrammar-cleanup
```

The local run uses Linux, Clang/clang-tidy 21.1.8, CMake 4.2.3 and Ninja 1.13.2,
with Release builds and two jobs. The explicit GCC directory selects the complete
local libstdc++ installation; it is not an upstream build requirement.

Analysis covers 40 first-party C++ compilation commands. Four vendored commands
are excluded from edits; all 44 remain covered by complete builds. Eighteen
preflight compilations reject four files: `test_json.cc`,
`test_jump_forward_tokens.cc`, `test_lark_converter.cc` and
`test_thread_safe_cache.cc`. Those files stay unchanged. The accepted patch
removes 17 includes across 14 first-party files. Baseline and candidate both
build, link and pass all 83 CTest cases.

The rejected thread-safe-cache proposal removed every include while leaving
`using namespace xgrammar;`, producing `expected namespace name`. An earlier
Buildray run stopped at that candidate build failure. The per-file check now
rejects that proposal before assembling the complete candidate.

Three alternating clean-build pairs give median elapsed times of 147.763 s before
and 146.908 s after cleanup (−0.579%). MAD is 0.731 s and 1.514 s respectively.
The 0.855 s difference is below the 4.541 s noise threshold, so the result is
`inconclusive` and `patch_recommended` is false. No speedup is established.
The [raw report](evidence/xgrammar-cleanup-report.json) retains all six samples
and the [measured patch](evidence/xgrammar-cleanup-includes.patch).
Upstream formatting removes five additional empty lines between include groups;
the benchmark above refers to the original generated patch.

The formatted [upstream draft #992](https://github.com/mlc-ai/xgrammar/pull/992)
passes the full upstream pre-commit hooks, `ruff check`, a fresh Release build
and all 83 CTest cases. It makes no compile-time improvement claim.

## SentencePiece automatic PCH

The opt-in workflow evaluates automatic selection on
`google/sentencepiece@e0cce7d37b065b5140349dbe12c6bcf6192fdd78` (v0.2.2).
The target restriction is explicit; the header choice comes from a new profile.

```bash
buildray pch --auto --target spm_test --source /path/to/sentencepiece \
  --compiler clang++-21 --runs 3 --jobs 2 \
  --work-dir /tmp/sentencepiece-auto-pch -- \
  -DSPM_BUILD_TEST=ON -DSPM_ENABLE_SHARED=OFF -DSPM_ENABLE_TCMALLOC=OFF
```

This configuration builds the native static libraries and tools and runs the
upstream `spm_test` CTest suite. It does not validate Python bindings, shared
libraries or other platforms. Selection profiling is outside all benchmark
pairs; PCH generation and the complete default build remain inside candidate
elapsed time. The input source tree is preserved.

The completed [run 38985618121](https://github.com/kennethassogba/buildray/actions/runs/38985618121)
uses Ubuntu 24.04, Clang 21.1.8, CMake 4.2.3 and Ninja
`1.13.0.git.kitware.jobserver-pipe-1`, with two jobs.
The new profile selects `src/common.h` for `spm_test`: 24 compiler invocations
and 22.616 s of overlapping inclusive parse cost. This score is not a benchmark.
The selected PCH passes baseline/candidate builds and the CTest suite; actual
compiler records confirm PCH creation and use.

| Three alternating clean-build pairs | Baseline | Selected PCH |
| --- | ---: | ---: |
| Median elapsed time | 177.984 s | 160.828 s |
| MAD | 0.006 s | 0.035 s |

The measured change is **−9.639%**. The 17.156 s difference exceeds the 3.560 s
noise threshold, so `performance_result` is `improved` and `patch_recommended`
is true. This is a result for the configuration above, not an incremental-build
estimate or a claim about other targets and platforms. Different runner sessions
have different baselines; do not compare these absolute times to earlier runs.

The [raw report](evidence/sentencepiece-auto-pch-report.json),
[selection](evidence/sentencepiece-auto-pch-selection.json) and
[patch](evidence/sentencepiece-auto-pch.patch) are checked in. The generated patch
also passes `git apply --check` against the pinned tag's `CMakeLists.txt`.
The complete new header profile remains in the workflow artifact
`sentencepiece-v0.2.2-pch`.
