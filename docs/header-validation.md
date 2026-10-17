# Header cleanup validation

Buildray 0.7 adds header cleanup and optional AST-derived forward declarations.
All eight suites pass locally in Release (378.29 s) and Debug with ASan/UBSan
(417.49 s) before the mixed-language driver correction described below. The local
sandbox requires `ASAN_OPTIONS=detect_leaks=0`; CI uses the default sanitizer
configuration. Installation with
`cmake --install` also places a working CLI and AST helper in the same `bin`
directory, outside the build tree.

The integration fixture checks both cleanup-only and forward-declaration modes,
source preservation, independent patch application followed by a fresh build and
CTest, three paired measurements, path restrictions and excluded files. It also
checks multiple compile contexts, required complete types, attributed/template/
nested/inline namespace types, IWYU suppression, a non-standalone original header,
and an include needed by a C user. Those unsupported or invalid proposals stay
unchanged. A declaration-only candidate that loses a required initialization side
effect compiles but fails CTest; its patch remains unvalidated and unrecommended.

Action integration uses the real native CLI. It checks header-mode inputs,
retention of the report, summary and patch, and ignoring the PR baseline reference
for this generated-patch experiment.

## Corpus

The opt-in `header_corpus` workflow restricts edits to internal headers in the
existing pinned corpus. Each run enables forward declarations and limits accepted changes to four
headers. The default is three alternating clean-build pairs with two jobs;
`header_runs: 1` is available for validation without a performance recommendation. XGrammar and MuJoCo public include trees are outside the selected edit paths;
vendored code is excluded from edits. SentencePiece’s `src` also contains
installed API headers, so its selected findings need individual API review.
The configured default build and CTest remain covered.

| Project | Revision | Edit paths | Configuration |
| --- | --- | --- | --- |
| XGrammar | `7e269d56c74c89de52bc8c31e42191155416371f` | `cpp` | Native C++ tests on, Python bindings off; pinned DLPack and GoogleTest submodules |
| SentencePiece | `e0cce7d37b065b5140349dbe12c6bcf6192fdd78` | `src`, excluding `src/builtin_pb` | Static libraries and CTest on, TCMalloc off; `third_party` excluded |
| MuJoCo | `f640345f72b773e7ec52cd9426a57335cf9c6c28` | `src` | Tests on; examples, simulator UI and Python utility test off; double precision |

Passing this configuration does not cover downstream clients, other build flags
or other platforms. MuJoCo contributions also require a separate single-precision
test run. See [the workflow and limits](header-cleanup.md).

## Mixed C/C++ toolchain regression

The first [MuJoCo header run](https://github.com/kennethassogba/buildray/actions/runs/39032931360)
failed the original project's link before any header analysis or edits.
Its [report](evidence/mujoco-headers-initial-rejected-report.json) records
`phase: baseline_validation` and `patch_validated: false`.
The header experiment selected Clang for C++ while CMake selected the system C
compiler. That combination loses C symbols in this project's LTO link.

`headers` now selects the matching Clang C driver too, records both compiler
versions and checks that both major versions match the AST helper. An explicit
`--c-compiler` remains available. The regression fixture requires CMake to select
Clang for C and enables IPO when supported, with a C function called by C++.
This fixes the experiment setup; it is not a MuJoCo source change or a finding
eligible for an upstream cleanup PR.

Header experiments also reuse the Ninja default-target coverage policy from
`deps`. MuJoCo configures 493 compilation commands but its validated default
build reaches 326; optional dependency targets must not be claimed as executed.
`headers` now instruments C and C++ without dependency-trace flags, checks every
default-build object, and indexes only commands whose outputs are reachable.
The regression fixture includes an optional target that cannot compile, verifies
that it is counted outside the experiment, and rejects a C launcher bypass.
Per-compiler header experiment totals cover both languages.

After the driver and default-target coverage corrections, the header suite passes
again in Release (176.06 s) and Debug with ASan/UBSan (192.45 s), including the
optional failing target and C launcher bypass. The action and dependency suites
also pass in both builds after the driver correction. Both native CI jobs at
`779822e` pass the original eight suites; the final branch checks cover the added
mixed-language regression cases.

## SentencePiece initial result

[Run 39032931360](https://github.com/kennethassogba/buildray/actions/runs/39032931360),
CLI `779822e`, validates eight unused-include removals in four internal headers
on the pinned v0.2.2 configuration. All 236 compile commands are indexed;
97 user/standalone preflight compilations and 45 standalone checks are recorded.
Baseline and candidate build, link and pass the configured CTest suite.
Four cleanup proposals and two forward-declaration proposals fail preflight and
stay unchanged. No forward declaration is accepted on this selected scope.

Three alternating clean-build pairs give medians of 100.336 s before and
102.147 s after (+1.806%); MAD is 0.332 s and 2.607 s. The result is
`inconclusive`, with no recommended patch or established speedup.
The [report](evidence/sentencepiece-headers-initial-report.json),
[findings](evidence/sentencepiece-headers-initial-findings.json) and
[patch](evidence/sentencepiece-headers-initial.patch) are retained.

Before contributing, the eight directives were checked against current upstream
`d6b191ec50dc4302b262688f0250b9b677bb15d2`: all are already absent from their
respective headers. The [check](evidence/sentencepiece-headers-upstream-check.json)
records each one. No duplicate upstream PR is opened for this older-tag result.

## XGrammar transitive-definition regression

The initial [run](https://github.com/kennethassogba/buildray/actions/runs/39032931360)
indexes 44 compilation commands and validates three unused-include removals plus
two forward declarations in four internal headers. Both variants pass all 83
CTest cases. Its [report](evidence/xgrammar-headers-initial-report.json),
[findings](evidence/xgrammar-headers-initial-findings.json) and
[patch](evidence/xgrammar-headers-initial.patch) are retained as the initial result.
Three clean-build pairs give medians of 107.635 s and 107.032 s (−0.560%):
`inconclusive`, with no recommended patch.

Review found that the two forward declarations were unnecessary. `Grammar` still
needs a full definition in those headers, supplied transitively by another
include. Passing standalone and user compilations alone had missed that condition.
The AST helper now vetoes providers used by value or as base classes, and defers
headers containing inline code or compile-time definitions. Those vetoes are
merged across contexts, so a declaration-only context cannot override a context
that needs the definition. The helper protocol is checked to prevent mixing incompatible executables.

Regression fixtures preserve a by-value use and inline member access even when a
transitive include makes their forward-declaration variants compile. The
multi-configuration fixture also supplies the full type transitively, requiring
the AST veto to survive across contexts. The initial cleanup selection keeps only the three justified unused-include
removals. The final local run below finds an additional unused `<climits>` include;
the initial timing above is for the older combined patch and is not a measurement
of the final upstream cleanup.

## Syntax-only argument regression

The second XGrammar experiment at `0e3b76e` incorrectly rejected its valid
unused-include proposals. Its [report](evidence/xgrammar-headers-v2-report.json)
and [findings](evidence/xgrammar-headers-v2-findings.json) show `no_changes` and
38 preflight commands. Replaying the actual compilation locally reproduces
`argument unused during compilation: '-c' [-Werror,-Wunused-command-line-argument]`:
appending `-fsyntax-only` while retaining `-c` conflicts under XGrammar’s `-Werror`.
Removing `-c` makes that same command pass without weakening warnings.
The integration project now treats unused compiler arguments as errors too.

Header runs with no eligible changes still validate both clean builds and the
CTest inventories, but skip timing identical copies. The long MuJoCo experiment
from the affected version completes both 3,496-case test passes, then rejects
the candidate because of the opaque GoogleTest display names described below. A fresh one-pair corpus run will validate the corrected tool;
one pair cannot recommend a performance change. The manual workflow defaults to
three pairs and exposes `header_runs` for these shorter validation runs.

SentencePiece’s second [report](evidence/sentencepiece-headers-v2-report.json)
and [findings](evidence/sentencepiece-headers-v2-findings.json) validate the same
eight-removal patch. Three pairs give medians of 137.215 s and 134.634 s (−1.880%):
`inconclusive` again. These independent runner sessions have different baselines;
compare variants within a run, not absolute times between runs.

## MuJoCo opaque GoogleTest names

The second MuJoCo [report](evidence/mujoco-headers-v2-rejected-report.json)
records 3,496 passing CTest cases in both trees, 326 indexed commands and 167
configured commands outside the default build. Its
[header findings](evidence/mujoco-headers-v2-findings.json) record no accepted
changes; the unused `-c` argument caused false preflight rejections under
`-Werror`. The run then fails the test-name comparison before measurements.

GoogleTest’s default value printer exposes opaque parameter objects as byte dumps,
including pointer addresses and padding. CMake uses those dumps in friendly CTest
names, which differ between independently linked processes even with unchanged
code. The local MuJoCo inventory reproduces names ending in `160-byte object <...>`
and `16-byte object <...>`.

Buildray now canonicalizes only an entire opaque byte-dump parameter on a test
with an actual `--gtest_filter=` command argument. It preserves the test-name
prefix and object size, and appends the exact filter to distinguish parameter
cases. Other names stay strict; identities retain multiplicity. Counts and
bounded mismatch details are recorded. This is inventory validation, not proof
that opaque parameter data is semantically unchanged.

A compiled protocol fixture exercises the real CMake `gtest_discover_tests`
module offline. Its pretty-printed bytes differ between the two executables,
while execution filters stay stable; validation must pass. Changing the filters
with the same test count must still fail. The existing ordinary renamed-test
regression also remains required.

The local 3,496-case MuJoCo inventory contains 153 opaque byte-dump parameter
names, all matching the narrowly supported whole-value format. The focused
CMake discovery regression passes with the corrected comparator. The final local
action, CMake-link and header suites pass in Release (269.24 s) and Debug with
ASan/UBSan (299.86 s), including the `-Werror` argument check, array/template
completeness vetoes and changed GoogleTest filters.

At `18156f6`, all eight suites pass in both native CI jobs (Release and Debug with
ASan/UBSan, including default LeakSanitizer) in
[the PR run](https://github.com/kennethassogba/buildray/actions/runs/39037728471)
and [the push run](https://github.com/kennethassogba/buildray/actions/runs/39037724961).
The final three-include XGrammar cleanup also passes all upstream pre-commit hooks,
Ruff, a fresh standalone Clang 21 Release build and all 83 CTest cases (1.97 s).

## Final XGrammar cleanup

The local run at `18156f6` restricts edits to `cpp/grammar_impl.h`,
`cpp/grammar_builder.h` and `cpp/support/compact_2d_array.h`. The input is a pristine
source copy of the pinned XGrammar revision, with its pinned submodules and native
C++ tests enabled, Python bindings disabled. Linux, Clang/clang-tidy 21.1.8,
CMake 4.2.3, Ninja 1.13.2 and two build jobs are used. Local
`CXXFLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/15` selects the complete
libstdc++ installation; it is not an upstream requirement.

All 44 compile commands are indexed. The patch removes four includes in those
three headers, including the additional `<climits>` removal found in this local
configuration. No forward declaration is accepted. Eighty preflight compilations
check the affected users and standalone contexts. Baseline and candidate build,
link and pass all 83 CTest cases before measurement.

| Three alternating clean-build pairs | Baseline | Candidate |
| --- | ---: | ---: |
| Median | 121.899 s | 123.292 s |
| MAD | 0.046 s | 1.688 s |

The median change is +1.143%. The 1.393 s difference is below the 5.063 s noise
threshold, so the result is `inconclusive` and the patch is not recommended as a
performance optimization. The first candidate sample is 149.737 s; all raw
samples are retained in the [report](evidence/xgrammar-headers-safe-report.json).
The [findings](evidence/xgrammar-headers-safe-findings.json) and
[measured patch](evidence/xgrammar-headers-safe.patch) are also checked in.
No build-time gain is established.

The formatted [upstream draft #994](https://github.com/mlc-ai/xgrammar/pull/994)
contains exactly these four include deletions, plus empty-line cleanup. Both
commits pass their applicable pre-commit hooks. A fresh Release build followed by
all 83 CTest cases (0.79 s) passes after the final formatting and fourth deletion.
The earlier full pre-commit run and Ruff checks also pass; no Python file changes.

The corrected [CI corpus run](https://github.com/kennethassogba/buildray/actions/runs/39037818246)
validates four includes in four XGrammar headers, with 83 passing cases per
variant. Its [report](evidence/xgrammar-headers-ci-report.json),
[findings](evidence/xgrammar-headers-ci-findings.json) and
[patch](evidence/xgrammar-headers-ci.patch) cover a different scope: selection is
allowed throughout `cpp`, and the fourth change removes `support/utils.h` from
`json_schema_converter.h`, while keeping `<climits>`. That run has one pair and
reports `insufficient_samples`. Its timings are not used for the upstream patch.

The same CI run validates SentencePiece’s unchanged eight-removal patch again;
see its [report](evidence/sentencepiece-headers-ci-report.json) and
[findings](evidence/sentencepiece-headers-ci-findings.json). One pair does not
recommend a performance change, and these removals are already present upstream.
