---
title: The guide
outline: false
---

# Understand where build time goes.

<p class="lead">Buildray measures a C++ build, tries a change in a copy of the project, then checks whether it saves time.</p>

Your source tree stays intact. You get a patch to review, test results and a before/after comparison.

<BuildFlow />

## Start with a measurement

For a CMake project with CTest tests:

```bash
buildray measure --source /path/to/project --runs 5 --jobs 2
```

This measures repeated full builds after removing compiled outputs. Follow [your first measurement](./start.md) to install Buildray and enable your project's tests.

## Find the explanation you need

- [How a build works](./concepts.md) explains headers, the compiler and linking from the beginning.
- [Choose an optimization](./optimize.md) helps you decide between include cleanup, a precompiled header and Unity.
- [Read the measurements](./measure.md) explains how to distinguish a measured gain from variation.
- [Results and limits](./results.md) shows improvements, slowdowns and inconclusive experiments on our test projects.

## What Buildray can do

It measures full builds and, for PCH and Unity experiments, rebuilds after individual edits. It can propose include removal, forward declarations and some CMake cleanup. Both the CLI and the [GitHub Action](./ci.md) retain reports and patches.

Current support covers Linux, Clang, CMake and Ninja. Buildray validates the configuration and tests you provide. It does not prove equivalent behavior across every configuration or guarantee a speedup.

The [technical notes and validation evidence](https://github.com/kennethassogba/buildray/tree/main/docs) provide more detail.
