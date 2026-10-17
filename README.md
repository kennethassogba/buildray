# Buildray

Measure C++ builds, find costly headers, and validate include cleanup or PCH patches.

Buildray is written in **C++26**. It supports Linux, Clang, CMake and Ninja.

## Build

Requirements: a compiler with C++26 mode (tested with Clang 21), CMake 3.30+,
Ninja, LLVM and Clang development libraries 20+, clang-tidy with `misc-include-cleaner`,
clang-scan-deps (for CMake's C++ module scanning), and GNU diff. Python 3.12+ is used
by the integration tests and GitHub Action; the CLI itself is native C++.
Install `libclang-21-dev` alongside LLVM 21. Keep the matching LLVM and Clang
runtimes installed. Installation places `buildray` and `buildray-header-tool`
in the same directory; header analysis needs both executables.

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-21 \
  -DLLVM_DIR=/usr/lib/llvm-21/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$HOME/.local"
```

Use [apt.llvm.org](https://apt.llvm.org/) if your distribution does not ship the
LLVM packages. Compiler and LLVM selection also accept your usual CMake configuration.

## Run

```bash
buildray optimize --source /path/to/cmake-project --runs 5 --jobs 2
```

Pass project configuration after `--`:

```bash
buildray optimize --source /path/to/project \
  --compiler clang++-21 --clang-tidy clang-tidy-21 \
  --work-dir /tmp/my-buildray-run --runs 5 --jobs 2 \
  -- -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

Keep vendored or generated source directories out of edits with repeatable literal
paths, for example `--exclude 3rdparty --exclude src/generated`. These paths are
relative to the source root. They remain part of the build and test validation.

The work directory must not already exist. The default is a unique directory
under `SOURCE/.buildray/`. For measurement without include analysis:

```bash
buildray measure --source /path/to/project --runs 5 --jobs 2
```

For a build-time regression check between two revisions:

```bash
buildray compare --baseline-source /path/to/base --source /path/to/current \
  --compiler clang++-21 --runs 5 --jobs 2 --max-regression-percent 5
```

Both revisions build and pass tests before their clean builds alternate on the
same machine. Exit code `3` means a slowdown exceeds both the budget and measured
noise. Reports and a Markdown comparison are kept even when the gate fails.

The [GitHub Action](docs/ci.md) compares PRs with their base revision and retains
reports as workflow artifacts. For an offline check of compatible saved reports:

```bash
buildray check --baseline base/report.json --current current/report.json \
  --output comparison.json --max-regression-percent 5
```

`check` recomputes statistics from raw samples and rejects incompatible tool,
configuration or machine context. [Comparison details and limits](docs/ci.md).
`buildray --help` lists all options and exit codes.

## Header cleanup and forward declarations

```bash
buildray headers --source /path/to/project --forward-declarations \
  --header-path src --exclude 3rdparty --max-header-edits 8 \
  --compiler clang++-21 --clang-tidy clang-tidy-21 --runs 3 --jobs 2
```

`headers` removes unused includes from observed project headers. With
`--forward-declarations`, it also tries replacing a direct project include with
class or struct declarations derived from Clang's AST. It checks standalone
headers, checks every observed C/C++ user, then builds, links, tests and
measures the combined patch. Your original tree stays intact.

The run writes `headers.patch` and `header-cleanup.json`, including rejected
proposals and validation counts. Passing validation does not establish a speedup;
the measured gain must exceed noise before Buildray recommends the patch.
[Selection rules, limits and Action usage](docs/header-cleanup.md).

## Header costs and PCH

```bash
buildray profile --source /path/to/project --compiler clang++-21 --jobs 2
buildray pch --source /path/to/project --target my_library \
  --header '<vector>' --header 'include/common.hpp' \
  --compiler clang++-21 --runs 5 --jobs 2
buildray pch --auto --source /path/to/project --compiler clang++-21 --runs 3 --jobs 2
```

`profile` builds and tests an isolated copy with Clang time traces. `headers.json`
and `headers.md` rank headers by inclusive parse time; the JSON also groups them
by CMake target. Nested headers overlap, so these costs cannot be added to predict
a speedup. Trace overhead is kept out of benchmark results.

`pch` takes a target and repeatable headers, generates `pch.patch`, and builds and
tests both variants. It alternates clean builds with PCH disabled in the baseline
and enabled only for the selected candidate target. PCH generation counts toward
candidate time. The patch is recommended only when the measured gain exceeds noise.
See [the workflow and limitations](docs/profiling.md). Both modes are also available
through the GitHub Action.

`pch --auto` profiles first, then selects one project header with the highest
inclusive parse cost within a target, requiring at least two compilations using
that header. `--target` can restrict the search. It runs the same validated PCH
experiment and requires at least three build pairs. Profiling is outside those
measurements. Selection does not establish a gain; only the measured result can
recommend the patch. `selection.json` records the decision and its metrics.

## Unity builds

```bash
buildray unity --source /path/to/project --target my_library \
  --batch-size 4 --compiler clang++-21 --runs 3 --jobs 2
```

`unity` groups C/C++ files on one target to reduce repeated header parsing. It
checks that every original source remains compiled on the same target, runs the
complete build and CTest, then compares alternating clean builds. Optional
`--unity-retries 4` isolates failed compiler groups before validation; link and
test failures remain blocking. Repeat `--unity-exclude path/to/file.cpp`
to keep incompatible files separate; CMake's existing per-file exclusions are
also respected. PCH stays disabled in both variants.

The generated `unity.patch` adds an option that defaults to off. After reviewing
and applying it, configure your project with `-DBUILDRAY_UNITY=ON` to enable the
tested batches. `unity.json` records source coverage and generated groups.
[Validation, memory tradeoffs and Action usage](docs/unity.md).

## Dependency graphs

```bash
buildray deps --source /path/to/project --compiler clang++-21 --c-compiler clang-21
# Optional CI policy: exit 5 when structural cycles are found.
buildray deps --source /path/to/project --fail-on-cycles
```

`deps` runs a C/C++ build and CTest, captures direct includes even when include
guards skip a header, and reports cycles and header fan-out. It also records CMake
target relationships and their declaration locations. JSON, Markdown and Graphviz
outputs are retained. It requires CMake 4.2+ and Ninja 1.13+.

Guarded include cycles and static-library link cycles can be valid. The graph
helps choose changes to investigate; it does not remove dependencies or estimate
a speedup. [Scope, CI usage and corpus reproduction](docs/dependencies.md).

## Redundant CMake links

```bash
buildray prune-links --source /path/to/project --compiler clang++-21 \
  --exclude 3rdparty --runs 3 --jobs 2
```

Buildray selects one literal link with an alternate path preserving link and
compilation usage requirements, then reconfigures, checks the graph changes,
builds, tests and measures both variants. `--target` and `--dependency` can restrict
selection. It produces `cmake.patch` and `links.json`; no patch is applied to the
original project. No eligible link completes without a patch or benchmark.
[Selection rules, limits and Action usage](docs/cmake-links.md).

## What optimize does

1. Copies the source tree into isolated baseline and candidate trees, excluding `.git`, `.cache`,
   `.buildray`, the current work directory and existing CMake build directories.
2. Configures a fresh Release build, builds it and runs its CTest tests. Project
   `-D` options can override the build type and enable the project's test suite.
3. Configures and builds the candidate to generate any headers needed for
   analysis. Both source copies are prepared before CMake can modify either one.
4. Runs `clang-tidy misc-include-cleaner` using the real compilation commands.
   Only whole-line include deletions in project C++ source files are eligible.
   Files generated or downloaded during configuration are excluded from edits.
   Suppression comments are respected. If several targets compile a file, a
   removal must be suggested for every compilation command.
   Every deletion-only proposal is compiled in each configuration first. Files
   that fail this check stay intact while independent valid proposals continue;
   rejected proposals are recorded in `preflight_rejected_files`.
5. Writes `includes.patch`, performs a clean candidate build and runs its tests.
   A compile or test failure marks the run failed and the patch unvalidated.
6. Measures repeated clean builds of both variants, alternating which runs first.
   Reports medians, median absolute deviations (MAD), raw samples and a performance verdict.

Buildray does not edit the original source tree or apply the patch automatically.
Inspect the report, then check and apply a validated patch from your project root:

```bash
git apply --check /path/to/run/includes.patch
git apply /path/to/run/includes.patch
```

## Results

```text
report.json                    Result, findings and measurements
comparison.json, comparison.md Comparison and CI gate (compare)
baseline-report.json           Base measurement for history (compare)
current-report.json            Current measurement for history (compare)
includes.patch                 Unified patch (optimize only)
headers.patch                  Header include and forward-declaration patch (headers)
header-cleanup.json            Header findings, rejected proposals and validation counts
pch.patch                      CMake PCH patch (pch only)
cmake.patch                    Redundant target-link patch (prune-links)
unity.patch, unity.json         Opt-in unity patch and verified source groups (unity)
links.json                     Eligible link candidates and selected experiment
cmake-before.json, cmake-after.json Configured target graph evidence
headers.json, headers.md        Inclusive header costs and target rankings (profile)
selection.json                 Automatic PCH candidate and selection metrics (pch --auto)
dependencies.json, .md, .dot    Include graph, cycles and CMake relationships (deps)
logs/*-compilers.json           Duration, CPU time, RSS and argv per compiler invocation
logs/*-build.log                Build output for each sample
logs/*-tests.xml                CTest validation results
analysis/*/fixes.yaml           Original clang-tidy findings
baseline-source/, candidate-source/
baseline-build/, candidate-build/
```

`status: passed` means the pipeline completed; it does not imply a speedup.
`patch_validated` records candidate validation. `patch_recommended` is true only
when a nonempty validated patch also has an `improved` performance result. A
failed candidate can leave a patch for diagnosis: its existence is not validation.

For `optimize`, `headers`, `prune-links`, `unity` and `pch`, `performance_result` is `improved`, `regressed`, `inconclusive`,
`insufficient_samples` (fewer than three runs), or `no_changes`. The noise
threshold is the larger of 2% of baseline time and three times the larger MAD.
This is a reporting heuristic, not a statistical significance test. A noisy
runner can produce an inconclusive result even when the candidate median is lower.

Timings cover full builds with a warm filesystem cache, fixed parallelism and
compiler caches disabled. Unity builds are disabled except on the selected `unity`
candidate target. PCH is disabled for include
cleanup and revision comparisons; only `pch` enables it in the candidate. Cleaning, configuration, analysis and
tests are outside the measured interval. Build wall time includes dependency
scans, compilation, code generation, linking and launcher overhead. Per-compiler
totals cover C++ compiler invocations (C/C++ for `headers`, `deps` and `unity`); their sum is not the parallel build's wall
time. RSS is the maximum of individual compiler peaks, not aggregate build memory.

## Validation scope

- One supplied CMake configuration per invocation. Run separately for other
  platforms, feature flags and build types. Passing tests is evidence for that
  configuration, not a proof of semantic equivalence.
- All C++ entries in the compilation database must be built and instrumented.
  Launcher overrides and `EXCLUDE_FROM_ALL` C++ targets cause a coverage error.
  Disable optional targets that are not part of the build you want to validate.
  Active unity compilation and unexpected PCH are refused, including target overrides.
  `deps`, `headers` and `unity` instead check every C/C++ object reachable from Ninja's default `all`
  target, allowing optional excluded targets. The `deps` CMake graph still lists configured
  excluded targets; their includes are not claimed as observed. `headers` also
  requires Ninja 1.13+ and counts inactive compilation commands. `unity` also requires Ninja 1.13+.
- CTest tests are required. Missing tests fail unless `--allow-no-tests` is
  explicit; that report says `build_only`. Disabled, skipped and failing tests
  are refused. Each test has a 120-second timeout.
- `optimize` edits source files; `headers` edits observed project headers.
  Macro includes, continued directives and directives
  carrying IWYU pragmas are not automatically removed. Source symlinks are refused.
  Implementation includes ending in `.inc`, `.inl`, `.ipp`, `.tpp`, `_inl.h`,
  `-inl.h` or `.inl.h` are kept because they can supply template definitions.
- Project CMake scripts and tests execute normally. Buildray uses no network
  service; project dependency downloads remain the project's responsibility.
- Cycle-breaking rewrites, general template forward declarations,
  incremental builds and a tagged Marketplace release are later work. The
  composite Action can already be used by its commit SHA.

Exit codes: `0` completed, `1` pipeline or validation failure, `2` CLI syntax
error, `3` regression over budget and noise, `4` incompatible saved reports,
`5` dependency cycles with the explicit cycle policy,
`130` interrupted. A completed comparison has `status: passed`; inspect
`gate_passed` for its performance policy result.

## Development

```bash
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-21 \
  -DLLVM_DIR=/usr/lib/llvm-21/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=Debug -DBUILDRAY_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure
```

Tests exercise the actual CLI and CMake/Clang/clang-tidy/CTest tools: patch
application, source preservation, multiple target configurations, build and test
failures, missing/skipped tests, coverage, symlink refusal, argument quoting,
measurements and interruption. Analyzer stubs only force false positives in
failure-path tests.

The real-project check uses unmodified tinyxml2 11.0.0. See
[validation evidence](docs/validation.md) for commands, scope and observed results.
[AI corpus](docs/corpus.md) records the next projects and their contribution constraints.
[Profiling and PCH evidence](docs/profiling-validation.md) covers the new commands,
including candidates with no measured benefit.
[Dependency graph evidence](docs/dependency-validation.md) covers MuJoCo and XGrammar.
[Automatic PCH and cleanup evidence](docs/automatic-validation.md) covers the
selection workflow and per-file validation on XGrammar.
[CMake link evidence](docs/cmake-validation.md) covers redundant declarations,
exported requirements and failed experiments.

[Header cleanup evidence](docs/header-validation.md) covers forward declarations,
standalone checks and C/C++ user validation.
[Unity build evidence](docs/unity-validation.md) covers the measured XGrammar
improvement, memory tradeoff and rejected corpus candidates.
