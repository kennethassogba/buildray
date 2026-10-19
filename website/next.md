---
title: What comes next
---

# Broaden dependency cleanup {#test-dependency-cleanup-at-scale}

`prune-deps` handles target names and library lists stored in CMake variables, including loops and functions. A test project with 40 unused libraries still needs one trial and a final check. Shared variable definitions stay unchanged.

On XGrammar, removing an unused dependency avoids a roughly 32-second link after a GoogleMock source edit. The parser edit remains inconclusive. This is a targeted rebuild gain. Dependency cleanup has not yet shown a clean-build speedup on these projects.

## Next steps

1. Reduce repeated rejections on Highway. The search validates 35 removals but reaches its 64-trial limit, mostly on repeated `libhwy.a` entries in link commands. Keep the same binary checks while distinguishing redundant copies from a library removed entirely.
2. Support compiler response files: lists of arguments stored in `.rsp` files. They block the search on MuJoCo, where 236 links are now eligible.
3. Resolve CMake aliases, which still exclude SentencePiece's executable links from the search.

The [dependency results](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies-validation.md) separate the test project from the real-project measurements. No eligible removal means the supported search found none, not that every dependency is necessary.

## Later

Explain which dependency causes each rebuild. Then use measured symbol usage to assess smaller, specialized targets. A large static library alone does not justify splitting it: the linker already picks the object files it needs.
