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

Before contributing, read SentencePiece's
[contribution requirements](https://github.com/google/sentencepiece/blob/v0.2.2/CONTRIBUTING.md),
including its CLA. No upstream SentencePiece PR is submitted by this workflow.

## Other candidates

llama.cpp's [repository instructions](https://github.com/ggml-org/llama.cpp/blob/59d5fad7c111d30a547d690b69235ed9e05c035d/AGENTS.md)
restrict AI-written contributions and automated submissions. Keep it as a possible
benchmark; do not turn automated findings into upstream PRs.

Cloudflare's [workerd](https://github.com/cloudflare/workerd) uses Bazel. It is a
candidate for a later build-system adapter; the current CLI requires CMake/Ninja.
