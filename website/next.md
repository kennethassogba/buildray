---
title: What comes next
---

# Reduce the work an edit triggers

The next priority is to explain why targets rebuild, then extend dependency cleanup to more CMake declarations.

## Dependency cleanup today

`deps` records file and target relationships and reports cycles. `prune-links` can remove one redundant CMake declaration in a conservative case, then validate and measure the patch.

`prune-deps` now tests private static-library removal with archive extraction evidence, identical-binary link replay and full build and test validation. It can retain compilation requirements when only the link is unnecessary. Its scope is deliberately limited to literal declarations in one configuration.

## What to measure next

Link cleanup now records individual link-driver commands and durations, including incremental rebuild counts. The next step is to explain which dependency path caused each target to rebuild, and support variable-based CMake declarations without losing their meaning.

Public dependency interfaces still need separate checks across consumers. Narrowing a `PUBLIC` dependency or separating a shared header from frequently edited implementation must preserve the requirements those consumers receive.

## Can this speed up linking?

Yes, if a change reduces the linker's work or avoids running it. Updating a static library can require relinking its consumers. Avoiding those links can matter more than saving a second of compilation.

Fewer lines in `target_link_libraries` do not automatically mean less work. A library may remain reachable through another path. A seemingly unused dependency may also provide options, macros or behavior at load time.

The evidence should be a smaller link command or fewer triggered links, passing tests and a measured before/after gain. Automatic cycle breaking and a full dependency audit also remain to be built.
