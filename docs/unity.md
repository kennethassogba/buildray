# Validated unity builds

```bash
buildray unity --source /path/to/project --target my_library \
  --batch-size 4 --compiler clang++-21 --runs 3 --jobs 2
```

Unity builds combine translation units so shared headers are parsed fewer times.
Buildray uses CMake's `BATCH` mode on one explicit target. This is a separate
experiment from include cleanup and PCH. It supports C/C++ targets with Ninja
1.13+; the matching Clang C driver is derived from `--compiler`, or can be set
with `--c-compiler`. CMake keeps C and C++ in separate groups.

The batch size defaults to four files and is limited to 2–32. Larger batches can
increase compiler memory consumption and reduce parallel work. The report's raw
compiler samples include peak RSS per invocation; this is not aggregate machine
memory. Fixed build parallelism is shared by both variants.

## Validation and selection

The original tree is never modified. Buildray snapshots it into isolated trees,
configures a baseline with unity and PCH disabled, builds and runs CTest. The
candidate adds a root CMake block enabling unity only on the selected target.
PCH stays disabled. Imported, alias and interface targets are refused.

Before accepting the candidate, Buildray expands the generated unity includes
and compares the original source/target multiset against the baseline. Every
active C/C++ source must remain present with the same multiplicity and target.
It requires at least one real reduction in compiler invocations. Compiler
launcher records must cover Ninja's complete default `all` target, including
separate source files. Optional targets outside `all` are outside the experiment.
Unity outside the selected target is refused. Coverage is checked again during
measurement, outside the timed interval.

CMake excludes files with per-source compile settings, module scanning or
`SKIP_UNITY_BUILD_INCLUSION` from its batches. Buildray preserves that behavior.
For additional exclusions, repeat `--unity-exclude src/special.cpp` or
`--unity-exclude third_party`. Paths must exist relative to the source root.
Directories expand deterministically to their original C/C++ source files;
files generated later during configuration are not part of that expansion.
Existing build trees, `.git`, `.cache` and `.buildray` are skipped as they are
in the source snapshots.
The generated CMake uses the
target's directory when setting the source property, including targets declared
in subdirectories.

The candidate must pass a clean build, link, CTest execution and inventory
comparison. By default a compilation failure rejects it. With `--unity-retries 4`,
Buildray can exclude all original sources in failed compiler groups and try again.
The budget is 0–8 retries and defaults to zero. Each retry regenerates the patch,
reconfigures, checks flags, and starts another clean build. Only failed compiler
records for multi-source unity groups on the selected target can trigger this.
Standalone failures, missing records, compiler crashes, generated build-tree
inputs, coverage changes, link failures and test failures remain blocking.
It stops if no useful grouping remains. Sources are never renamed or rewritten.
This coarse isolation can exclude more files than necessary; it does not identify
the smallest conflicting pair or diagnose every compiler error.

`report.json` records every attempt, its build log, and `unity_auto_exclusions`.
Failed attempt plans and compiler records are retained as `unity-attempt-N-*.json`;
the final `unity.patch` contains both manual and learned exclusions. The Action
also retains these JSON files and the last 64 KiB of each attempt's build log.
Retries and discovery happen before timing; only the final fixed layout is timed.

Testing cannot prove that merging translation units preserves all
behavior: macros, lookup and initialization can change. Other feature flags,
platforms, downstream users and incremental builds require separate validation.

Three or more alternating clean-build pairs are needed for a performance
recommendation. Complete build wall time includes compiler work and linking;
analysis, cleaning and tests stay outside the timed interval. A patch is
recommended only when the measured improvement exceeds the existing noise
heuristic. One pair can validate a patch but reports `insufficient_samples`.

## Apply the result

- `unity.patch`: CMake patch with `option(BUILDRAY_UNITY ... OFF)`.
- `unity.json`: target, batch size, generated groups, original source coverage
  and before/after compiler invocation counts.
- `report.json`: validation, raw samples, memory peaks and performance verdict.

After review, apply the patch and pass `-DBUILDRAY_UNITY=ON` to your normal CMake
configuration. Without the option, the project's default behavior stays the same.
The variable is reserved in Buildray's own experiments to keep the baseline
disabled and the candidate enabled consistently.

## GitHub Action

```yaml
- uses: kennethassogba/buildray@COMMIT_SHA
  with:
    command: unity
    unity-target: my_library
    unity-batch-size: '4'
    unity-exclude: '["src/special.cpp"]'
    unity-retries: '4'
    runs: '3'
    jobs: '2'
```

The `patch`, `unity` and `report` outputs point to retained artifacts. PR baseline
references are ignored; this compares the supplied project with its generated
patch. The opt-in `unity_corpus` workflow tests the existing pinned SentencePiece,
XGrammar and MuJoCo projects. Each candidate can fail validation independently;
a failure is not evidence of an improvement.

CMake's [unity documentation](https://cmake.org/cmake/help/v4.2/prop_tgt/UNITY_BUILD.html)
describes grouping and source exclusions. Its
[batch-size documentation](https://cmake.org/cmake/help/v4.2/prop_tgt/UNITY_BUILD_BATCH_SIZE.html)
explains the memory and parallelism tradeoffs.
