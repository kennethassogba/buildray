---
title: What comes next
---

# Reduce the work an edit triggers

The proposed next priority is to understand and reduce unnecessary rebuilds, with a closer look at linking. MuJoCo provides a concrete case to investigate.

## Dependency cleanup today

`deps` records file and target relationships and reports cycles. `prune-links` can remove one redundant CMake declaration in a conservative case, then validate and measure the patch.

This first cleanup does not yet find every library a program could do without. A declaration can be redundant while leaving exactly the same linker inputs. Removing it then simplifies CMake without speeding up the link.

## What to measure next

We need separate link-command timings and an explanation of which compilations and links an edited file triggers. Reports could then identify expensive targets and why they rebuild. Detailed linker measurement still needs implementation.

That would let us test specific changes: narrow a `PUBLIC` dependency, remove an unnecessary library, or separate a widely shared header from frequently edited implementation. Each requires checking the requirements transmitted to other targets, then building and testing the affected consumers.

## Can this speed up linking?

Yes, if a change reduces the linker's work or avoids running it. Updating a static library can require relinking its consumers. Avoiding those links can matter more than saving a second of compilation.

Fewer lines in `target_link_libraries` do not automatically mean less work. A library may remain reachable through another path. A seemingly unused dependency may also provide options, macros or behavior at load time.

The evidence should be a smaller link command or fewer triggered links, passing tests and a measured before/after gain. Automatic cycle breaking and a full dependency audit also remain to be built.
