# Header profiling and PCH experiments

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
PCH or `REUSE_FROM` are refused. Selection is explicit in this release.

The candidate appends a guarded `target_precompile_headers(... PRIVATE ...)` block
to its root `CMakeLists.txt`. It enables C++ PCH only for the selected target; C
sources in mixed targets keep normal compilation. Other targets' existing PCHs
stay disabled during the experiment. Actual compiler records must show creation
and use of the selected PCH. The baseline must build and pass tests without PCH.
The candidate then passes a clean build, linking and the same CTest count.

Both variants undergo repeated alternating clean builds without time traces.
PCH creation, all C++ compilation, code generation and linking are included in
elapsed candidate time. CPU and individual compiler RSS measurements include PCH
creation. This release measures clean builds only; it does not estimate incremental
rebuild cost or aggregate memory across concurrent compiler processes.

`report.json` retains raw measurements, validation and the performance verdict.
`patch_recommended` requires a validated patch, at least three samples per variant,
and an improvement exceeding `max(2% baseline median, 3 × larger MAD)`. Otherwise
the result is inconclusive, regressed or insufficiently sampled. This heuristic
is not a statistical significance test. A validated slower patch returns exit 0
because the experiment completed; inspect its recommendation before applying it.

```bash
git apply --check /tmp/project-pch/pch.patch
git apply /tmp/project-pch/pch.patch
```

Only the chosen configuration is validated. Test other platforms and feature
flags separately. A forced PCH can affect macros or hide missing includes in
future edits; keep include-cleanup validation without PCH. No source includes are
removed by `pch`, and neither command edits the original tree.

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
```

`headers` is the profiling JSON output. `patch` points to the include patch or PCH
patch for the selected mode. The Action retains header rankings and adds the
profile or PCH result to the job summary. `baseline-ref` and the regression budget
apply to `measure` only. Failed experiments retain bounded diagnostic log tails.

The implementation follows the official [Clang trace options](https://clang.llvm.org/docs/ClangCommandLineReference.html)
and [CMake PCH API](https://cmake.org/cmake/help/latest/command/target_precompile_headers.html).
