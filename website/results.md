---
title: Results and limits
---

# What the experiments show

We test Buildray on real projects with fixed revisions and settings. These results do not predict performance on your machine.

## Full builds: Unity on XGrammar

Each row uses three alternating pairs on the same runner, with all 83 C++ tests passing before and after.

| Jobs | Before → Unity | Faster |
| ---: | ---: | ---: |
| 1 | 172.46 → 136.32 s | 21% |
| 2 | 103.02 → 85.01 s | 17.5% |
| 4 | 67.08 → 56.36 s | 16% |
| 8 | 70.46 → 60.85 s | 13.6% |

Linux, Clang 21, Release, native tests enabled, Python bindings disabled. Levels 1/2 and 4/8 used separate four-CPU runners; eight jobs did not mean eight CPUs. [Reports and independent verification](https://github.com/kennethassogba/buildray/actions/runs/39129371937).

## Selected before/after measurements

Every row uses three alternating pairs, with full build and test validation. Compare values within a row; different rows may come from different runners.

| Project and experiment | Edited file | Before → after | Result |
| --- | --- | --- | --- |
| XGrammar, Unity | `cpp/earley_parser.cc` | 26.245 → 29.676 s | 13.07% slower |
| XGrammar, Unity | `cpp/grammar_impl.h` | 58.397 → 50.094 s | 14.22% faster |
| SentencePiece, PCH | `src/sentencepiece_processor.cc` | 3.933 → 3.257 s | 17.19% faster |
| SentencePiece, PCH | `src/common.h` | 50.036 → 51.494 s | Inconclusive |
| MuJoCo, PCH | `src/user/user_model.cc` | 227.502 → 227.547 s | Inconclusive |

Unity can speed up a header rebuild while slowing down a source rebuild. SentencePiece's PCH gain depends on reuse: editing `common.h` requires regenerating it. These MuJoCo experiments do not establish an overall PCH speedup.

The [18 scenarios, configurations and raw reports](https://github.com/kennethassogba/buildray/blob/main/docs/incremental-validation.md) explain each result. Tested revisions are XGrammar `7e269d5`, SentencePiece v0.2.2 `e0cce7d` and MuJoCo `651bae1`. SentencePiece uses `src/common.h` on `sentencepiece-static`; this is separate from the older `spm_test` experiment.

## Why linking deserves a closer look

One MuJoCo rebuild log contains 80 link steps. Compiling the edited source takes about five seconds, while the default build takes about 228 seconds. That warrants investigating what triggers those links and which are required. It does not prove that a particular dependency is unnecessary.

Both variants use the same setting to work around temporary dependencies emitted by gold with LTO. Without it, the build kept linking even without an edit, so Buildray refused incremental timings. The evidence records the setting and diagnosis.

## Dependency cleanup so far

The grouped search removes 40 unused library links in one trial on a test project, including when the libraries come from a CMake variable. A final check verifies the combined patch. [Dependency results and limits](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies-validation.md).

On XGrammar, removing the unused `gmock` link avoids a roughly 32-second LTO link when `gmock-all.cc` changes: rebuild time falls from 34.768 to 2.589 s, with all 83 tests passing. Editing the parser stays inconclusive at 35.769 → 35.995 s. This gain applies to changes in GoogleMock; it does not establish a faster clean build. SentencePiece still has no supported candidate because its programs use aliases. MuJoCo passes 3,519 tests, but response files block the search.

The first Google Highway run checked 17 links across 15 programs and validated one removal. The full build passed 2,784 tests, keeping the same 23 intentional skips. The measured `vqsort.cc` rebuild stayed inconclusive: 4.415 → 4.448 s. That edit affects a different library from the removed link; that version could not handle the variables in most test dependencies.

A separate build of the core and examples, with tests disabled, checks an edit to the library concerned. It avoids one link (13 → 12), but its timings remain inconclusive: 4.433 → 4.027 s. These measurements do not establish an overall speedup.

## Limits of the evidence

Tests cover the supplied configuration. They do not replace validation on other platforms or of a library's consumers. Three pairs and a variation threshold are a screening method, not a statistical study.

A correct cleanup can be useful without a measurable speedup. We distinguish a validated patch from a recommended optimization. [Next steps](./next.md) focus on work that can actually be avoided.
