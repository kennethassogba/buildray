"""Real Clang traces and CMake PCH builds, including invalid and harmful candidates."""
import hashlib
import json
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

binary, compiler = sys.argv[1:]


def run(*args, expected=0, **kwargs):
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def digest(tree):
    return {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in tree.rglob("*") if p.is_file()}


with tempfile.TemporaryDirectory(prefix="buildray profile ") as folder:
    root = Path(folder)
    source = root / "source tree"
    source.mkdir()
    (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.30)
project(profile_fixture LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(lib)
enable_testing()
add_test(NAME works COMMAND sample)
''')
    (source / "lib").mkdir()
    (source / "lib/CMakeLists.txt").write_text('''add_executable(sample "main file.cpp" second.cpp helper.c)
target_include_directories(sample PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
''')
    (source / "lib/common header.hpp").write_text('''#pragma once
#include <vector>
#include <string>
inline int answer() { return std::vector<std::string>{"ok"}.size() != 1; }
''')
    (source / "lib/main file.cpp").write_text('''#include "common header.hpp"
int second();
int main() { return answer() + second(); }
''')
    (source / "lib/second.cpp").write_text('#include "common header.hpp"\nint second() { return answer(); }\n')
    (source / "lib/helper.c").write_text("int c_helper(void) { return 0; }\n")
    original = digest(source)

    def invoke(mode, name, *extra, expected=0, runs=1):
        work = root / name
        run(binary, mode, "--source", source, "--work-dir", work, "--compiler", compiler,
            "--runs", runs, "--jobs", 2, *extra, expected=expected)
        return work

    profiled = invoke("profile", "profile")
    profile = json.loads((profiled / "headers.json").read_text())
    report = json.loads((profiled / "report.json").read_text())
    assert report["status"] == "passed" and report["baseline_tests_passed"] == 1
    assert "baseline" not in report, "trace overhead must not become a benchmark"
    assert report["runs"] == 1 and report["profiling_context"]["method"] == "clang-time-trace-v1"
    assert "measurement_context" not in report
    assert profile["compilations"] == 2
    common = next((h for h in profile["headers"] if h["header"] == "<SOURCE>/lib/common header.hpp"), None)
    assert common, profile
    assert common["compilations"] == 2 and common["inclusive_seconds"] > 0, common
    assert profile["targets"][0]["target"] == "sample"
    assert "nested headers overlap" in (profiled / "headers.md").read_text()

    pch = invoke("pch", "pch", "--target", "sample", "--header", "<vector>",
                 "--header", "lib/common header.hpp", runs=3)
    result = json.loads((pch / "report.json").read_text())
    assert result["patch_validated"] and result["baseline_tests_passed"] == result["candidate_tests_passed"] == 1
    assert len(result["baseline"]["samples"]) == len(result["candidate"]["samples"]) == 3
    assert result["performance_result"] in ("improved", "regressed", "inconclusive")
    assert result["patch_recommended"] == (result["performance_result"] == "improved")
    assert digest(source) == original
    patch = pch / "pch.patch"
    run("git", "apply", "--check", patch, cwd=source)
    run("git", "apply", patch, cwd=source)
    assert (source / "CMakeLists.txt").read_bytes() == (pch / "candidate-source/CMakeLists.txt").read_bytes()
    # A patch must work outside Buildray, including on the C source in this target.
    run("cmake", "-S", source, "-B", root / "applied", "-G", "Ninja", f"-DCMAKE_CXX_COMPILER={compiler}")
    run("cmake", "--build", root / "applied", "--parallel", 2)
    run("ctest", "--test-dir", root / "applied", "--output-on-failure")
    run("git", "apply", "--reverse", patch, cwd=source)
    assert digest(source) == original
    records = json.loads((pch / "logs/candidate-0-compilers.json").read_text())
    assert any("-emit-pch" in r["command"] for r in records), "PCH generation must be measured"
    assert sum("-include-pch" in r["command"] for r in records) == 2

    # Automatic selection profiles first, then uses the same validated paired experiment.
    auto = invoke("pch", "auto", "--auto", runs=3)
    state = json.loads((auto / "report.json").read_text())
    assert state["pch_auto"] and state["patch_validated"] and state["status"] == "passed"
    assert state["automatic_selection"]["target"] == "sample"
    assert state["automatic_selection"]["header"] == "lib/common header.hpp"
    assert state["automatic_selection"]["compilations"] == 2
    assert state["selection_profiling_in_measurements"] is False
    assert len(state["baseline"]["samples"]) == len(state["candidate"]["samples"]) == 3
    assert (auto / "headers.json").is_file() and (auto / "selection.json").is_file()
    assert "measurement_context" not in json.loads((auto / "profile/report.json").read_text())
    assert digest(source) == original
    run("git", "apply", "--check", auto / "pch.patch", cwd=source)
    # A work directory inside the source must not recursively copy profiling outputs.
    nested = source / "automatic-output"
    run(binary, "pch", "--auto", "--target", "sample", "--source", source,
        "--work-dir", nested, "--compiler", compiler, "--runs", 3, "--jobs", 2)
    assert not (nested / "input/automatic-output").exists()
    assert json.loads((nested / "report.json").read_text())["status"] == "passed"
    shutil.rmtree(nested)
    assert digest(source) == original

    # A repeated header can still change TUs that did not previously include it.
    main = source / "lib/main file.cpp"
    common = source / "lib/common header.hpp"
    library_cmake = source / "lib/CMakeLists.txt"
    saved_main, saved_common, saved_cmake = main.read_text(), common.read_text(), library_cmake.read_text()
    main.write_text('''int second(); int third();
int main() {
#ifdef AUTO_PCH_PRESENT
return 1;
#else
return second() + third();
#endif
}
''')
    common.write_text(saved_common + "\n#define AUTO_PCH_PRESENT 1\n")
    third = source / "lib/third.cpp"
    third.write_text('#include "common header.hpp"\nint third() { return answer(); }\n')
    library_cmake.write_text(saved_cmake + "\ntarget_sources(sample PRIVATE third.cpp)\n")
    harmful = invoke("pch", "auto-harmful", "--auto", runs=3, expected=1)
    failed_auto = json.loads((harmful / "report.json").read_text())
    assert not failed_auto["patch_recommended"] and "ctest exited" in failed_auto["error"]
    assert (harmful / "logs/automatic-pch.log").is_file()
    main.write_text(saved_main)
    common.write_text(saved_common)
    library_cmake.write_text(saved_cmake)
    third.unlink()
    assert digest(source) == original
    run(binary, "profile", "--auto", expected=2)
    run(binary, "pch", "--auto", "--header", "<vector>", expected=2)
    run(binary, "pch", "--auto", "--runs", 1, expected=2)

    empty = root / "headerless-source"
    empty.mkdir()
    (empty / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.30)
project(headerless LANGUAGES CXX)
add_executable(app main.cpp)
enable_testing()
add_test(NAME works COMMAND app)
''')
    (empty / "main.cpp").write_text("int main() { return 0; }\n")
    no_candidate = root / "no-candidate"
    run(binary, "pch", "--auto", "--source", empty, "--work-dir", no_candidate,
        "--compiler", compiler, expected=1)
    assert "No eligible project header" in json.loads((no_candidate / "report.json").read_text())["error"]
    assert not (no_candidate / "experiment").exists()

    # Candidate compilation and tests must still reject a bad header/changed semantics.
    (source / "lib/bad.hpp").write_text("#error invalid PCH\n")
    bad = invoke("pch", "bad", "--target", "sample", "--header", "lib/bad.hpp", expected=1)
    assert not json.loads((bad / "report.json").read_text())["patch_recommended"]
    (source / "lib/bad.hpp").write_text("#pragma once\n#define PCH_PRESENT 1\n")
    main = source / "lib/main file.cpp"
    old_main = main.read_text()
    main.write_text(old_main.replace("return answer() + second();", """return answer() + second()
#ifdef PCH_PRESENT
 + 1
#endif
;"""))
    macro = invoke("pch", "macro", "--target", "sample", "--header", "lib/bad.hpp", expected=1)
    assert "ctest exited" in json.loads((macro / "report.json").read_text())["error"]
    main.write_text(old_main)
    missing = invoke("pch", "missing", "--target", "missing", "--header", "<vector>", expected=1)
    assert json.loads((missing / "report.json").read_text())["status"] == "failed"
    for header in ["../escape.hpp", "<vector>;message(FATAL_ERROR bad)", "<vector$>"]:
        invoke("pch", "invalid", "--target", "sample", "--header", header, expected=1)
        assert not (root / "invalid").exists()
    run(binary, "pch", "--source", source, expected=2)
    run(binary, "measure", "--target", "sample", expected=2)
    library_cmake = source / "lib/CMakeLists.txt"
    original_library = library_cmake.read_text()
    library_cmake.write_text(original_library.replace(" helper.c", "") + '''
add_library(pch_interface INTERFACE)
target_precompile_headers(pch_interface INTERFACE <map>)
target_link_libraries(sample PRIVATE pch_interface)
''')
    inherited = invoke("pch", "inherited", "--target", "sample", "--header", "<vector>", expected=1)
    assert "inherited PCHs" in json.loads((inherited / "report.json").read_text())["error"]
    library_cmake.write_text(original_library)
    # Existing PCHs stay disabled in profile, and cannot be silently mixed into an experiment.
    with (source / "lib/CMakeLists.txt").open("a") as stream:
        stream.write('target_precompile_headers(sample PRIVATE <string>)\n')
    invoke("profile", "existing-profile")
    invoke("pch", "existing-pch", "--target", "sample", "--header", "<vector>", expected=1)

print("profiling and PCH integration passed")
