# MuJoCo model comparison helper

Buildray's compiler records identified `test/compare_model.cc` as an expensive
translation unit. Its pointer-field macro repeats an array loop and string
formatting for each model field. A manual refactor moves that work into one
template shared by fields of the same type. The comparison calculation, field
order, exclusions, maximum difference and diagnostic coordinates are preserved.
Buildray does not automatically make this refactor.

## Compilation measurement

Source: MuJoCo `651bae11c3c38efe483f033b4b7e869e7ce3f4ec`.
Machine: Intel Core i7-8650U. Clang 21, GCC 15 standard library, Release `-O3`,
ThinLTO, one compiler process at a time. The real CMake compile command was
reused, removing include tracing and dependency-output flags. Inputs and object
outputs use separate temporary files; header paths and compiler flags match.
Buildray's native compiler launcher captured wall time, CPU and peak RSS.

These measurements cover compilation and ThinLTO object emission for this
translation unit. Whole-project clean build time and runtime performance were
not measured by this experiment. The filesystem cache was warm after a pilot;
compiler caches were disabled. Three pairs alternate baseline/candidate order.

| Pair | Order | Original (s) | Shared helper (s) |
| --- | --- | ---: | ---: |
| 1 | Original first | 161.689 | 1.064 |
| 2 | Shared helper first | 147.175 | 1.033 |
| 3 | Original first | 147.508 | 1.091 |
| Median | | **147.508** | **1.064** |
| MAD | | 0.333 | 0.026 |

Median compiler wall time decreased **99.28%**. The difference exceeds the
existing noise heuristic, whose threshold is 2.950 seconds here. This heuristic
is not a significance test. The maximum compiler peak RSS among these samples
was 439,972 KiB before and 131,136 KiB after, about 430 and 128 MiB. This is the
peak of this compiler invocation, not total build or machine memory.

[Raw samples and commands](evidence/mujoco-compare-model-compile-report.json)
include source hashes and normalized paths. The
[measured patch](evidence/mujoco-compare-model-measured.patch) preserves the exact
temporary source bytes. The
[native-validation patch](../tests/corpus/mujoco-compare-refactor.patch) formats
the same extracted helper and macro call.

## Native validation

Dispatch `ci.yml` with `mujoco_compare=true` to run the dedicated MuJoCo workflow
in double and single precision. Both baseline and candidate first receive the
same three regression tests: equal-model diagnostics, numeric/byte/character
array differences and coordinates, and maximum/tie ordering. This checks the
tests against the original implementation as well as the refactor.

The harness builds the original implementation, runs CTest, applies the source
refactor, rebuilds and runs CTest again. The second build is incremental and is
used for functional validation only. It is not a clean-build timing comparison.
CTest inventories must match, including multiplicity and stable filters for
GTest's opaque byte-dump parameter names.

MuJoCo explicitly skips 15 tests under `mjUSESINGLE` at this revision. The
corpus-only native harness requires exactly those same cases before and after,
with zero failures. Double precision permits no skipped cases. This does not
relax the Buildray CLI's rule: a runtime-skipped CTest case still rejects a CLI
experiment, including the single-precision unity baseline.

The workflow retains native JUnit, test inventories, build logs and
`validation.json`. Bazel registration is included in the proposed upstream test
patch; the native harness exercises CMake on Linux.

Both native jobs passed in [run 39054447117](https://github.com/kennethassogba/buildray/actions/runs/39054447117).

| Precision | Original plus regression tests | Shared helper plus the same tests |
| --- | --- | --- |
| Double | 3,522 passed; no skips or failures | 3,522 passed; no skips or failures |
| Single | 3,507 passed; 15 upstream skips; no failures | 3,507 passed; the same 15 skips; no failures |

The [double](evidence/mujoco-compare-model-double-native.json) and
[single](evidence/mujoco-compare-model-single-native.json) summaries record the
exact skip list, source revision, verified patch hash and inventory identity.
The source patch published on `kennethassogba/mujoco` matches the validated
refactor patch byte for byte.
