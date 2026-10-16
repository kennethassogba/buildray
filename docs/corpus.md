# C++ test corpus

Keep tinyxml2 as the fast end-to-end check. Add AI tooling where a broader C++
build can expose different failure modes and potentially larger savings.
Repository size or an organization's use of AI does not guarantee a useful patch
or acceptance upstream. Check each project's contribution rules before submitting.

| Project | Pinned input | Purpose |
| --- | --- | --- |
| [tinyxml2](https://github.com/leethomason/tinyxml2) | 11.0.0, `9148bdf719e997d1f474be6bcc7943881046dba1` | Quick CI fixture; original CTest suite |
| [SentencePiece](https://github.com/google/sentencepiece) | v0.2.2, `e0cce7d37b065b5140349dbe12c6bcf6192fdd78` | AI tokenizer; larger CMake build with bundled dependencies and tests |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | `59d5fad7c111d30a547d690b69235ed9e05c035d` | Benchmark candidate only; upstream rules prohibit autonomous AI PR submissions |

The tinyxml2 cleanup was also checked on upstream master
`8224e427b655b83dae5e2298f1e6919523a78737` before opening
[draft PR #1100](https://github.com/leethomason/tinyxml2/pull/1100).
It removes three unused includes; no build-time gain is claimed.

## SentencePiece

```bash
git clone --depth 1 --branch v0.2.2 https://github.com/google/sentencepiece.git /tmp/sentencepiece
buildray optimize --source /tmp/sentencepiece \
  --compiler clang++-21 --clang-tidy clang-tidy-21 --runs 3 --jobs 2 \
  --work-dir /tmp/sentencepiece-result \
  -- -DSPM_BUILD_TEST=ON -DSPM_ENABLE_SHARED=OFF -DSPM_ENABLE_TCMALLOC=OFF
```

This configuration downloads Abseil during CMake configuration. It also creates
a source-tree symlink to the dependency; Buildray prepares both pristine source
copies first so generated links do not contaminate the next snapshot. Existing
input symlinks remain refused.

Use the `C++26 CLI` workflow's manual `ai_corpus` input for this longer run.
The default push/PR checks keep the fast tinyxml2 fixture. The workflow validates
the pinned commit and retains the report and patch. A validated patch is not
automatically recommended: the measured performance result still matters.

The first [CI run](https://github.com/kennethassogba/buildray/actions/runs/38823949565)
proposed 106 removals across 52 files, then failed the candidate's clean build.
Its [raw report](evidence/sentencepiece-initial-rejected-report.json) records
`patch_validated: false`; no before/after timing result was produced.

The failure was reproduced in `word_model_trainer.cc`: removing `word_model.h`
also removed a transitive declaration of `SplitIntoWords`. Include-cleaner had
requested a direct provider include alongside that deletion. Buildray now reads
those insertion suggestions and precompiles deletion-only proposals in every
configuration. A file that fails remains unchanged; the final batch still needs
a clean build and CTest validation.

The [second run](https://github.com/kennethassogba/buildray/actions/runs/38825788008)
performed 48 preflight compilations, kept `word_model_trainer.cc`, and proposed
104 remaining removals. Its [report](evidence/sentencepiece-link-rejected-report.json)
records another validation refusal: compilation succeeded, but linking failed
because the proposed removal of `extension_set_inl.h` lost Protobuf template
definitions. Conventional implementation-header suffixes are now protected too.
This case also verifies that compilation preflight does not replace a full link
and test pass. Failure-log tails were retained by the Action and inspected.

Before contributing, read SentencePiece's
[contribution requirements](https://github.com/google/sentencepiece/blob/v0.2.2/CONTRIBUTING.md),
including its CLA. No upstream SentencePiece PR is submitted by this workflow.

## Completed SentencePiece run

[CI run](https://github.com/kennethassogba/buildray/actions/runs/38827344178),
optimizer at `e5e7cc6439c2b37909bdaff01894dc1ad1177c13`, Clang/LLVM 21.1.8,
Ubuntu 24.04 x86_64, 2 build jobs. Buildray itself was compiled in C++26.
The source remains pinned to v0.2.2 and the configuration above.

| Check | Observed result |
| --- | --- |
| Original C++ compile commands analyzed | 78 |
| Generated/downloaded compile commands excluded from edits | 158 |
| Additional compiler preflight invocations | 48 |
| Files kept after failed preflight | 1 |
| Includes removed / files changed | 103 / 51 |
| Baseline and candidate validation | Clean builds and 1 CTest suite passed for each |
| Samples | 3 clean builds per variant, alternating order |
| Baseline median / MAD | 91.554851412 s / 0.819718481 s |
| Candidate median / MAD | 94.185779013 s / 0.349577386 s |
| Wall-time change | +2.874% |
| Performance classification | `regressed` |
| Patch validation / recommendation | Validated for this configuration; not recommended |

The configured build and tests pass, but this run shows a slower median after
cleanup. Removing more directives did not produce a measured speedup. The
classification uses the documented heuristic; three pairs do not establish a
statistical significance result. No upstream SentencePiece PR was submitted.

The [raw report](evidence/sentencepiece-report.json) retains every sample and
both summary distributions. The [validated patch](evidence/sentencepiece-validated.patch)
also passed `git apply --check` on the pristine pinned checkout; that checkout
remains unchanged. The patch is retained as benchmark evidence, not recommended
as a performance change. Source/build copies and full compiler records were
on the ephemeral runner; the Action retains the report and patch as artifacts.

## Other candidates

Two candidates for the next AI-focused checks, based on their CMake configuration:

- [MuJoCo / Google DeepMind](https://github.com/google-deepmind/mujoco/blob/main/CMakeLists.txt):
  C++ simulation code with `MUJOCO_BUILD_TESTS`; examples and the simulator UI can
  be disabled. Prepare a CPU test configuration before running the full benchmark.
- [XGrammar / MLC AI](https://github.com/mlc-ai/xgrammar/blob/main/CMakeLists.txt):
  C++ grammar tooling with `XGRAMMAR_BUILD_CXX_TESTS` and CTest discovery. Disable
  Python bindings and materialize its required submodules for a native test run.

These are source-level compatibility checks, not completed benchmarks. Pin a
revision before running them. MuJoCo's [contribution rules](https://github.com/google-deepmind/mujoco/blob/main/CONTRIBUTING.md)
require a CLA and tests in both precision variants. XGrammar's
[contribution guide](https://github.com/mlc-ai/xgrammar/blob/main/CONTRIBUTING.md)
asks for typed PR titles, formatting checks and code-owner review. Neither guide
promises acceptance of automated cleanup.

llama.cpp's [repository instructions](https://github.com/ggml-org/llama.cpp/blob/59d5fad7c111d30a547d690b69235ed9e05c035d/AGENTS.md)
restrict AI-written contributions and automated submissions. Keep it as a possible
benchmark; do not turn automated findings into upstream PRs.

Cloudflare's [workerd](https://github.com/cloudflare/workerd) uses Bazel. It is a
candidate for a later build-system adapter; the current CLI requires CMake/Ninja.
