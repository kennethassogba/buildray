# Validate a redundant CMake link

```bash
buildray prune-links --source /path/to/project --compiler clang++-21 \
  --exclude 3rdparty --runs 3 --jobs 2 --work-dir /tmp/project-links
# Optional restrictions:
buildray prune-links --source /path/to/project \
  --target my_tests --dependency test_library --compiler clang++-21
```

The command builds an isolated baseline and runs CTest. It reads CMake's configured
direct link and compilation relationships, then chooses one literal link whose
dependency is also reached through another linked target's interface. Both
linking and compilation usage requirements must survive every hop. A `PUBLIC`
link on a library also needs an alternate exported interface path.

The graph identifies configured redundancy, not unused symbols. A dependency
can remain necessary for its headers, definitions, link options or runtime effects
even if no symbol appears unused. Selection is deterministic by target and
dependency name. `--target` restricts the search; `--dependency` requests one
eligible link and fails when none matches. There is one experiment, without
fallback or repeated removal of other candidates.

Only an unquoted literal dependency in a literal `target_link_libraries` call is
edited. CMake backtraces locate the declaration. The lexer preserves comments and
consumes quoted/bracket arguments as whole tokens. Variables, generator
expressions, aliases spelled differently from their resolved target, duplicate
tokens, `debug`/`optimized` selections, legacy `LINK_*` signatures and ambiguous
declarations are skipped. Raw libraries such as `m`, linker flags, object-library
dependencies and interface-only declarations are not removed. Files generated or
downloaded during configuration are excluded. Repeatable `--exclude` paths are
relative to the source root and do not reduce build or test coverage.

The candidate is reconfigured. Its target inventory, raw linker arguments and
other target relationships must remain unchanged; the selected direct link and
its associated compilation/interface relationships must disappear. An associated
order relationship may remain or disappear. The command then performs a complete
clean build, linking and CTest. Baseline and candidate must have the same test
names and count. Arbitrary CMake scripts can respond to changed target properties,
so graph reachability alone never validates a patch.

At least three alternating clean-build pairs follow validation, without include
or time-trace instrumentation. The same existing rule decides whether to recommend
the patch: the gain must exceed `max(2% baseline median, 3 × larger MAD)`. This
heuristic is not a significance test. Removing a redundant declaration may change
no compiler or linker work, so an inconclusive result is expected in some cases.
A completed slower or inconclusive experiment returns 0 with
`patch_recommended: false`. A rejected experiment returns 1 and retains its logs.

No eligible candidate returns 0 with `performance_result: no_changes`, no patch
and no benchmark. An explicit unmatched `--dependency` returns 1. The original
tree is never edited. Only the supplied configuration and its registered tests
are validated; test exported consumers and other platforms separately.

Outputs:

- `links.json`: eligible candidates, selection, alternate paths and token location.
- `cmake.patch`: the selected deletion, including when later validation fails.
- `cmake-before.json`, `cmake-after.json`: configured target graph evidence.
- `report.json`: validation, raw build samples and recommendation.
- `logs/`: configure, build, compiler and CTest evidence.

Inspect `patch_validated` and `patch_recommended` before applying a patch. The
presence of a patch file alone does not imply validation.

```bash
git apply --check /tmp/project-links/cmake.patch
git apply /tmp/project-links/cmake.patch
```

Requires CMake 4.2+ for direct relationships. As with `optimize`, compilation
coverage checks concern the project's C++ compilation commands; C sources, if
present, still take part in the complete build. Existing PCH and unity builds
are disabled. This command does not remove include directives or headers.

## GitHub Action

```yaml
- uses: kennethassogba/buildray@YOUR_REVIEWED_COMMIT_SHA
  with:
    command: prune-links
    link-target: my_tests
    link-dependency: test_library
    exclude-paths: '["3rdparty"]'
    runs: '3'
    artifact-name: cmake-link-experiment
```

Both restrictions are optional. The `links`, `patch` and `report` outputs point to
retained artifacts, including graph evidence and failure diagnostics. A PR's
`baseline-ref` applies only to `measure` and is ignored here.

The implementation uses CMake's [direct target relationships and backtraces](https://cmake.org/cmake/help/v4.2/manual/cmake-file-api.7.html)
and [transitive link interfaces](https://cmake.org/cmake/help/v4.2/command/target_link_libraries.html).
In particular, `LINK_ONLY` and `COMPILE_ONLY` paths alone are insufficient proof
that both sets of usage requirements survive.
