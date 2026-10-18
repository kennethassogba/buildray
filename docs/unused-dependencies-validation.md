# Unused dependency cleanup: measured results

The [corpus run](https://github.com/kennethassogba/buildray/actions/runs/39084953086)
uses the native `prune-deps` CLI through the GitHub Action. The
[workflow](../.github/workflows/unused-dependencies.yml) pins XGrammar,
SentencePiece and MuJoCo, enables their C++ tests and records the complete flags.

Both variants use Clang 21 and ELF LLD explicitly. MuJoCo normally selects gold
in some targets; these results describe the common LLD profile, not its default
linker. PCH, Unity and compiler caches are disabled. Configuration, analysis,
link replays and tests are outside build timings.

## XGrammar: valid removal, no measured speedup

Revision `7e269d56c74c89de52bc8c31e42191155416371f` is also upstream main when
checked on 18 October 2026. Python bindings are disabled and C++ tests enabled.

Deleting `gmock` outright changes compiler usage requirements and is rejected.
The second strategy keeps them with `COMPILE_ONLY`. LLD reports one archive
member and zero extractions from `libgmock.a`. Replaying the modified link with
the original objects produces a byte-identical ELF binary. Both variants pass
all 83 CTest cases, including after each timed incremental edit.

| Three alternating clean-build pairs | Baseline | Candidate |
| --- | ---: | ---: |
| Median build time | 102.040 s | 102.004 s |
| MAD | 0.021 s | 0.054 s |
| Median link-driver duration | 31.293 s | 31.325 s |
| Compiler invocations per build | 44 | 44 |
| Link invocations per build | 1 | 1 |

The clean-build difference is −0.035%, below the noise threshold. Editing
`cpp/earley_parser.cc` gives medians of 34.668 s and 34.587 s, also inconclusive.
Each variant recompiles one file and links once. GoogleMock still belongs to
the default build, so removing this link saves no compilation here.

The link command includes `-flto=auto`; its roughly 31 seconds dominate this
incremental edit. Removing an archive that supplies no objects does not remove
that LTO work. No upstream PR is proposed for this patch without a measured gain.

The [raw report](evidence/xgrammar-unused-dependencies-report.json),
[plan and evidence](evidence/xgrammar-unused-dependencies-plan.json) and
[measured patch](evidence/xgrammar-unused-dependencies.patch) are retained. The
run used Buildray `bf4c3ff`; subsequent fixes accept static-only builds and
preserve indentation in generated CMake wrappers. They do not change this link
experiment's semantics.

## SentencePiece: baseline only

Revision `e0cce7d37b065b5140349dbe12c6bcf6192fdd78` uses tests enabled, shared
libraries disabled and TCMalloc disabled, with two build jobs. Its CTest entry
passes. Three clean builds take 180.555 s, 180.097 s and 180.097 s. The median
sum of its six link-driver invocations is 0.313 s.

There is no eligible literal private dependency in this configuration. The
project uses aliases for its static libraries and exports requirements through
interfaces and variable lists. Those cases are outside this implementation.
This is a baseline profile, with no candidate and no claimed gain. It does not
establish that every dependency is necessary.

The [raw report](evidence/sentencepiece-unused-dependencies-report.json) and
[selection plan](evidence/sentencepiece-unused-dependencies-plan.json) are retained.

## MuJoCo: no removal confirmed

Revision `651bae11c3c38efe483f033b4b7e869e7ce3f4ec` uses four build jobs, tests
enabled and examples, simulate and Python test utilities disabled. Its baseline
passes 3,519 CTest cases. None of the 14 attempted strategies confirms a removal.
The workflow retains baseline-only clean and link-driver measurements, rejected
trial reasons and complete test inventories in `unused-mujoco`.

## Regression coverage

The native suite builds real mixed C/C++ fixtures with LLD. It checks unused and
required archives, whole-archive behavior, compilation requirements, identical
ELF replay, changed test inventories and unrelated work removed by CMake callbacks.
It applies generated patches independently and checks that the original tree is
unchanged. An unused-library edit drops from one compilation and one link to
zero of each in the candidate.

Static-only projects also pass with explicit build-only validation and zero
link-driver invocations. The compile-only wrapper is tested inside an indented,
multiline CMake declaration. Its fallback is built and tested locally with an
actual CMake 3.26.4 executable, as well as the current engine in CI.
