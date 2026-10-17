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
index original project headers and their C/C++ compilation users. It runs one
translation unit per subprocess to bound the AST lifetime. The helper and project
compiler must use the same Clang major version. Install both Buildray executables
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
scopes. It changes only original project files outside excluded paths.

Every proposal must compile as a standalone header and with every observed user,
including C users and users in excluded directories. Failed proposals are restored
while independent changes continue. Accepted changes remain staged during later
checks, so those checks cover the combined changes. A final standalone pass checks
all changed headers again. Buildray then emits the patch, performs a clean build,
links and reruns the same CTest suite. A failure leaves the patch unvalidated.

Clean baseline/candidate builds alternate using the existing measurement workflow.
Analysis, AST indexing, preflight compilations and tests are outside the measured
interval. The AST helper is not loaded by the compiler timing launcher. Three or
more pairs are needed for a performance recommendation.

## Results and limits

- `headers.patch`: unified patch; never applied to the supplied source tree.
- `header-cleanup.json`: selection scope, findings, rejected/skipped headers and
  AST, tidy, standalone and preflight counts.
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
