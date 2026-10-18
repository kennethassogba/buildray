---
title: Choose an optimization
---

# Choose what to try

Find where the build spends time first. Run one experiment at a time so you can tell what caused the result.

## Find expensive headers

```bash
buildray profile --source /path/to/project --compiler clang++-21 --jobs 2
```

`headers.md` ranks headers by parse time from Clang traces. Nested header times overlap; adding them does not predict a speedup. Traces are diagnostic and stay outside before/after timings.

## Clean up includes

```bash
buildray optimize --source /path/to/project --runs 3 --jobs 2
buildray headers --source /path/to/project \
  --forward-declarations --header-path src \
  --exclude third_party --runs 3 --jobs 2
```

`optimize` tries removing unused includes from C++ sources. `headers` works on observed headers; `--forward-declarations` also tries replacing includes with forward declarations. Analysis uses actual compiler commands. It does not enforce every clang-tidy style rule.

Excluded paths still build and run tests, but their contents are not edited. Files generated or downloaded during configuration are also excluded from edits.

## Try PCH or Unity

```bash
buildray pch --auto --source /path/to/project \
  --compiler clang++-21 --runs 3 --jobs 2

buildray unity --source /path/to/project --target my_library \
  --batch-size 4 --runs 3 --jobs 2 \
  --rebuild-file src/model.cc --rebuild-file include/model.h
```

`pch --auto` profiles the project and selects a header on one target. It tests one proposal; the profile alone does not establish a gain. To choose explicitly, replace `--auto` with `--target my_library --header 'include/common.h'`.

Unity produces a CMake option that defaults to off. After applying the patch, `-DBUILDRAY_UNITY=ON` enables the tested groups. Add `--rebuild-file` to either mode to measure files you often edit.

For Unity or a manually selected PCH, replace `--jobs` with
`--job-levels 1,2,4,8` to compare job counts. This cannot be combined with `--auto` or Unity retries. Buildray runs each level sequentially, validates the same
candidate and keeps its measurements separate. The report marks job counts
above the machine's detected CPU capacity. See the
[parallelism reference](https://github.com/kennethassogba/buildray/blob/main/docs/parallelism.md).

## Examine dependencies

```bash
buildray deps --source /path/to/project
buildray prune-links --source /path/to/project --runs 3 --jobs 2
```

`deps` shows header and target relationships, including cycles. It does not break cycles automatically; some are valid.

`prune-links` tests removing one redundant CMake declaration whose requirements remain reachable through another path. It does not yet find every unnecessary library. If the final link command stays the same, removing that declaration alone is not expected to save time.

`prune-deps` checks archive use once per program, then tries removing eligible private static libraries together. It splits groups that fail and keeps accepted removals in every later trial. Compiler commands must stay unchanged, and link replay must produce identical binaries from the same objects. The final combined patch must also pass the full build and tests.

This mode needs ELF LLD, selected explicitly for both variants. If a library only supplies headers or compilation settings, the patch can keep those while removing the link. Add `--rebuild-file` to check whether an edit triggers fewer links. The [command reference](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies.md) gives the setup and limits.

The search defaults to 64 trial configurations and 300 seconds. Change these with `--max-dependency-trials` and `--dependency-time-budget`. The final build and tests still run after the search budget is reached.

If your project deliberately skips some tests, use `--allow-baseline-skips`. Buildray records those tests separately and requires the same skipped names after each change, including incremental edits. Skipped tests never count as passed; disabled tests, failures and a run with no passing tests still block validation.

## Keep a validated patch

Read `patch_validated`, `patch_recommended` and `performance_result` in `report.json`. A `.patch` file can remain after a failure; its presence does not establish validation.

For a validated include patch, from your project root:

```bash
git apply --check /path/to/run/includes.patch
git apply /path/to/run/includes.patch
```

Review the change and test other configurations before integrating it. The [technical references](https://github.com/kennethassogba/buildray/tree/main/docs) describe each command's selection and validation rules.
