---
title: In GitHub Actions
---

# Watch build time in CI

The GitHub Action compares a pull request with its base revision. It builds and tests both, then alternates their full-build measurements on the same runner.

## Add the workflow

Create `.github/workflows/build-time.yml` in your project:

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
      - uses: kennethassogba/buildray@a55e07e75441251f4d9566215f83246244004577
        with:
          source: .
          cmake-args: '["-DBUILD_TESTING=ON"]'
          runs: '5'
          jobs: '2'
          max-regression-percent: '5'
```

The example pins a Buildray revision so updates are explicit. Adjust the project's CMake test option. The Action installs its tools on Ubuntu 24.04 and builds the CLI; preparation is outside the compared timings. It works directly from the repository without a Marketplace release.

## Find the results

The job summary shows the comparison. Artifacts retain reports and, for optimization experiments, the patch. Retention defaults to 30 days. The gate fails when a slowdown exceeds the budget and noise threshold.

The Action does not apply patches or open PRs. Sources and tests execute project code; use the same trust boundaries as a normal build workflow.

## Try an optimization

For example, add these inputs to the Buildray step's `with` block:

```yaml
command: pch
pch-auto: 'true'
runs: '3'
jobs: '2'
```

Optimization experiments compare the project with its patched copy. They ignore `baseline-ref`, which is used for revision comparisons.

The automatic revision comparison exports the base with `git archive`. It does not materialize Git submodules or LFS contents. If your project needs them, prepare both source trees and use the native `compare` command in your workflow.

See the [Action inputs and outputs](https://github.com/kennethassogba/buildray/blob/main/action.yml) and [technical CI guide](https://github.com/kennethassogba/buildray/blob/main/docs/ci.md) for other modes and limitations.
