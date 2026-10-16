# Dependency graphs

```bash
buildray deps --source /path/to/project --work-dir /tmp/dependency-run \
  --compiler clang++-21 --c-compiler clang-21 --jobs 2 \
  -- -DBUILD_TESTING=ON
```

Buildray snapshots the source, configures a fresh Ninja build, builds the default
`all` target and runs CTest. C and C++ compilations use Clang's `-H` and
`-fshow-skipped-includes`. Skipped includes matter: `a.h -> b.h -> a.h` is still a
structural cycle when include guards make the program compile.

The CLI stays C++26. This command needs CMake 4.2+ (codemodel 2.9) and Ninja 1.13+
(`compdb-targets`). Existing commands keep their CMake 3.30 minimum. Both language
compilers must be Clang; use `--compiler` for C++ and `--c-compiler` for C.

## Reports

- `dependencies.json` contains observed source nodes and direct include edges,
  strongly connected components, shared-header rankings and configured CMake targets.
- `dependencies.md` shows cycle groups and the thirty most shared headers.
- `dependencies.dot` contains include and target graphs in separate clusters.
- `report.json` records tool versions, configuration, test validation and cycle policy.
- `baseline-records/*.includes` retains raw compiler traces locally, alongside
  compiler invocation records. These files are not uploaded by the Action.

Each include edge counts compiler invocations in which it appeared. Repeated
includes within one compilation count once. Header fan-out counts invocations
reaching a header, including skipped includes. It is not parse time, build time or
a prediction of rebuild cost. Use `profile` for inclusive parse costs, then
`optimize` or `pch` to validate and measure a candidate.

The include graph contains files present in the snapshot before configuration,
including vendored files and populated submodules. System headers, downloaded
dependencies and generated include nodes are excluded. Compilations of downloaded
dependencies are still captured and checked. Their includes of original snapshot
headers contribute to fan-out. Configuration-specific inactive branches are absent.
An excluded target appears in the configured CMake graph but has no observed
include graph unless it is a dependency of `all`.

CMake relationships retain their kinds: `linkLibraries`, `interfaceLinkLibraries`,
`compileDependencies`, `interfaceCompileDependencies`, `objectDependencies` and
`orderDependencies`. Available backtraces identify declaration files and lines.
Link-cycle detection uses the two link kinds. It does not use CMake's legacy
`dependencies` field, which describes transitive build ordering and can omit static
link cycles. Interface and imported targets come from the codemodel's abstract targets.
Relationships to CMake targets form the graph. Raw linker flags and library names
without targets are retained separately in `cmake.link_fragments`, with their kinds
and available declaration locations. They do not create vertices or target cycles.
No unused or missing link dependency is inferred.

Guarded includes and cyclic static libraries can be valid. A finding needs review
before changing declarations, include ownership or CMake dependencies. No automatic
cycle fix or forward declaration is generated. Source bytes remain unchanged.
Unity builds and PCH are disabled, and launcher overrides cause coverage failure.
Tests are required unless `--allow-no-tests` explicitly permits build-only validation.

## CI policy

```yaml
- uses: kennethassogba/buildray@<reviewed-commit>
  with:
    command: deps
    source: .
    cmake-args: '["-DBUILD_TESTING=ON"]'
    fail-on-cycles: 'false'
```

The default is informational. Setting `fail-on-cycles: 'true'` (or CLI
`--fail-on-cycles`) returns exit `5` if any observed include SCC or configured
target-link SCC is cyclic, including self loops. There is no baseline suppression.
Reports remain available, `status` is `passed` for a completed analysis, and
`cycle_gate_passed` is false. The Action result is `cycles` and its step fails.

Action outputs `dependencies` and `dependency-dot` point to retained artifacts.
The Markdown report is added to the job summary. A failed build, incomplete compiler
coverage or failed CTest does not produce a successful graph.

## Reproduce the corpus

The existing `C++26 CLI` workflow has an opt-in `dependency_corpus` input. It checks
out exact upstream commits, runs this command and retains reports for each project.

MuJoCo: `google-deepmind/mujoco@f640345f72b773e7ec52cd9426a57335cf9c6c28`.

```bash
buildray deps --source fixtures/mujoco --compiler clang++-21 --c-compiler clang-21 \
  --jobs 2 -- -DMUJOCO_BUILD_EXAMPLES=OFF -DMUJOCO_BUILD_SIMULATE=OFF \
  -DMUJOCO_BUILD_TESTS=ON -DMUJOCO_TEST_PYTHON_UTIL=OFF
```

XGrammar: `mlc-ai/xgrammar@7e269d56c74c89de52bc8c31e42191155416371f`.
Initialize its pinned `3rdparty/dlpack` and `3rdparty/googletest` submodules. Its
tracked `cmake/config.cmake` overrides the `-D` test/binding options; use the
project's supported root configuration file:

```cmake
# fixtures/xgrammar/config.cmake
set(XGRAMMAR_BUILD_PYTHON_BINDINGS OFF)
set(XGRAMMAR_BUILD_CXX_TESTS ON)
```

```bash
buildray deps --source fixtures/xgrammar --compiler clang++-21 --c-compiler clang-21 --jobs 2
```

These runs evaluate dependency analysis. They do not benchmark optimization gains
or establish validation across other platforms, Python bindings or build flags.

References: [Clang include tracing](https://clang.llvm.org/docs/ClangCommandLineReference.html),
[CMake 4.2 File API](https://cmake.org/cmake/help/v4.2/manual/cmake-file-api.7.html),
[Ninja tools](https://ninja-build.org/manual.html#_extra_tools).
