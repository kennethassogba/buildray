---
title: What comes next
---

# Test dependency cleanup at scale

`prune-deps` already tests removals in groups. A test project with 40 unused libraries needs one trial and a final check. We have also run it on XGrammar, SentencePiece and MuJoCo, but have not demonstrated a build-time gain from those removals.

## Next steps

1. Test groups of 100 and 1,000 dependencies, including libraries that must stay. Measure search time, trial count and memory use.
2. Support compiler response files: lists of arguments stored in `.rsp` files. They currently block the search on MuJoCo.
3. Validate many removals together on a real project. Measure complete builds and rebuilds after library edits, including links avoided. Passing the tests alone is not a speedup.

The [dependency results](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies-validation.md) separate the test project from the real-project measurements. No eligible removal means the supported search found none, not that every dependency is necessary.

## Later

Explain which dependency causes each rebuild. Then use measured symbol usage to assess smaller, specialized targets. A large static library alone does not justify splitting it: the linker already picks the object files it needs.
