# Buildray 0.2 validation

The native CLI remains C++26. Clang/LLVM 21.1.8, CMake 4.2.3 and Ninja were used
for local Release and Debug ASan/UBSan builds. All four CTest suites passed in
both configurations:

- `unit`: comparison statistics are recomputed from samples; budgets, noise,
  malformed context, missing test results and numeric overflow are checked.
- `integration`: actual include cleanup, patch application, unchanged input,
  configuration-generated links and files, multiple compile configurations,
  compiler, linker and runtime failures, coverage and interruption. The transitive-provider
  fixture verifies that an unsafe deletion-only proposal leaves its source intact.
- `comparison`: two real CMake builds, a deliberately slow fixture for the
  regression exit, replayed reports, incompatible context, and output aliases.
- `action`: a local Git baseline, actual paired builds, retained reports after a
  regression, summary output, unchanged checkout, input validation and bootstrap
  failure evidence, bounded failure logs and native exit status.

The slow fixture adds a one-second CMake command to exercise the regression
gate deterministically. It is test infrastructure, not a project speedup result.

```bash
ctest --test-dir build-release --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

Local runs also set the host-specific GCC 15 selection flag described in
[the original validation](validation.md). The local sandbox requires disabling
LeakSanitizer; address and undefined-behavior checks remain enabled. GitHub CI
runs with sanitizer defaults, including leak detection.

## GitHub Action

[PR validation run](https://github.com/kennethassogba/buildray/actions/runs/38823945336)
at `5aa75037b5b4f6d7cd04ad30ae8dfe758bc3be04` passed both Release and sanitizer
jobs. The Release job also:

1. Installed dependencies using the action's installer on Ubuntu 24.04.
2. Bootstrapped a fresh native C++26 CLI through the composite Action.
3. Compared the tracked CMake fixture with the same Git revision, with three
   samples per variant. The fixture has a C++26 `static_assert`.
4. Checked the resolved base SHA and comparison output, uploaded `action-smoke`
   reports, and completed the original tinyxml2 cleanup check.

The smoke comparison uses a 1000% budget to test wiring without enforcing a
performance policy on a subsecond fixture. It reported `inconclusive`; no gain
is claimed. The action artifacts were downloaded and the installed local CLI
successfully replayed their raw `baseline-report.json` and `current-report.json`
using `buildray check`.

The [provider-guard follow-up CI run](https://github.com/kennethassogba/buildray/actions/runs/38825782458)
at `241e22e02809a32be06b781056a7751482275821` passed both jobs too, including
all four suites, the Action bootstrap and tinyxml2. It covers the actual
transitive-provider regression and retains diagnostics for failed project runs.

The implementation-header guard adds explicit policy checks and a real linker
failure case: a template declaration still compiles after its definition header
is removed, but the full build rejects the patch. Source and build roots in
cached flags are normalized; the comparison fixture now compiles with real
`-ffile-prefix-map` and `-fdebug-prefix-map` options containing those roots.

The [final code CI run](https://github.com/kennethassogba/buildray/actions/runs/38828009423)
at `3e882e100e3fb08a9afe34a3d2db636677d7548e` passed both jobs and covers these cases.

The installed binary reports `buildray 0.2.0 (C++26)`. C++ formatting, Python
syntax and YAML parsing checks passed. A tagged Marketplace release has not
been published.

The [larger AI-project run](https://github.com/kennethassogba/buildray/actions/runs/38827344178)
passed every job, including the real SentencePiece optimization workflow.
It validated 103 include removals, then measured a +2.874% median wall-time change
and left `patch_recommended: false`. Commands, raw samples, rejected earlier
proposals and the final patch are recorded in [the corpus](corpus.md).
