# Incremental rebuild validation

The experiments compare the default build after editing one file in an isolated
snapshot. Each row uses three alternating baseline/candidate pairs. Editing,
readiness checks, restoration and CTest are outside the timer; compilation and
linking are inside. All variants use Release builds, warm filesystem caches and
disabled compiler caches. Repeated clean-build timings are skipped with
`--incremental-only`; the initial full builds and tests still validate each patch.

These are comment-only edits that measure dependency invalidation. Results apply
to the selected files and build configurations. Different jobs have their own
runners and baselines; do not compare their absolute times as a head-to-head
Unity/PCH ranking. The result labels use the documented MAD noise heuristic,
not a statistical significance test.

## Inputs

| Project | Revision | Target | PCH header |
| --- | --- | --- | --- |
| XGrammar | `7e269d56c74c89de52bc8c31e42191155416371f` | `xgrammar` | `cpp/grammar_impl.h` |
| SentencePiece v0.2.2 | `e0cce7d37b065b5140349dbe12c6bcf6192fdd78` | `sentencepiece-static` | `src/common.h` |
| MuJoCo | `651bae11c3c38efe483f033b4b7e869e7ce3f4ec` | `mujoco` | `include/mujoco/mujoco.h` |

The implementation/internal/public file choices are:

- XGrammar: `cpp/earley_parser.cc`, `cpp/grammar_impl.h`, `include/xgrammar/grammar.h`.
- SentencePiece: `src/sentencepiece_processor.cc`, `src/common.h`, `src/sentencepiece_processor.h`.
- MuJoCo: `src/user/user_model.cc`, `src/user/user_model.h`, `include/mujoco/mujoco.h`.

XGrammar enables native C++ tests and disables Python bindings. SentencePiece
builds native static libraries and tests, with shared libraries and TCMalloc
disabled. MuJoCo uses double precision, tests on, examples/simulator/Python
utility tests off. The workflow prepares its required German locale. Unity uses
batch size four and up to four automatic isolation retries. SentencePiece keeps
`third_party` and `src/builtin_pb` outside Unity groups. PCH and Unity affect only
the listed target; all default-build targets remain inside the measured command.

The completed XGrammar and SentencePiece jobs use two compiler jobs, Clang 21,
CMake 4.2.3 and Ninja 1.13 on Ubuntu 24.04. The MuJoCo rerun uses four compiler
jobs for both variants on a runner with four CPUs. Its final configuration also
sets `CMAKE_LINK_DEPENDS_USE_LINKER=OFF` in both variants, for the readiness issue
described below. Those incremental results are pending.

## Completed measurements

Compiler calls include PCH creation. Input-source counts expand Unity groups and
count each original input file once, excluding PCH wrappers. The first reports
were recorded at Buildray `f3440f1`; their input-source counts below are derived
from saved compiler sources and the verified Unity plan. The CLI now emits these
counts directly. Every timed rebuild in these jobs passes the original CTest
inventory: 83 entries for XGrammar and the `spm_test` entry for SentencePiece.

| Project | Strategy / edit | Before (s) | After (s) | Change | Result | Compiler calls | Input sources |
| --- | --- | ---: | ---: | ---: | --- | ---: | ---: |
| xgrammar | unity / implementation | 26.245 | 29.676 | +13.07% | regressed | 1 → 1 | 1 → 4 |
| xgrammar | unity / internal header | 58.397 | 50.094 | -14.22% | improved | 25 → 10 | 25 → 31 |
| xgrammar | unity / public header | 61.464 | 53.019 | -13.74% | improved | 30 → 15 | 30 → 36 |
| xgrammar | pch / implementation | 34.899 | 33.594 | -3.74% | improved | 1 → 1 | 1 → 1 |
| xgrammar | pch / internal header | 81.965 | 72.709 | -11.29% | improved | 25 → 33 | 25 → 32 |
| xgrammar | pch / public header | 86.337 | 78.147 | -9.49% | improved | 30 → 38 | 30 → 37 |
| sentencepiece | unity / implementation | 6.959 | 8.667 | +24.55% | regressed | 1 → 1 | 1 → 4 |
| sentencepiece | unity / internal header | 98.974 | 90.197 | -8.87% | improved | 50 → 42 | 50 → 50 |
| sentencepiece | unity / public header | 97.423 | 85.517 | -12.22% | improved | 50 → 42 | 50 → 50 |
| sentencepiece | pch / implementation | 3.933 | 3.257 | -17.19% | improved | 1 → 1 | 1 → 1 |
| sentencepiece | pch / internal header | 50.036 | 51.494 | +2.91% | inconclusive | 50 → 79 | 50 → 78 |
| sentencepiece | pch / public header | 51.981 | 49.117 | -5.51% | improved | 50 → 50 | 50 → 50 |

Unity recompiles four input sources after the selected implementation-file edit
in both XGrammar and SentencePiece. Each still has one compiler invocation, so
an invocation count alone would hide that extra work. These scenarios regress
even though their header scenarios improve. SentencePiece's PCH regenerates and
increases compiler calls from 50 to 79 after the internal-header edit, with no
established timing improvement for that case.

## MuJoCo readiness check

The [initial PCH job](https://github.com/kennethassogba/buildray/actions/runs/39063875317/job/117487317262)
and [initial Unity job](https://github.com/kennethassogba/buildray/actions/runs/39063875317/job/117487317269)
both build their variants and pass all 3,519 CTest entries in each. Both then
refuse to time the first probe: the untouched baseline relinks 81 targets during
its readiness check. No incremental timing samples are accepted from these attempts.
The [report](evidence/mujoco-pch-incremental-initial-report.json) and
[readiness log](evidence/mujoco-pch-incremental-initial-noop.log) retain the rejection.
The [Unity report](evidence/mujoco-unity-incremental-initial-report.json) and
[readiness log](evidence/mujoco-unity-incremental-initial-noop.log) retain the same
failure after three automatic isolation retries and successful candidate tests.

Readiness now allows two untimed settling attempts, but still requires a real
no-op build before every measured edit. Persistent work remains an error. Each
attempt retains compiler records and a Ninja dependency explanation. A regression
fixture exercises a one-off library timestamp change that needs one untimed
relink; the existing always-running-target fixture still rejects the experiment.
The [bounded-readiness rerun](https://github.com/kennethassogba/buildray/actions/runs/39066408970)
still rejects Unity after three builds keep relinking the same untouched baseline.
Settling cannot make a persistently dirty dependency graph valid for timing.

The [library-only probe](https://github.com/kennethassogba/buildray/actions/runs/39069068276)
isolates the cause without compiling or running test binaries. The actual link
command selects `-fuse-ld=gold`, while the cache records `/usr/bin/ld` as the
default linker. Its generated dependency file includes temporary
`/tmp/lto-llvm-*.o` objects, and Ninja marks them dirty on every control build.
This matches the [documented gold/LTO dependency-file issue](https://cmake.org/cmake/help/v4.2/variable/CMAKE_LINK_DEPENDS_USE_LINKER.html).

With `CMAKE_LINK_DEPENDS_USE_LINKER=OFF`, all three subsequent controls report no
work. Compiler optimization flags and the selected gold linker remain unchanged.
The [probe report](evidence/mujoco-linker-idle-probe-report.json),
[cache summary](evidence/mujoco-linker-idle-cache-summary.json),
[generated dependencies](evidence/mujoco-linker-idle-generated-deps.txt), and
the `mujoco-linker-idle-*-*.log` files retain the evidence. This is a readiness
diagnosis, not a runtime validation or a Unity/PCH speed result.

The [controlled rerun](https://github.com/kennethassogba/buildray/actions/runs/39069068276)
uses that common flag for both baseline and candidate. Compiler header
dependencies, complete source coverage and all 3,519 CTest entries remain
required. Its incremental measurements are pending; no timing from a refused
readiness check is used.

## Evidence and reproduction

The completed measurements come from independent jobs in
[run 39063875317](https://github.com/kennethassogba/buildray/actions/runs/39063875317).
The run also contains the rejected MuJoCo attempt, so its overall status must not
be used as proof of success for every project. Completed job links:

- [XGrammar / Unity](https://github.com/kennethassogba/buildray/actions/runs/39063875317/job/117487317222).
- [XGrammar / PCH](https://github.com/kennethassogba/buildray/actions/runs/39063875317/job/117487317186).
- [SentencePiece / Unity](https://github.com/kennethassogba/buildray/actions/runs/39063875317/job/117487317292).
- [SentencePiece / PCH](https://github.com/kennethassogba/buildray/actions/runs/39063875317/job/117487317399).

Each matching `evidence/<project>-<mode>-incremental-report.json` retains all
samples and validation counts; its `.patch` retains the measured candidate.
Action artifacts retain individual compiler records, inventories, JUnit files,
no-op checks and build logs. Independent checks recompute medians, MAD, changes
and verdicts, and verify raw compiler counts/CPU/RSS/PCH creation and JUnit totals.

Trigger the opt-in corpus with `incremental_corpus=true`. Optional
`incremental_project` and `incremental_mode` inputs restrict a rerun. The
[workflow](../.github/workflows/incremental.yml) fixes the revisions, targets,
headers, file choices, parallelism and CMake arguments.
For MuJoCo in this toolchain, also set `incremental_linker_deps=off`.
`mujoco_linker_probe=true` runs the separate library-only diagnostic.
