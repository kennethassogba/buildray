# Remove unused static-library dependencies

`prune-deps` tests whether a program can stop linking one private static library.
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

This first implementation needs Linux ELF, Clang, LLD with
`--print-archive-stats`, CMake 4.2+ and Ninja 1.13+. Set the LLD path for your
machine. Buildray does not change the linker automatically: the supplied flags
apply to both variants. Results obtained with LLD describe that toolchain, even
if your project normally uses another linker.

Use `--target my_program --dependency my_library` to restrict the search. A
requested dependency that cannot be validated causes a failure with reasons in
`links.json`. Without that selector, a run with no confirmed candidate succeeds
and records baseline-only clean build and link timings, unless
`--incremental-only` skips them. It claims no gain.

## What it checks

Buildray tries literal private static-library dependencies of executable,
shared-library and module targets. It tries deleting the declaration first. If
the library supplies headers, macros or other compilation requirements, it also
tries keeping those requirements with CMake's
[`COMPILE_ONLY` expression](https://cmake.org/cmake/help/latest/manual/cmake-generator-expressions.7.html#genex:COMPILE_ONLY). That
patch keeps the original link on CMake versions older than 3.27.

Each proposal must pass these checks:

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

The search stops at the first proposal passing the structural and replay checks,
or after 16 strategies. A subsequent build or test failure is blocking. Public
interfaces, shared-library dependencies, aliases, variable lists, response files and
ambiguous declarations are outside this scope. Unsupported cases are skipped or
rejected; they are not evidence that those dependencies are necessary.

## Read the results

`cmake.patch` contains the proposed change. `links.json` records candidates,
rejected attempts and archive evidence. `report.json` records validation, raw
measurements and the recommendation. Per-invocation records are retained under
`logs/*-linkers.json`; `dependency-trials/` contains small replay logs and LLD
archive statistics.

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
`exclude-paths`, and optional `link-target` / `link-dependency` selectors. The
Action installs LLD with its LLVM tools. Select it explicitly in `cmake-args`
as above. The [corpus workflow](../.github/workflows/unused-dependencies.yml)
shows the complete configuration for XGrammar, SentencePiece and MuJoCo.
