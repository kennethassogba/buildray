# Buildray

Measure C++ builds, remove unused includes, validate the patch, and compare build times.

Buildray is written in **C++26**. This first release supports Linux, Clang, CMake and Ninja.

## Build

Requirements: a compiler with C++26 mode (tested with Clang 21), CMake 3.30+,
Ninja, LLVM development libraries 20+, clang-tidy with `misc-include-cleaner`,
clang-scan-deps (for CMake's C++ module scanning), and GNU diff. Python 3 is used
by the integration tests; the CLI itself is native C++.
Keep the matching LLVM runtime installed for the executable.

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

The work directory must not already exist. The default is a unique directory
under `SOURCE/.buildray/`. For measurement without include analysis:

```bash
buildray measure --source /path/to/project --runs 5 --jobs 2
```

`buildray --help` lists all options and exit codes.

## What optimize does

1. Copies the source tree into an isolated baseline, excluding `.git`, `.cache`,
   `.buildray`, the current work directory and existing CMake build directories.
2. Configures a fresh Release build, builds it and runs its CTest tests. Project
   `-D` options can override the build type and enable the project's test suite.
3. Copies the baseline source into a candidate, configures and builds it to
   generate any headers needed for analysis.
4. Runs `clang-tidy misc-include-cleaner` using the real compilation commands.
   Only whole-line include deletions in project C++ source files are eligible.
   Suppression comments are respected. If several targets compile a file, a
   removal must be suggested for every compilation command.
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
includes.patch                 Unified patch (optimize only)
logs/*-compilers.json           Duration, CPU time, RSS and argv per C++ compilation
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

`performance_result` is `improved`, `regressed`, `inconclusive`,
`insufficient_samples` (fewer than three runs), or `no_changes`. The noise
threshold is the larger of 2% of baseline time and three times the larger MAD.
This is a reporting heuristic, not a statistical significance test. A noisy
runner can produce an inconclusive result even when the candidate median is lower.

Timings cover full builds with a warm filesystem cache, fixed parallelism and
compiler caches disabled. Unity builds and PCH are disabled in both variants so
aggregation cannot mask missing includes. Cleaning, configuration, analysis and
tests are outside the measured interval. Build wall time includes dependency
scans, compilation, code generation, linking and launcher overhead. Per-compiler
totals cover C++ compiler invocations; their sum is not the parallel build's wall
time. RSS is the maximum of individual compiler peaks, not aggregate build memory.

## Validation scope

- One supplied CMake configuration per invocation. Run separately for other
  platforms, feature flags and build types. Passing tests is evidence for that
  configuration, not a proof of semantic equivalence.
- All C++ entries in the compilation database must be built and instrumented.
  Launcher overrides and `EXCLUDE_FROM_ALL` C++ targets cause a coverage error.
  Disable optional targets that are not part of the build you want to validate.
  Active PCH or unity compilation is refused, including target overrides.
- CTest tests are required. Missing tests fail unless `--allow-no-tests` is
  explicit; that report says `build_only`. Disabled, skipped and failing tests
  are refused. Each test has a 120-second timeout.
- Headers are not edited. Macro includes, continued directives and directives
  carrying IWYU pragmas are not automatically removed. Source symlinks are refused.
- Project CMake scripts and tests execute normally. Buildray uses no network
  service; project dependency downloads remain the project's responsibility.
- CMake rewrites, forward declarations, PCH selection, trace analysis,
  incremental builds and Marketplace packaging are later work.

Exit codes: `0` completed, `1` pipeline or validation failure, `2` CLI syntax
error, `130` interrupted. CI can inspect `report.json` to enforce a performance
policy independently of validation success.

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
