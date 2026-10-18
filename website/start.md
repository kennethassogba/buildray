---
title: Your first measurement
---

# Make your first measurement

Start with a project that already builds and passes its tests. Supply the CMake options needed to build and test the parts you care about.

## Install Buildray

On Linux, install Clang/LLVM 21 and their development libraries, `clang-tidy-21`, `clang-scan-deps`, CMake 3.30 or newer, Ninja and GNU diff. Install `libclang-21-dev` alongside LLVM. `deps` and `prune-links` need CMake 4.2 or newer and Ninja 1.13 or newer.

LLVM packages are available from [apt.llvm.org](https://apt.llvm.org/). The [repository build instructions](https://github.com/kennethassogba/buildray#build) describe compiler requirements. Python 3.12 or newer is used by integration tests and the Action.

```bash
git clone https://github.com/kennethassogba/buildray.git
cd buildray
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-21 \
  -DLLVM_DIR=/usr/lib/llvm-21/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$HOME/.local"
```

Add `$HOME/.local/bin` to your `PATH` if needed. Keep the installed `buildray` and `buildray-header-tool` executables together for header analysis. You can also [use the Action](./ci.md) without installing locally.

## Measure your project

```bash
buildray measure --source /path/to/project \
  --compiler clang++-21 --runs 5 --jobs 2 \
  --work-dir /tmp/my-buildray-measurement \
  -- -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

The work directory must be new. Replace `BUILD_TESTING` with your project's test option if it has a different name. Arguments after `--` go to CMake.

`--jobs 2` allows two build tasks at a time. Keep this value fixed when comparing versions, and avoid other heavy work during the measurement.

## Open the report

Inside the work directory, `report.json` contains individual timings, their median and the measurement context. `logs/` retains tool output. Configuration and tests are outside the timed build interval.

Resolve build or test failures before interpreting timings. Buildray normally refuses test validation when no tests are registered. `--allow-no-tests` permits a build-only measurement, with that limit recorded in the report.

You can then [try an optimization](./optimize.md) or [compare revisions in CI](./ci.md).
