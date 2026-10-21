# Header profiling and PCH experiments

PCH is optional and disabled by default. Prefer include and dependency cleanup;
consider PCH only when measured absolute savings justify the added configuration.

Buildray 0.3 adds `profile` and `pch`. The CLI remains C++26; the inspected project
keeps its own language standard.

## Find repeated header costs

```bash
buildray profile --source /path/to/project --compiler clang++-21 \
  --work-dir /tmp/project-headers --jobs 2 -- -DBUILD_TESTING=ON
```

One isolated build runs with Clang's `-ftime-trace` and zero event granularity.
The normal compiler launcher still verifies full compilation coverage, and CTest
must pass. Existing PCH and unity builds are disabled. No clang-tidy is needed.

`headers.json` ranks `Source` events globally and per CMake target. Each row keeps
inclusive seconds, maximum event duration, event count, and number of compiler
invocations that parsed the header. Repeated events in one invocation count once
in `compilations`. Source and build roots use `<SOURCE>` and `<BUILD>` markers;
system paths remain absolute. Raw traces and compiler records stay in the work
directory. Both complete events and Clang 21 asynchronous begin/end events are
supported. Missing traces and unmatched events fail the run.

Inclusive costs overlap: a wrapper's time includes the headers it includes.
These numbers identify places to investigate; their sum is neither elapsed build
time nor predicted savings. This instrumented build has overhead, so `profile`
does not emit a clean-build benchmark or a performance verdict. `--runs` does not
repeat profiling; the command always profiles one build.

## Test a PCH on one target

```bash
buildray pch --source /path/to/project --target core \
  --header '<string>' --header '<vector>' --header 'include/common.hpp' \
  --compiler clang++-21 --runs 5 --jobs 2 \
  --work-dir /tmp/project-pch -- -DBUILD_TESTING=ON
```

Choose a local compiled CMake target and headers repeated in that target's profile.
System headers use angle brackets; project paths are relative to the source root.
Paths with spaces are accepted. Paths outside the source tree, CMake expressions,
duplicate headers, imported targets, aliases, and targets with an existing direct
PCH or `REUSE_FROM` are refused.
The generated PCH wrapper must contain exactly the requested includes; inherited
interface PCH headers are refused rather than silently added to the experiment.

The candidate appends a guarded `target_precompile_headers(... PRIVATE ...)` block
to its root `CMakeLists.txt`. It enables C++ PCH only for the selected target; C
sources in mixed targets keep normal compilation. Other targets' existing PCHs
stay disabled during the experiment. Actual compiler records must show creation
and use of the selected PCH. The baseline must build and pass tests without PCH.
The candidate then passes a clean build, linking and the same CTest count.

Both variants undergo repeated alternating clean builds without time traces.
PCH creation, all C++ compilation, code generation and linking are included in
elapsed candidate time. CPU and individual compiler RSS measurements include PCH
creation. Add `--rebuild-file` to also measure
[incremental rebuilds](incremental.md). RSS does not estimate aggregate memory
across concurrent compiler processes.

`report.json` retains raw measurements, validation and the performance verdict.
`patch_recommended` requires a validated patch, at least three samples per variant,
and an improvement exceeding `max(2% baseline median, 3 × larger MAD)`. Otherwise
the result is inconclusive, regressed or insufficiently sampled. This heuristic
is not a statistical significance test. A validated slower patch returns exit 0
because the experiment completed; inspect its recommendation before applying it.

```bash
git apply --check /tmp/project-pch/pch.patch
git apply /tmp/project-pch/pch.patch
cmake -S . -B build -DBUILDRAY_PCH=ON
```

Applying the patch alone leaves PCH disabled. Set `BUILDRAY_PCH=OFF` to turn it
off again. Buildray enables it only in the candidate of an explicit `pch` run.

Only the chosen configuration is validated. Test other platforms and feature
flags separately. A forced PCH can affect macros or hide missing includes in
future edits; keep include-cleanup validation without PCH. No source includes are
removed by `pch`, and neither command edits the original tree.

## Select a candidate automatically

```bash
buildray pch --auto --source /path/to/project --compiler clang++-21 \
  --runs 3 --jobs 2 --work-dir /tmp/project-auto-pch
# Restrict selection to one target when needed:
buildray pch --auto --target core --source /path/to/project --compiler clang++-21
```

Buildray freezes the input tree, profiles one isolated build with CTest, and chooses
one header/target pair. Eligible headers are original source-root files with `.h`,
`.hpp`, `.hh` or `.hxx` extensions, used in at least two C++ compiler invocations of
that target. Generated and external headers are excluded. Selection uses the
largest per-target inclusive parse cost, with deterministic ties by target and path.
Nested costs overlap; the score is not a prediction of savings or incremental cost.

The selected candidate goes through the manual mode's complete experiment:
baseline and candidate builds, tests, PCH creation/use checks, then at least three
alternating clean-build pairs. Selection profiling is outside those measurements.
There is one candidate, with no automatic fallback or search through other PCHs.
The existing restrictions on direct or inherited PCHs still apply. `--header`
cannot be combined with `--auto`; use manual mode for explicit system headers.

The root retains `report.json`, `pch.patch`, `headers.json`, `headers.md` and
`selection.json`. Full profile and experiment workspaces are under `profile/` and
`experiment/`; the original frozen input is under `input/`. The report records
`pch_auto`, `automatic_selection` and `selection_profiling_in_measurements: false`.
Source files remain unchanged, including when the work directory is inside the
source tree. Failure logs and selection evidence are retained.

An expensive repeated header can still make other files compile differently when
forced into a PCH. Failed compilation or tests reject the experiment. A completed
slower or inconclusive experiment returns 0 with `patch_recommended: false`, just
as in manual mode. No eligible header produces an error and no candidate patch.

## GitHub Action

```yaml
- uses: kennethassogba/buildray@YOUR_REVIEWED_COMMIT_SHA
  with:
    command: profile
    source: .
    artifact-name: headers
- uses: kennethassogba/buildray@YOUR_REVIEWED_COMMIT_SHA
  with:
    command: pch
    pch-target: core
    pch-headers: '["<string>", "<vector>", "include/common.hpp"]'
    runs: '5'
    artifact-name: pch-experiment
- uses: kennethassogba/buildray@YOUR_REVIEWED_COMMIT_SHA
  with:
    command: pch
    pch-auto: 'true'
    runs: '3'
    artifact-name: automatic-pch
```

`headers` is the profiling JSON output. `patch` points to the include patch or PCH
patch for the selected mode. The Action retains header rankings and adds the
profile or PCH result to the job summary. `baseline-ref` and the regression budget
apply to `measure` only. Failed experiments retain bounded diagnostic log tails.
Automatic mode requires empty `pch-headers`; `pch-target` is an optional restriction.
Its `headers` and `selection` outputs retain the profile and selection JSON.

For include cleanup, `exclude-paths: '["3rdparty"]'` keeps dependency sources out
of edits while retaining their build and test coverage. Every proposed deletion
set is compiled for that file before acceptance, even when include-cleaner does
not request provider additions. Rejected files remain unchanged and are listed in
`preflight_rejected_files`; independent valid removals can still be validated and
measured. Complete linking and CTest remain required after these file checks.

The implementation follows the official [Clang trace options](https://clang.llvm.org/docs/ClangCommandLineReference.html)
and [CMake PCH API](https://cmake.org/cmake/help/latest/command/target_precompile_headers.html).
