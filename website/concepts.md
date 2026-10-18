---
title: How a build works
---

# What happens during a build

A build turns source code into a program or library. CMake describes the work; Ninja runs the required commands.

<BuildFlow />

## Headers and includes

A source file, usually `.cc` or `.cpp`, contains code to compile. A header, usually `.h` or `.hpp`, describes types and functions shared by several files.

When a source writes `#include "model.h"`, the compiler reads that header and the headers it includes. Twenty sources using the same large header repeat some of that analysis twenty times. Removing an unnecessary include can avoid work.

A **forward declaration** provides only a type's name, such as `class Model;`. That can be enough to declare a pointer to the type. Knowing its size, accessing its members or destroying some objects requires the full definition. Buildray checks observed uses before proposing this replacement.

## Compiling and linking

The compiler produces object files, usually `.o`. The **linker** combines objects and libraries into the final output. That step is called **linking**.

A header mainly costs compilation time. A library used by many executables can trigger many links when it changes. Saving compiler time therefore does not always speed up the whole build.

A **CMake target** is something to build, such as a library or test executable. A target dependency can transmit libraries, header paths, macros and compiler options.

## PCH: analyze a header once

A *precompiled header* stores an already analyzed form of headers. Compatible sources can reuse it instead of repeating that analysis.

PCH works best with expensive headers that many sources share and that rarely change. Creating it takes time and memory. A change to a header it contains requires rebuilding the PCH and affected sources. It also depends on the compiler and options used.

Buildray's PCH experiment is private to one target. It does not try to impose a PCH on the library's consumers. [How CMake handles PCH](https://cmake.org/cmake/help/latest/command/target_precompile_headers.html).

## Unity: compile sources together

A Unity build combines several source files into one compilation. This can avoid parsing some headers separately for each file.

Editing one source may then require recompiling its entire group. Names that worked in separate files can also collide. Buildray verifies groups and can isolate incompatible files; measure rebuilds before keeping the patch.

## An incremental rebuild

After an initial build, the build system reconstructs what an edit makes necessary. This is an **incremental rebuild**. Editing one source often affects few files; editing a public header can affect dozens.

A faster full build does not guarantee a faster rebuild after an edit. [Buildray measures both](./measure.md).
