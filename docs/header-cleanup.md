# Header cleanup

```bash
buildray headers --source /path/to/project --forward-declarations \
  --header-path src --exclude 3rdparty --max-header-edits 8 \
  --compiler clang++-21 --clang-tidy clang-tidy-21 --runs 3 --jobs 2
```

Without `--forward-declarations`, the command only removes unused includes.
`--header-path` and `--exclude` are repeatable literal paths relative to the
source root. The default budget is eight changed headers (range 1–1000).
Headers with more observed compilation users are considered first, with paths
breaking ties. At most one included provider per header is replaced by forward
declarations in one run. Selection and the budget are recorded in the report.

## Validation

Buildray snapshots the project and builds/tests the baseline and candidate before
analysis. A separate native helper uses Clang's AST and preprocessor callbacks to
index original project headers and their C/C++ compilation users in Ninja’s
default `all` target. Ninja 1.13+ is required. Launcher records must cover every
C/C++ object in that target; optional excluded targets are counted separately
and are outside the experiment. It runs one
translation unit per subprocess to bound the AST lifetime. The helper and project
C and C++ compilers must use the same Clang major version. `headers` derives the
C driver from the C++ driver name (`clang++-21` → `clang-21`); `--c-compiler`
can set it explicitly. Both variants use those compilers, including C targets
and mixed C/C++ LTO builds. Install both Buildray executables
next to each other, together with their matching LLVM/Clang runtime libraries.
Building the helper also requires the Clang development headers (`libclang-21-dev`
for LLVM 21).

Each header is first compiled on its own using the flags of each distinct observed
C++ compilation context. Headers that fail this original check are skipped.
`clang-tidy misc-include-cleaner` analyzes the header as the main file; a removal
must be suggested in every context. Suppression comments, macro directives,
continued directives and implementation includes are preserved.

Forward declarations come from actual class/struct definitions used through direct
pointers or references in header declarations. The first version excludes
aliases, templates, specializations, nested/local classes, attributed classes,
standard-library types, inline/anonymous namespaces and includes inside lexical
scopes. By-value fields, arrays, base classes and function signatures keep their record
provider. Template argument types are conservatively treated as requiring their
providers too; smart-pointer-specific rewrites are deferred.
Forward rewrites are limited to declaration-oriented headers: inline function
bodies, object/field initializers, default arguments, aliases, templates, enums
and static assertions defer the whole header until those requirements are modeled. A transitive include supplying a complete type is not
sufficient justification for these proposals. AST vetoes are merged across all
observed contexts: one context requiring the definition blocks that include
rewrite everywhere, even when another context only uses pointers. It changes only original project files outside excluded paths. A header carrying
any `NOLINT` marker is conservatively excluded from forward-declaration rewrites,
including next-line and block suppressions; tidy still handles unused-include
suppressions normally.

Every proposal must compile as a standalone header and with every observed user,
including C users and users in excluded directories. User preflights reuse the
original flags with `-c` removed and `-fsyntax-only` added; code generation and linking are checked by
the subsequent clean build. Failed proposals are restored
while independent changes continue. Accepted changes remain staged during later
checks, so those checks cover the combined changes. A final standalone pass checks
all changed headers again. Buildray then emits the patch, performs a clean build,
links and reruns the same CTest suite. Inventory matching keeps names and
multiplicity; opaque GoogleTest byte-dump parameters use their actual stable
filters, with the normalization counts recorded. A failure leaves the patch unvalidated.

Clean baseline/candidate builds alternate using the existing measurement workflow.
Analysis, AST indexing, preflight compilations and tests are outside the measured
interval. The AST helper is not loaded by the compiler timing launcher. Three or
more pairs are needed for a performance recommendation. When there are no eligible
changes, both builds and their CTest inventories are still validated, then the
command returns `no_changes` without benchmarking identical source copies.

## Results and limits

- `headers.patch`: unified patch; never applied to the supplied source tree.
- `header-cleanup.json`: selection scope, findings, rejected/skipped headers and
  AST, tidy, standalone and preflight counts, plus inactive compilation commands.
- `report.json`: build/test validation and raw samples, medians, noise threshold,
  performance verdict and recommendation.
- `header-analysis/` and `logs/`: local AST facts, compilation databases, tidy fixes
  and compiler output. The Action retains the summary, patch and report; failures
  also retain bounded log tails.

Only observed headers and one supplied build configuration are covered. Unseen
public headers, external downstream clients, other platforms and feature flags
need separate checks. Standalone checks reuse observed flags, including any forced
includes. Compilation and tests cannot prove semantic equivalence; removing a
provider can change overload selection, macros or initialization behavior.
Review public API compatibility before publishing a patch.

The implementation deliberately requires a baseline header to compile on its own;
it does not repair existing header self-containment problems or add missing
includes in this command. `patch_validated` records validation in the configured
project; `patch_recommended` additionally requires a measured improvement exceeding
the existing noise heuristic.

## GitHub Action

```yaml
- uses: kennethassogba/buildray@COMMIT_SHA
  with:
    command: headers
    source: .
    forward-declarations: 'true'
    header-paths: '["src"]'
    exclude-paths: '["3rdparty"]'
    max-header-edits: '8'
    runs: '3'
    jobs: '2'
```

The `patch`, `header-cleanup` and `report` outputs point to retained artifacts.
A PR baseline reference is ignored in this mode: the experiment compares the
supplied project with its generated header patch.
