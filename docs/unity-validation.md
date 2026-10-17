# Unity build validation

Buildray 0.8 adds target-scoped, opt-in CMake unity experiments. At `d2bbf7a`,
all nine native suites pass in Release and Debug with ASan/UBSan in both
[PR CI](https://github.com/kennethassogba/buildray/actions/runs/39046880572)
and [push CI](https://github.com/kennethassogba/buildray/actions/runs/39046877682).
CI uses the default LeakSanitizer configuration; local sanitizer runs require
`ASAN_OPTIONS=detect_leaks=0` in the sandbox.

The real C/C++ integration fixture checks independent application of the opt-in
patch, both enabled and disabled builds, source preservation, per-file settings,
directory exclusions and the same source/target multiset. It rejects name
collisions, runtime changes, launcher bypasses, changed source coverage and
ineffective grouping, including module-scanned sources. The Action tests use the
real CLI and verify retained patch, plan and report outputs.

After directory exclusions were added, the local Action and unity suites pass in
Release (67.94 s) and with ASan/UBSan (73.49 s). A further regression keeps an
unrelated build directory out of an exclusion's expansion, including a generated
filename that would otherwise be unsafe in CMake. The updated unity suite passes
in Release (29.23 s) and with ASan/UBSan (32.33 s).

## XGrammar: measured improvement

The successful [XGrammar job](https://github.com/kennethassogba/buildray/actions/runs/39045707762/job/117433883758)
uses CLI `20648e9` and upstream
`7e269d56c74c89de52bc8c31e42191155416371f`, with its pinned DLPack and GoogleTest
submodules. Root `config.cmake` enables native C++ tests and disables Python
bindings. The environment is Ubuntu 24.04 x86_64, Clang 21.1.8, CMake 4.2.3 and
Ninja 1.13, with two build jobs and Release builds. Existing project LTO settings
stay in place. Compiler caches and PCH are disabled in both variants.

```bash
buildray unity --source /path/to/xgrammar --target xgrammar \
  --batch-size 4 --compiler clang++-21 --runs 3 --jobs 2
```

The target's 29 sources form eight unity translation units. Across the complete
default build, compiler invocations drop from 44 to 23. Original source/target
coverage is verified, and both variants build, link and pass all 83 CTest cases
before measurement. The tests and dependency targets remain separately compiled.

| Three alternating clean-build pairs | Baseline | Unity candidate |
| --- | ---: | ---: |
| Median complete build | 105.348 s | 85.882 s |
| MAD | 0.150 s | 0.091 s |
| Highest compiler peak RSS across samples | 376,160 KiB | 520,652 KiB |

The reduction is **18.477%**, or 19.465 s. It exceeds the run's 2.107 s noise
threshold, so the report records `improved`, `patch_validated: true` and
`patch_recommended: true`. The largest per-compiler memory peak increases from
about 367 MiB to 508 MiB. This is not aggregate machine memory.

The [raw report](evidence/xgrammar-unity-initial-report.json),
[verified groups](evidence/xgrammar-unity-initial-plan.json) and
[measured patch](evidence/xgrammar-unity-initial.patch) are retained. Cleaning,
configuration, analysis and tests are outside the timed interval; linking and
all compilation remain inside. Three pairs and the noise heuristic do not imply
statistical significance. No incremental-build or cross-platform gain is claimed.

The [upstream draft #995](https://github.com/mlc-ai/xgrammar/pull/995) uses the
project-specific option `XGRAMMAR_ENABLE_UNITY_BUILD`, off by default, rather than
Buildray's generic option and validation guards. Its eight generated groups are
checked against the measured groups. The formatted patch at
`f95f769b2eca1ca10a2820ff6566eca20efddc23` passes a fresh enabled Release build
and all 83 CTest cases, then a disabled rebuild and the same 83 cases. The disabled
configuration has 44 individual compile commands and no unity sources. Full
pre-commit hooks and Ruff pass. The
[upstream check](evidence/xgrammar-unity-upstream-check.json) records this separate
validation. Local `CXXFLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/15`
selects the complete installed libstdc++; this is not an upstream requirement.

## Corpus candidates rejected before measurement

The initial manual run also tests pinned SentencePiece v0.2.2
(`e0cce7d37b065b5140349dbe12c6bcf6192fdd78`) and MuJoCo
(`f640345f72b773e7ec52cd9426a57335cf9c6c28`). Both baselines build and pass
their configured tests. Both initial unity candidates fail compilation and remain
unvalidated and unrecommended. There are no performance samples for these failed
candidates. The manual run therefore fails overall, even though the independent
XGrammar job succeeds. Its older Action fixture also needed module scanning
disabled for its module-free sources; the native checkpoint above includes that
correction.

SentencePiece's combined Protobuf-lite sources redefine `port_def.inc` macros.
The [rejected report](evidence/sentencepiece-unity-initial-report.json),
[candidate patch](evidence/sentencepiece-unity-initial.patch) and
[error excerpts](evidence/sentencepiece-unity-initial-errors.txt) are retained.
The next experiment keeps `third_party` and `src/builtin_pb` separate. This
configuration already passes both local builds and the CTest suite. The local
run was intentionally interrupted during its first measurement to move complete
timings to a dedicated [CI run](https://github.com/kennethassogba/buildray/actions/runs/39047169786);
no partial local timings are used.

MuJoCo's baseline passes 3,496 CTest cases. Its initial candidate combines
`user_composite.cc` and `user_flexcomp.cc`, whose file-local `comperr` functions
collide. The [rejected report](evidence/mujoco-unity-initial-report.json),
[candidate patch](evidence/mujoco-unity-initial.patch) and
[error excerpts](evidence/mujoco-unity-initial-errors.txt) are retained.
The [follow-up run](https://github.com/kennethassogba/buildray/actions/runs/39046879895)
keeps `src/user/user_flexcomp.cc` separately compiled. That candidate is also
rejected before measurement: `xml_native_reader.cc` and `xml_native_writer.cc`
include the same generated `mjcf_read_table.inc` definitions in one translation
unit. The [second report](evidence/mujoco-unity-isolated-report.json),
[patch](evidence/mujoco-unity-isolated.patch) and
[error excerpts](evidence/mujoco-unity-isolated-errors.txt) are retained. Its
baseline passes all 3,496 tests, but the candidate does not reach CTest. Neither
MuJoCo layout establishes a speedup; no upstream patch is published. No source
symbol is renamed.
MuJoCo uses double precision, tests on, examples/simulator/Python utility tests
off. Upstream contributions need current-source validation in both precision
modes, in addition to this pinned experiment.


## SentencePiece: validated, no established speedup

The dedicated [follow-up run](https://github.com/kennethassogba/buildray/actions/runs/39047169786)
at `d2bbf7a` completes with the two directory exclusions. Both variants build,
link and pass the configured CTest suite. Of the selected target’s 39 sources,
11 first-party sources form three groups; the others stay separate. Complete
default-build compiler invocations drop from 236 to 228, with the same original
source/target multiset. Native static libraries and tests are enabled; shared
libraries and TCMalloc are disabled. Python bindings are outside this experiment.

| Three alternating clean-build pairs | Baseline | Unity candidate |
| --- | ---: | ---: |
| Median complete build | 125.084 s | 120.148 s |
| MAD | 0.281 s | 2.915 s |

The median change is −3.946%, but the 4.936 s difference is below the 8.744 s
noise threshold. The result is `inconclusive`, with `patch_validated: true`
and `patch_recommended: false`. No speedup or new upstream PR is claimed for
this older-tag experiment. The [report](evidence/sentencepiece-unity-isolated-report.json),
[verified groups](evidence/sentencepiece-unity-isolated-plan.json) and
[patch](evidence/sentencepiece-unity-isolated.patch) retain the complete samples
and selection.
