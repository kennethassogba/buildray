# CI comparisons

Buildray 0.2 adds a composite GitHub Action and two native C++26 commands:
`compare` for paired builds, and `check` for saved reports.

## GitHub Action

Use Ubuntu 24.04 for automatic dependency installation. The action installs
Clang/LLVM 21, Ninja and CMake 4.2.3, then builds its own C++26 CLI. A prepared
Linux runner can set `install-dependencies: 'false'`; it needs the same build
tools and Python 3.12+. `buildray-path` can point to a compatible prebuilt CLI.

```yaml
name: Build time
on: [pull_request]
permissions:
  contents: read
jobs:
  buildray:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v6
        with:
          fetch-depth: 0
      - uses: kennethassogba/buildray@codex/ci-baselines
        with:
          source: .
          cmake-args: '["-DBUILD_TESTING=ON"]'
          runs: '5'
          jobs: '2'
          max-regression-percent: '5'
```

The branch reference above is for trying this change. Pin a reviewed commit SHA
in a production workflow. There is no published Marketplace release yet.

On a PR, `baseline-ref` defaults to its base commit. The action exports that
commit with `git archive`, snapshots the current checkout, builds and tests both,
then alternates their clean builds. It fetches a missing base commit when needed.
Outside PRs, an empty baseline means a standalone `measure` run. Set an explicit
`baseline-ref` to compare another Git revision.

The baseline contains tracked files from Git; the current input includes the
checkout's working files. Git submodules and LFS payloads are not materialized
by `git archive`. Use the native `compare` command with fully prepared source
directories for projects that need them. Source symlinks remain unsupported;
links generated during CMake configuration are allowed inside the isolated run.

Use `command: optimize` to validate an include-removal patch instead. This mode
compares the untouched source with its patched copy and ignores `baseline-ref`.
It never applies a patch to the checkout or submits a PR.

`cmake-args` is a JSON array of `-DNAME=VALUE` strings. Inputs are passed as argv,
without shell evaluation. Projects still execute their own CMake code and tests,
so run them with the same trust boundaries as a normal build workflow.

The action writes a step summary and uploads reports and any patch, including
when a regression fails the step. It does not upload source copies or complete
compiler logs. Artifact retention defaults to 30 days; `artifact-name` and
`retention-days` are configurable. This provides downloadable run history, not
an external history service. Outputs include `report`, `comparison`,
`baseline-report`, `current-report`, `baseline-sha`, `patch`, and `result`.

## Native commands

```bash
buildray compare --baseline-source /tmp/base --source /tmp/current \
  --compiler clang++-21 --runs 5 --jobs 2 --max-regression-percent 5 \
  --work-dir /tmp/comparison -- -DBUILD_TESTING=ON

buildray check --baseline /tmp/comparison/baseline-report.json \
  --current /tmp/comparison/current-report.json \
  --max-regression-percent 5 --output /tmp/replayed-comparison.json
```

The output directory for `compare` must be new. `check` writes JSON and a `.md`
companion and refuses paths that would overwrite either input report.

Both commands require at least three positive, finite raw wall-time samples per
revision. `check` accepts successful `measure` reports, including those exported
by `compare`; it recomputes medians and MAD from the samples. Buildray 0.1 reports
lack the required context and must be measured again.

Reports must agree on tool versions, Buildray version, build method, sampling
method, job count, CMake arguments, selected cache/environment flags, CPU model,
affinity, quota, architecture, kernel, test policy and optional `--session` label.
This detects common mismatches. It cannot detect every difference in toolchain
contents, load, thermal state or runner hardware. Prefer paired `compare` runs
on one runner for a CI decision. Independently measured history is diagnostic;
matching recorded context does not prove identical conditions.

A regression fails only when the median slowdown exceeds **both**:

- the configured percentage budget (default 5%);
- three times the larger median absolute deviation (MAD) of the two samples.

Results are `regressed`, `improved`, `within_budget`, or `inconclusive`. This is
a reporting heuristic, not a significance test. A noisy run can pass as
inconclusive; increase samples or use a quieter runner before drawing a conclusion.
CTest validation happens before timing and is required unless `--allow-no-tests`
is explicit. Build-only reports do not claim test validation.

| Exit | Meaning |
| --- | --- |
| 0 | Completed; no demonstrated regression over budget |
| 1 | Build, validation, I/O or pipeline failure |
| 2 | Invalid CLI arguments |
| 3 | Regression exceeds budget and noise |
| 4 | Saved reports are incompatible or malformed |
| 130 | Interrupted |

A completed comparison uses `status: passed` for pipeline completion and
`gate_passed` for the performance gate. Read both when consuming the JSON.
