# Remove unused static-library dependencies

`prune-deps` tests whether programs can stop linking unused private static libraries.
Unlike `prune-links`, it requires an archive to disappear from the actual link
command. The source tree you give it stays unchanged.

```bash
buildray prune-deps --source /path/to/project \
  --compiler clang++-21 --exclude third_party --runs 3 --jobs 2 \
  --rebuild-file src/model.cc \
  -- -DBUILD_TESTING=ON \
     -DCMAKE_EXE_LINKER_FLAGS=--ld-path=/usr/lib/llvm-21/bin/ld.lld \
     -DCMAKE_SHARED_LINKER_FLAGS=--ld-path=/usr/lib/llvm-21/bin/ld.lld \
     -DCMAKE_MODULE_LINKER_FLAGS=--ld-path=/usr/lib/llvm-21/bin/ld.lld
```

This mode needs Linux ELF, Clang, LLD with
`--print-archive-stats`, CMake 4.2+ and Ninja 1.13+. Set the LLD path for your
machine. Buildray does not change the linker automatically: the supplied flags
apply to both variants. Results obtained with LLD describe that toolchain, even
if your project normally uses another linker.

Use `--target my_program --dependency my_library` to restrict the search. A
requested dependency that cannot be validated causes a failure with reasons in
`links.json`. Without that selector, a run with no confirmed candidate succeeds
and records baseline-only clean build and link timings, unless
`--incremental-only` skips them. It claims no gain.

## How the search scales

After one complete baseline build, Buildray profiles each relevant link once
with LLD. This gives extraction counts for all its archives. Archives that
contribute objects are excluded from the removal search.

Unsupported compiler commands, such as response files, stop the search before
those link replays. The report records the blocker instead of retrying it for
every dependency.

It tries removing the remaining eligible dependencies together. If that changes
compilation requirements, it tries retaining those requirements with
`COMPILE_ONLY`. When both forms fail, it divides the group and tries each half.
Every trial includes the removals already accepted, so two individually valid
removals cannot silently break the combined patch. Identical failed proposals
reuse their earlier rejection. Baseline archive statistics, reference binaries
and compiler commands are reused throughout the search.

Forty compatible unused dependencies can therefore take one trial configuration,
rather than forty complete builds. In difficult groups, the number of trial
configurations can still grow in proportion to the number of candidates. Each
trial also checks the combined patch and replays affected links, including
earlier accepted removals. Total runtime is not guaranteed to be linear or
logarithmic.

`--max-dependency-trials` limits trial configurations to 64 by default.
`--dependency-time-budget` defaults to 300 seconds and includes baseline archive
profiling and search. It is a soft limit: a running command finishes, then no
new trial starts. The baseline build happens before the budget starts. The
mandatory final verification, complete candidate build, tests and paired
benchmarks are outside both limits. A budget cutoff can keep a validated subset;
it does not mean every candidate was examined.

## What it checks

Buildray tries private static-library dependencies of executable,
shared-library and module targets. It tries deleting the declaration first. If
the library supplies headers, macros or other compilation requirements, it also
tries keeping those requirements with CMake's
[`COMPILE_ONLY` expression](https://cmake.org/cmake/help/latest/manual/cmake-generator-expressions.7.html#genex:COMPILE_ONLY). That
patch keeps the original link on CMake versions older than 3.27.

CMake variables can hold target names or library lists, including in functions
and loops. Buildray reads their evaluated arguments from CMake's expanded trace.
The patch guards each changed call with the exact target and list values tested;
other expansions run the original command. It leaves shared variable definitions
unchanged. Plain names, quoted variables and lists of names are supported;
generator expressions, configuration keywords and repeated ambiguous calls stay intact.

Each trial must pass checks 1–4. The final combined patch must pass all five:

1. The compiler command inventory stays identical, after normalizing the two
   isolated directory paths. CMake relationships change only as expected.
2. Link arguments only lose explicit `.a` inputs. Other arguments keep their
   order and values. LLD must report zero extracted objects from every archive
   removed from every affected link.
3. Replaying both links with the same baseline object files produces
   byte-identical ELF binaries. These analysis replays are outside timings.
4. Active compilations can disappear only for the removed archive targets.
   Unrelated targets cannot be excluded to manufacture a faster build.
5. The complete candidate builds and passes the same CTest test inventory as
   the baseline. Compiler and link coverage are checked for Ninja's default
   `all` target.

The final patch is checked again after search, including after a budget cutoff.
Runtime-skipped tests fail by default. With `--allow-baseline-skips`, their sorted
JUnit names must match the baseline after candidate builds and every incremental
edit. Reports list them separately in `baseline_skipped_tests`,
`candidate_skipped_tests` and incremental samples. Names retain JUnit XML escaping;
unstable skipped names are rejected. Disabled tests, failures and an all-skipped
run still fail. The Action exposes the same policy as `allow-baseline-skips` and
retains the JUnit evidence.

A subsequent build or test failure is blocking. Public
interfaces, shared-library dependencies, aliases, response files and
ambiguous declarations are outside this scope. Unsupported cases are skipped or
rejected; they are not evidence that those dependencies are necessary.

## Read the results

`cmake.patch` contains the proposed change. `links.json` records candidates,
rejected attempts and archive evidence. `report.json` records validation, raw
measurements and the recommendation. `dependency_search` counts archive
profiles, trial configurations, cached rejections and final verifications, and
reports whether a budget was reached. `selected_links` contains every accepted
removal; `selected_link` remains an alias for the first one for older consumers.

`archive-profile.json` records the reused baseline archive statistics and compiler
inventory. `dependency-profiles/` and `dependency-trials/` contain the small replay
logs, statistics, trial patches and proofs. Reference ELF files are temporary
and deleted when search finishes. Per-invocation timing records remain under
`logs/*-linkers.json`.

Clean timings alternate baseline and candidate builds. Add `--rebuild-file`
for edits you make often; `--incremental-only` keeps build and test validation
but skips repeated clean timings. An edit to an unused library may stop
triggering a consumer link altogether. If no patch is found, incremental
comparisons are not run.

The report separates total build wall time from the sum of link-driver times.
Link-driver time includes compiler-driver startup and the linker it launches.
Parallel links overlap, so their sum is not elapsed build time. Link CPU time
and the largest individual link-driver RSS peak are also recorded; the RSS peak
is not total build memory. Static archive creation is outside these link counts.

A passing patch can still have no measurable benefit. The existing noise rule
requires at least three pairs and a gain larger than both 2% of baseline time
and three times the larger MAD. This is a reporting heuristic. Read the clean
and incremental results before applying a patch, then validate your other
platforms and configurations.

```bash
git apply --check /path/to/run/cmake.patch
git apply /path/to/run/cmake.patch
```

## GitHub Action

Use `command: prune-deps`, JSON arrays for `cmake-args`, `rebuild-files` and
`exclude-paths`, and optional `link-target` / `link-dependency` selectors.
`max-dependency-trials` and `dependency-time-budget` set the same search limits
as the CLI. The `archive-profile` output points to retained baseline evidence. The
Action installs LLD with its LLVM tools. Select it explicitly in `cmake-args`
as above. The [corpus workflow](../.github/workflows/unused-dependencies.yml)
shows the complete configuration for XGrammar, SentencePiece and MuJoCo.
