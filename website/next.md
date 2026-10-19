---
title: What comes next
---

# Test dependency cleanup at scale

`prune-deps` already tests removals in groups. A test project with 40 unused libraries needs one trial and a final check. The Highway run validates one removal across 15 profiled programs, with 2,784 passing tests and 23 unchanged skips. Most of its test dependencies use CMake variables, which the search cannot handle yet. The real-project runs have not demonstrated an overall build-time gain.

## Next steps

1. Support library lists and target names expressed through CMake variables, then repeat the Highway search on the test programs it currently skips.
2. Support compiler response files: lists of arguments stored in `.rsp` files. They currently block the search on MuJoCo.
3. Validate groups of confirmed unused dependencies on those projects, recording search cost and rebuilds avoided. One removal does not establish performance at scale.

The [dependency results](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies-validation.md) separate the test project from the real-project measurements. No eligible removal means the supported search found none, not that every dependency is necessary.

## Later

Explain which dependency causes each rebuild. Then use measured symbol usage to assess smaller, specialized targets. A large static library alone does not justify splitting it: the linker already picks the object files it needs.
