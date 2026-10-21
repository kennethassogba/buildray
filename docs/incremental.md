# Measure incremental rebuilds

Pass one or more `--rebuild-file` paths to `pch`, `unity` or `prune-deps`. Buildray measures
each file separately in the baseline and candidate build trees. Select files
that reflect your workload: an implementation file, an internal header and a
public header are useful starting points. Header visibility is chosen by the
caller; Buildray does not infer it from directory names.
Incremental coverage checks require Ninja 1.13 or newer.

```bash
buildray unity --source /path/to/project --target core --unity-retries 4 \
  --rebuild-file src/parser.cpp \
  --rebuild-file src/parser.hpp \
  --rebuild-file include/project/api.hpp \
  --compiler clang++-21 --runs 3 --jobs 2

buildray pch --source /path/to/project --target core --header src/common.hpp \
  --rebuild-file src/parser.cpp --rebuild-file src/common.hpp \
  --incremental-only --compiler clang++-21 --runs 3 --jobs 2
```

Without `--incremental-only`, both repeated clean-build timings and incremental
timings are retained. With it, full baseline/candidate builds, source coverage
checks and CTest validation still run, but repeated clean-build timings are
skipped. The top-level clean performance result is `not_measured` and the patch
is not automatically recommended. Each incremental scenario has its own verdict.

## What is measured

Each variant starts from a complete validated build. Before every sample, an
untimed build must report no work and produce no compiler records. Buildray
allows up to two untimed settling builds first, for one-off work such as a
relink after dependency discovery. If the build still does not become idle,
the experiment stops and retains Ninja's dependency explanation. Buildray
then appends a numbered comment to the selected file in its isolated snapshot
and times the complete default build. The same probe bytes are used in both
variants; pairs alternate baseline-first and candidate-first order. Each probe
replaces the previous comment, so subsequent pairs also invalidate the file.

Compilation, PCH regeneration when needed, code generation and linking are inside
the timer. Editing, no-op checks and tests are outside it. Every measured rebuild
normally produces at least one successful compiler record, using a subset of the
commands verified during full validation. For `prune-deps`, baseline probes must
trigger compilation or linking; the candidate can do no work if the removed
dependency was the only reason to rebuild. C and C++ commands are covered,
including in mixed-language PCH targets. CTest runs after every sample with the
original test inventory; failures, disabled tests and runtime skips reject the
experiment. `--allow-no-tests` explicitly permits build-only validation.

After a scenario, the exact original file bytes are restored in both snapshots
and each build is brought up to date outside timing. File contents are restored
on failure and interruption too. The original input tree is never edited.
Generated files, duplicate paths, files outside the snapshot and files that do
not trigger compilation are refused, except for the link-cleanup cases above. Projects with build commands that always
run are refused by the no-op check.

Comment edits measure dependency invalidation and the amount of work required
to rebuild. They do not reproduce arbitrary semantic changes, changes to compiler
options, or a cold filesystem cache. These experiments use warm filesystem
caches with compiler caches disabled. Choose additional files when their
dependency fan-out matters; three files do not describe every developer edit.

## Reports and recommendations

`incremental.json`, `incremental.md` and the `incremental` field in `report.json`
retain the scenarios. Raw samples include wall time, compiler invocation count,
compiled source paths, unique original input source files (expanding Unity groups),
PCH creation count, compiler CPU time, maximum individual
compiler RSS, no-op duration and test count. Link cleanup also records link
counts, link-driver wall/CPU time, individual RSS peaks and linked outputs. RSS is not aggregate build memory.
Generated files and PCH wrappers stay in `compiled_sources` but do not count as
original input source files.
Compiler records, build logs, inventories and JUnit results are kept under
`logs/incremental-*`. The GitHub Action retains these artifacts too.

Each scenario reports medians, MAD and a wall-time change. A verdict needs at
least three pairs and a difference exceeding
`max(2% baseline median, 3 × larger MAD)`. This is a noise heuristic, not a
statistical significance test. A measured incremental regression prevents the
top-level `patch_recommended` flag even when the clean-build result improved.
An inconclusive result establishes neither a gain nor absence of regression.

Action inputs are `rebuild-files` (a JSON array of relative paths) and
`incremental-only` (`'true'` or `'false'`). The `incremental` output points to
the retained JSON. The job summary lists completed scenarios and their results.

The opt-in `incremental_corpus` CI input defaults to Unity on pinned
XGrammar, SentencePiece and MuJoCo revisions. Select `incremental_mode=pch`
or `all` explicitly to include PCH. Each uses three single-file
scenarios and three alternating pairs per scenario. The workflow records the
specific target, PCH header, Unity exclusions, build flags and test scope.
