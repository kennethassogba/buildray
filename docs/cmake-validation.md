# CMake link experiment validation

All seven suites pass locally in Release (212.00 s) and Debug with ASan/UBSan
(258.03 s). The local sandbox requires `ASAN_OPTIONS=detect_leaks=0`; CI uses
the default sanitizer configuration. After the final interrupt checks, the unit
and CMake suites are checked again in both builds. The final CMake suite,
including repeated declarations and changed raw linker arguments, passes in
30.62 s (Release) and 33.26 s (ASan/UBSan).

The CMake suite executes real Clang builds and CTest for:

- A private link whose compilation definitions survive through a public interface.
- An unscoped executable declaration and a public library declaration.
- Refusal of link-only or compile-only paths and loss of an exported requirement.
- Multiline calls, comments, bracket arguments, variables, quoted arguments,
  repeated tokens and excluded paths.
- Independent application and compilation of the generated patch.
- A candidate that preserves graph reachability but fails a runtime test.
- Unexpected configured targets and a replaced test with the same test count.
- No eligible link and explicit requests that cannot be justified.

Action integration checks the candidate plan, before/after graphs, patch and
report artifacts. It verifies that PR baseline selection is not used for this
command. The same test-name check protects include cleanup and PCH experiments.

## XGrammar input

The opt-in `link_corpus` workflow evaluates
`mlc-ai/xgrammar@7e269d56c74c89de52bc8c31e42191155416371f`, with its pinned
DLPack and GoogleTest submodules. Root `config.cmake` enables C++ tests and
disables Python bindings. The experiment restricts selection to
`xgrammar_test -> gtest` and excludes `3rdparty` from edits.

```bash
buildray prune-links --source /path/to/xgrammar --compiler clang++-21 \
  --target xgrammar_test --dependency gtest --exclude 3rdparty \
  --runs 3 --jobs 2 --work-dir /tmp/xgrammar-cmake-link
```

In the tested Linux configuration, `gmock` and `gtest_main` both declare `gtest`
as a public link dependency in the pinned GoogleTest source. The experiment
checks that an alternate path preserves
both compilation and linking requirements. This is a configured redundancy;
XGrammar's tests still use GoogleTest. No library or test is deleted.

The formatted [upstream draft #993](https://github.com/mlc-ai/xgrammar/pull/993)
contains only that CMake declaration change, independently of the earlier include
cleanup. All upstream pre-commit hooks and `ruff check` pass. A fresh standalone
Release build with Clang 21.1.8 passes all 83 CTest cases after formatting.
The draft makes no compile-time improvement claim.

## Completed experiment

[Run 39004319353](https://github.com/kennethassogba/buildray/actions/runs/39004319353)
passes both native CI jobs and the real XGrammar experiment at `89205fc`.
The corpus uses Ubuntu 24.04, Clang 21.1.8, CMake 4.2.3 and Ninja
`1.13.0.git.kitware.jobserver-pipe-1`, Release and two jobs.

The selected alternate path is `xgrammar_test -> gmock -> gtest`.
The configured graph keeps all 8 targets and changes from 37 to 35 typed
relationships: the direct link and compilation relationship to `gtest` disappear.
The graph check passes; both variants build, link and pass all 83 CTest cases.

| Three alternating clean-build pairs | Baseline | Candidate |
| --- | ---: | ---: |
| Median elapsed time | 80.054 s | 80.042 s |
| MAD | 0.891 s | 0.362 s |

The elapsed-time change is −0.0151%. The 0.012 s difference is below the 2.672 s
noise threshold, so the result is `inconclusive` and `patch_recommended` is false.
This cleanup establishes no build-time improvement. GoogleTest still builds and
links through the remaining targets. These timings describe this configuration
only; do not compare absolute times with other runner sessions.

The [raw report](evidence/xgrammar-cmake-report.json),
[selection](evidence/xgrammar-cmake-selection.json),
[measured patch](evidence/xgrammar-cmake.patch) and
[graph excerpt](evidence/xgrammar-cmake-graphs-summary.json) are checked in.
Complete graphs remain in that run's `xgrammar-cmake-link` artifact.
The measured patch passes `git apply --check` against the pinned revision.
Upstream formatting removes the extra space left by the token deletion; the
standalone build and test result above covers that formatted diff.
