# Dependency graph validation

Buildray 0.4 stays C++26. The dependency suite builds actual C/C++ fixtures and
runs CTest. It checks guarded two-file cycles, self includes, inactive branches,
repeated includes, static-library link cycles, interface targets, declaration
locations, directory-scoped imported targets, paths with spaces and excluded targets.
A C launcher bypass and a compiler error both fail the pipeline. The Action tests
cover graph artifacts and the opt-in cycle failure policy. A unit fixture exercises
a 10,000-node cycle and rejects malformed include depth.

All six suites pass locally in Release (133.52 s, followed by the isolated-temp
unit recheck) and Debug with ASan/UBSan (133.95 s). The local sanitizer run disables
LeakSanitizer because of the sandbox; GitHub uses its default configuration.
Both native CI jobs also pass at `61899df` in
[run 38882067479](https://github.com/kennethassogba/buildray/actions/runs/38882067479).
The installed CLI also builds and tests a pure-C project with no includes.

## XGrammar

Input: `mlc-ai/xgrammar@7e269d56c74c89de52bc8c31e42191155416371f`, with pinned
DLPack `bbd2f4d32427e548797929af08cfe2a9cbb3cf12` and GoogleTest
`df1544bcee0c7ce35cd5ea0b3eb8cc81855a4140` submodules. The root `config.cmake`
disables Python bindings and enables C++ tests as described in
[the reproduction instructions](dependencies.md#reproduce-the-corpus).

Linux, Clang 21.1.8, Release, two jobs, CMake 4.2.3 and Ninja 1.13.2:

- 44 C++ compiler invocations captured, with complete default-build coverage.
- All 83 CTest cases pass.
- 141 original-snapshot nodes and 469 observed include edges.
- 8 configured CMake targets and 37 typed target relationships.
- No include or target-link cycle observed in this configuration.

`cpp/support/logging.h` has 33 distinct direct includers and appears in 37
compilations. `include/xgrammar/exception.h` appears in 36. These are fan-out
counts, not parse costs or predicted savings. The populated GoogleTest and DLPack
submodules are part of the original snapshot. No upstream source code is edited.

The independent Ubuntu CI evaluation in
[run 38882067479](https://github.com/kennethassogba/buildray/actions/runs/38882067479)
also passes all 83 tests and produces the same node, include-edge, target and
typed-target-edge counts using CMake 4.2.3 and the pinned Ninja 1.13.0 package.
Its complete graphs are retained in `xgrammar-dependencies`. A
[summary excerpt](evidence/xgrammar-dependencies-summary.json) is checked in.

## MuJoCo configuration

Input: `google-deepmind/mujoco@f640345f72b773e7ec52cd9426a57335cf9c6c28`, with
examples, simulate and Python utility tests disabled; native C++ tests remain
enabled. The pinned corpus workflow generates `de_DE.UTF-8`, which the upstream
locale test needs. Compilation and test outcomes are retained in workflow artifacts.
This configuration uses double precision. It does not validate single precision,
the Python bindings or graphical applications.

The initial CI run passed all 3,496 MuJoCo tests, then exposed a parser error:
`linkLibraries` and `interfaceLinkLibraries` can contain raw `fragment` entries
instead of target `id` entries (for example `m`, `dl` or an absolute library path).
The parser now retains these separately as `cmake.link_fragments`, including their
available declaration locations. They do not create target vertices or cycles.
The regression fixture links a static library to `m` publicly and checks both
relationship kinds. The old binary rejects that fixture; the corrected binary
builds it and reports the graph.

The opt-in `dependency_corpus` workflow input builds and tests both pinned projects.
Graph tracing overhead is never reported as a performance measurement. These
evaluations make no compile-time reduction claim.
