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
existing pinned corpus. Each run enables forward declarations, limits accepted
changes to four headers, and uses three alternating clean-build pairs with two
jobs. Public API headers and vendored code are outside the selected edit paths;
the configured default build and CTest remain covered.

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
