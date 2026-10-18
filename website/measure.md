---
title: Read the measurements
---

# Tell whether a gain is measured

Build times vary. Buildray repeats builds and alternates which variant runs first, on the same machine.

## What the timer includes

The main duration covers the build through linking. CMake configuration, analysis, cleaning and tests are outside that interval. Compiler caches are disabled; the operating system's file cache stays warm.

The **median** is the middle duration after sorting the samples. It is less affected by an unusually slow sample than the mean. **MAD** describes variation around that median; a larger value means less consistent timings.

To recommend an optimization, Buildray requires at least three build pairs and a gain above the larger of two thresholds: 2% of the baseline median, or three times the larger MAD. This is a practical rule, not a statistical significance test.

## Read the verdict

| Result | What it means |
| --- | --- |
| `improved` | The gain exceeds this run's variation threshold. |
| `regressed` | The slowdown exceeds this run's threshold. |
| `inconclusive` | The measurements do not establish a conclusion. |
| `insufficient_samples` | There are too few samples to recommend the patch. |
| `no_changes` | No eligible change was produced. |
| `not_measured` | This type of timing was not measured. |

`status: passed` means the pipeline completed. `patch_validated` records the candidate's validation. `patch_recommended` also requires a measured gain. These fields answer different questions.

## Measure everyday edits

```bash
buildray pch --source /path/to/project \
  --target my_library --header include/common.h \
  --rebuild-file src/model.cc --rebuild-file include/model.h \
  --incremental-only --runs 3 --jobs 2
```

Buildray prepares both builds and checks that they have no work left. It then adds a comment to one file and measures the rebuild. It repeats this for each requested file, runs CTest after every timed build and restores the original contents in its copies.

The comment reveals which files an edit causes to rebuild. It does not simulate every semantic code change. Reports retain compiler counts and PCH creation. Reported memory is the peak of one compiler invocation, not total build or linker memory.

Dependency cleanup also records link counts and the time spent in link-driver commands. That includes driver startup. Parallel links can overlap, so their summed duration is not total elapsed build time. An unused dependency may cost little in a clean build but still cause unnecessary relinks after an edit.

`--incremental-only` skips repeated full-build timings while retaining build and test validation. It does not automatically recommend a full-build improvement. When both types of timing are enabled, a measured incremental slowdown blocks that recommendation.

## Compare revisions

```bash
buildray compare --baseline-source /path/to/baseline \
  --source /path/to/current --runs 5 --jobs 2 \
  --max-regression-percent 5
```

Exit code `3` indicates a slowdown beyond both the budget and the noise threshold. An inconclusive result can pass this gate. Use more samples on a quieter machine before drawing a conclusion.

The [incremental protocol](https://github.com/kennethassogba/buildray/blob/main/docs/incremental.md) and [CI comparison reference](https://github.com/kennethassogba/buildray/blob/main/docs/ci.md) explain the details.
