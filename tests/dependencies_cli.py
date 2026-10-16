"""Real guarded include cycles, C/C++, target relationships, and default-build coverage."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary, compiler = sys.argv[1:]
c_compiler = compiler.replace("clang++", "clang")


def run(*args, expected=0):
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def digest(tree):
    return {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in tree.rglob("*") if p.is_file()}


with tempfile.TemporaryDirectory(prefix="buildray deps ") as folder:
    root = Path(folder)
    source = root / "source tree"
    source.mkdir()
    (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 4.2)
project(deps_fixture LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(a STATIC a.cpp)
add_library(b STATIC b.cpp)
add_library(api INTERFACE)
target_link_libraries(a PUBLIC b api m)
target_link_libraries(b PUBLIC a)
add_executable(sample "main file.cpp" helper.c)
target_link_libraries(sample PRIVATE a)
add_library(unbuilt STATIC EXCLUDE_FROM_ALL broken.cpp)
add_subdirectory(one)
add_subdirectory(two)
enable_testing()
add_test(NAME works COMMAND sample)
''')
    (source / "a.cpp").write_text("int a() { return 0; }\n")
    for name in ["one", "two"]:
        directory = source / name
        directory.mkdir()
        (directory / "CMakeLists.txt").write_text(f'''add_library(Local::api INTERFACE IMPORTED)
add_library({name}_api INTERFACE)
target_link_libraries({name}_api INTERFACE Local::api)
''')
    (source / "b.cpp").write_text("int b() { return 0; }\n")
    (source / "broken.cpp").write_text("#error excluded target should not build\n")
    (source / "a header.hpp").write_text('''#ifndef A_HEADER_HPP
#define A_HEADER_HPP
#include "b.hpp"
#include "self.hpp"
#endif
''')
    (source / "b.hpp").write_text('#pragma once\n#include "a header.hpp"\n')
    (source / "self.hpp").write_text('#pragma once\n#include "self.hpp"\n')
    (source / "c.h").write_text('#pragma once\n#include "other.h"\n')
    (source / "other.h").write_text('#pragma once\n#include "c.h"\n')
    (source / "inactive.h").write_text("#error inactive branch must not appear\n")
    (source / "main file.cpp").write_text('''#include "a header.hpp"
#include "a header.hpp"
#if 0
#include "inactive.h"
#endif
int main() { return 0; }
''')
    (source / "helper.c").write_text('#include "c.h"\nint helper(void) { return 0; }\n')
    before = digest(source)

    def invoke(name, *extra, expected=0):
        work = root / name
        output = run(binary, "deps", "--source", source, "--work-dir", work,
                     "--compiler", compiler, "--c-compiler", c_compiler, *extra, expected=expected)
        return work, output

    work, output = invoke("cycles", "--fail-on-cycles", expected=5)
    graph = json.loads((work / "dependencies.json").read_text())
    report = json.loads((work / "report.json").read_text())
    assert report["status"] == "passed" and report["baseline_tests_passed"] == 1
    assert report["cycle_gate_passed"] is False and report["include_cycles"] == 3
    assert report["fail_on_cycles"] is True
    assert report["target_link_cycles"] == 1 and report["runs"] == 1
    assert "measurement_context" not in report and "baseline" not in report
    assert graph["compilations"] == 4 and graph["c_compilations"] == 1
    assert graph["cxx_compilations"] == 3
    cycles = {tuple(row["members"]) for row in graph["include_cycles"]}
    assert ("<SOURCE>/a header.hpp", "<SOURCE>/b.hpp") in cycles
    assert ("<SOURCE>/self.hpp",) in cycles
    assert ("<SOURCE>/c.h", "<SOURCE>/other.h") in cycles
    assert all("inactive.h" not in node for node in graph["nodes"])
    assert all("broken.cpp" not in node for node in graph["nodes"])
    repeated = next(e for e in graph["edges"]
                    if e["from"] == "<SOURCE>/main file.cpp" and e["to"] == "<SOURCE>/a header.hpp")
    assert repeated["compilations"] == 1, "Repeated includes must count once per invocation"
    assert graph["cmake"]["link_cycles"][0]["members"] == ["a", "b"]
    assert any(t["name"] == "unbuilt" for t in graph["cmake"]["targets"])
    local_apis = [t["name"] for t in graph["cmake"]["targets"] if t["name"].startswith("Local::api")]
    assert len(set(local_apis)) == 2, "Directory-scoped imported targets must remain distinct"
    edge = next(e for e in graph["cmake"]["edges"]
                if e["from"] == "a" and e["to"] == "api" and e["kind"] == "linkLibraries")
    assert edge["file"] == "<SOURCE>/CMakeLists.txt" and edge["line"] == 8
    fragments = [f for f in graph["cmake"]["link_fragments"] if f["from"] == "a"]
    assert {f["kind"] for f in fragments} == {"linkLibraries", "interfaceLinkLibraries"}
    assert all(f["fragment"] == "m" and f["line"] == 8 for f in fragments)
    assert all(t["name"] != "m" for t in graph["cmake"]["targets"])
    assert "guarded include cycles" in (work / "dependencies.md").read_text()
    dot = (work / "dependencies.dot").read_text()
    assert '"include:<SOURCE>/self.hpp" -> "include:<SOURCE>/self.hpp"' in dot
    assert digest(source) == before
    invoke("informational")  # Cycles are informational unless the explicit gate is enabled.

    # Remove the edges closing every cycle; the gate should now pass.
    (source / "b.hpp").write_text("#pragma once\n")
    (source / "self.hpp").write_text("#pragma once\n")
    (source / "other.h").write_text("#pragma once\n")
    cmake = source / "CMakeLists.txt"
    cmake.write_text(cmake.read_text().replace("target_link_libraries(b PUBLIC a)", ""))
    clean, _ = invoke("acyclic", "--fail-on-cycles")
    clean_report = json.loads((clean / "report.json").read_text())
    assert clean_report["cycle_gate_passed"] and clean_report["include_cycles"] == clean_report["target_link_cycles"] == 0

    # A C launcher override must not silently produce a partial C++-only graph.
    cmake.write_text(cmake.read_text() + 'set_property(TARGET sample PROPERTY C_COMPILER_LAUNCHER "")\n')
    bypass, _ = invoke("bypass", expected=1)
    assert "coverage" in json.loads((bypass / "report.json").read_text())["error"]
    cmake.write_text(cmake.read_text().replace('set_property(TARGET sample PROPERTY C_COMPILER_LAUNCHER "")\n', ""))
    # Ordinary compiler diagnostics must survive trace capture.
    (source / "helper.c").write_text("#error visible_compile_failure\n")
    failed, _ = invoke("failed", expected=1)
    assert "visible_compile_failure" in (failed / "logs/baseline-warmup.log").read_text()
    assert not (failed / "dependencies.json").exists()
    run(binary, "measure", "--fail-on-cycles", expected=2)

print("dependency graph integration passed")
