"""Header cleanup and AST forward declarations with real compilers and failure guards."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary, compiler, tidy = sys.argv[1:]


def run(*args, expected=0, **kwargs):
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def digest(tree):
    return {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in tree.rglob("*") if p.is_file()}


with tempfile.TemporaryDirectory(prefix="buildray headers ") as temporary:
    root = Path(temporary)
    source = root / "source tree"
    source.mkdir()
    (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.30)
project(header_fixture LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 26)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "Header validation must select the matching Clang C driver")
endif()
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported)
if(ipo_supported)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()
add_executable(sample main.cpp api.cpp helper.c)
add_library(configured OBJECT variant.cpp)
target_compile_definitions(configured PRIVATE DETAIL_REQUIRED=1)
enable_testing()
add_test(NAME works COMMAND sample)
''')
    api = source / "api.hpp"
    api.write_text('''#pragma once
#include <set>
#include "detail.hpp"
namespace demo { int inspect(const Detail& value); }
''')
    (source / "detail.hpp").write_text('''#pragma once
#include <vector>
#include <string>
namespace demo { class Detail { public: std::vector<std::string> values; }; }
''')
    (source / "api.cpp").write_text('#include "api.hpp"\n#include "detail.hpp"\nint demo::inspect(const Detail& value) { return value.values.size(); }\n')
    (source / "guarded.hpp").write_text('''#pragma once
#include "detail.hpp"
#if DETAIL_REQUIRED
namespace demo { struct Guarded { Detail value; }; }
#else
namespace demo { void guarded(const Detail& value); }
#endif
''')
    (source / "variant.cpp").write_text('#include "guarded.hpp"\n')
    (source / "meta.hpp").write_text('''#pragma once
namespace demo {
template<class T> class Box {};
struct [[nodiscard]] Attributed {};
struct Outer { struct Inner {}; };
inline namespace v1 { struct Versioned {}; }
}
''')
    (source / "meta_api.hpp").write_text('''#pragma once
#include "meta.hpp"
#include <vector>
void opaque(demo::Box<int>*, demo::Attributed*, demo::Outer::Inner*, demo::Versioned*, std::vector<int>*);
''')
    (source / "kept.hpp").write_text('''#pragma once
#include "detail.hpp"  // IWYU pragma: keep
void keep(demo::Detail*);
''')
    (source / "context_only.hpp").write_text('void context_only(demo::Detail*);\n')
    (source / "c_required.h").write_text('#pragma once\nstruct CThing { int value; };\n')
    (source / "shared.h").write_text('#pragma once\n#include "c_required.h"\n')
    (source / "helper.c").write_text('#include "shared.h"\nint helper(void) { struct CThing value = {0}; return value.value; }\n')
    (source / "main.cpp").write_text('''#include "api.hpp"
#include "detail.hpp"
#include "guarded.hpp"
#include "meta_api.hpp"
#include "kept.hpp"
#include "context_only.hpp"
#include "shared.h"
extern "C" int helper(void);
int main() { demo::Detail value; return demo::inspect(value) + helper(); }
''')

    def invoke(label, *args, expected=0, runs=1):
        before = digest(source)
        work = root / label
        run(binary, "headers", "--source", source, "--work-dir", work,
            "--compiler", compiler, "--clang-tidy", tidy, "--runs", runs, "--jobs", 2,
            *args, expected=expected)
        assert digest(source) == before, "Header pipeline edited the original tree"
        return work, json.loads((work / "report.json").read_text())

    work, state = invoke("forward", "--forward-declarations", "--header-path", "api.hpp", runs=3)
    assert state["patch_validated"] and state["baseline_tests_passed"] == state["candidate_tests_passed"] == 1
    assert len(state["baseline"]["samples"]) == len(state["candidate"]["samples"]) == 3
    changed = (work / "candidate-source/api.hpp").read_text()
    assert "#include <set>" not in changed and '#include "detail.hpp"' not in changed, changed
    assert "class Detail;" in changed and "namespace demo" in changed
    summary = json.loads((work / "header-cleanup.json").read_text())
    assert {row["kind"] for row in summary["findings"]} == {"unused_include", "forward_declaration"}
    assert summary["ast_commands"] == 4 and summary["modified_headers"] == 1
    assert not (work / "includes.patch").exists()
    run("git", "apply", "--check", work / "headers.patch", cwd=source)
    original = api.read_text()
    run("git", "apply", work / "headers.patch", cwd=source)
    run("cmake", "-S", source, "-B", root / "applied", "-G", "Ninja", f"-DCMAKE_CXX_COMPILER={compiler}", f"-DCMAKE_C_COMPILER={compiler.replace("clang++", "clang")}")
    run("cmake", "--build", root / "applied", "--parallel", 2)
    run("ctest", "--test-dir", root / "applied", "--output-on-failure")
    api.write_text(original)

    _, plain = invoke("cleanup", "--header-path", "api.hpp")
    assert plain["patch_validated"] and all(row["kind"] == "unused_include" for row in plain["header_cleanup"]["findings"])
    for label, header in [("contexts", "guarded.hpp"), ("metadata", "meta_api.hpp"),
                          ("kept", "kept.hpp"), ("c-users", "shared.h"),
                          ("not-standalone", "context_only.hpp")]:
        _, report = invoke(label, "--forward-declarations", "--header-path", header)
        assert report["performance_result"] == "no_changes", (label, report)
        if label in ("contexts", "c-users"):
            assert report["header_cleanup"]["rejected"], label
        if label == "not-standalone":
            assert report["header_cleanup"]["skipped"]
    _, excluded = invoke("excluded", "--header-path", "api.hpp", "--exclude", "api.hpp", "--forward-declarations")
    assert excluded["performance_result"] == "no_changes"

    # A forced declaration can compile yet remove a required initialization effect.
    (source / "provider.hpp").write_text('''#pragma once
extern int shared_state;
inline int initialized = (shared_state = 1);
namespace demo { class Opaque {}; }
''')
    api.write_text('#pragma once\n#include "provider.hpp"\nnamespace demo { void use(Opaque*); }\n')
    (source / "api.cpp").write_text("int shared_state = 0;\n")
    (source / "main.cpp").write_text('#include "api.hpp"\nextern int shared_state; int main() { return shared_state == 1 ? 0 : 1; }\n')
    failed_work, failed = invoke("semantics", "--header-path", "api.hpp", "--forward-declarations", expected=1)
    assert not failed["patch_validated"] and not failed["patch_recommended"]
    assert "candidate-tests.log" in failed["error"] and (failed_work / "headers.patch").exists()
    run(binary, "measure", "--forward-declarations", expected=2)
    run(binary, "headers", "--header-path", "../escape", expected=2)
    run(binary, "headers", "--max-header-edits", 0, expected=2)

print("Header cleanup, AST declarations, context coverage, C users and behavioral rejection passed")
