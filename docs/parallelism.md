# Compare build parallelism

A Unity or PCH result at two jobs may not hold at eight. More jobs can hide
repeated header parsing, while larger compilation units can leave fewer tasks
to run in parallel. Check the job counts you actually use.

```bash
buildray unity --source /path/to/project --target core \
  --batch-size 4 --job-levels 1,2,4,8 --runs 3

buildray pch --source /path/to/project --target core \
  --header src/common.h --job-levels 1,2,4,8 --runs 3 \
  --rebuild-file src/model.cc
```

`--job-levels` replaces `--jobs` for Unity and manually selected PCH experiments.
It accepts two to eight distinct counts. Each count needs at least three paired
samples; automatic PCH selection and Unity retries are disabled for this mode.
Use the same explicit headers, Unity batch size and exclusions at every level.

Buildray freezes the source once, then runs the levels sequentially on the same
machine. Each level gets independent build trees, complete build/test validation
and alternating baseline/candidate timings. It checks that the machine,
toolchain, configuration and candidate patch stay the same. Unity groups must
also match. A failed or interrupted level leaves earlier results available and
prevents an aggregate validated patch.

`parallelism.md` gives the comparison table. `parallelism.json` and `report.json`
record the levels, with complete reports under `jobs-N/`. Clean and incremental
results stay separate; samples from different job counts are never pooled. The
top-level patch is recommended only if every level recommends it and no measured
incremental scenario regresses.

CPU affinity and cgroup quota are recorded for every level. A job count above
the detected capacity is marked explicitly. For example, eight jobs on a
four-CPU runner measures oversubscription, not performance on an eight-core
machine. Per-compiler peak RSS is also retained; it is not total build memory.
More job levels mean more full builds, so allow enough time for the run.

## GitHub Action

```yaml
- uses: kennethassogba/buildray@main
  with:
    command: unity
    unity-target: core
    unity-batch-size: '4'
    job-levels: '[1,2,4,8]'
    runs: '3'
    jobs: '2'
```

With `job-levels`, the Action's `jobs` input controls compilation of Buildray
itself. The listed levels control the measured project. Outputs `parallelism`
and `parallelism-summary` point to the comparison, and artifacts retain every
child report, patch, measurement and test log.
The `artifact-path` output gives the directory containing this evidence.

The [XGrammar workflow](../.github/workflows/parallelism.yml) runs two independent
sweeps, `1,2` and `4,8`, to reduce CI turnaround. Each sweep uses its own runner.
Compare Unity on/off within a row; absolute times from the separate runners are
not a controlled comparison between job counts. The workflow also builds the
actual option in upstream PR #995 and checks that its eight Unity groups match
the measured groups.
