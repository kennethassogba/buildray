"""Exercise real C/C++ unity builds, source coverage, collisions and runtime failures."""
import hashlib
import json
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


with tempfile.TemporaryDirectory(prefix="buildray unity ") as folder:
    root = Path(folder)
    source = root / "source tree"
    lib = source / "lib"
    lib.mkdir(parents=True)
    (lib / "c").mkdir()
    top = source / "CMakeLists.txt"
    top.write_text('''cmake_minimum_required(VERSION 3.30)
project(unity_fixture LANGUAGES C CXX)
add_subdirectory(lib)
add_executable(other other.cpp)
add_library(optional EXCLUDE_FROM_ALL broken.cpp)
enable_testing()
add_test(NAME works COMMAND sample)
''')
    cmake = lib / "CMakeLists.txt"
    cmake.write_text('''add_executable(sample "main file.cpp" second.cpp third.cpp c/a.c c/b.c)
set_source_files_properties(third.cpp PROPERTIES COMPILE_DEFINITIONS MUST_STAY_SEPARATE=1)
''')
    main = lib / "main file.cpp"
    main.write_text('''int second(); int third();
extern "C" int a(void); extern "C" int b(void);
int main() { return second() + third() + a() + b(); }
''')
    (lib / "second.cpp").write_text("int second() { return 0; }\n")
    (lib / "third.cpp").write_text('''#ifndef MUST_STAY_SEPARATE
#error missing per-file definition
#endif
int third() { return 0; }
''')
    (lib / "c/a.c").write_text("int a(void) { return 0; }\n")
    (lib / "c/b.c").write_text("int b(void) { return 0; }\n")
    (source / "other.cpp").write_text("int main() { return 0; }\n")
    (source / "broken.cpp").write_text("#error excluded target must remain outside the experiment\n")
    original = digest(source)

    def invoke(name, *extra, expected=0, runs=1):
        work = root / name
        run(binary, "unity", "--source", source, "--target", "sample", "--compiler", compiler,
            "--work-dir", work, "--runs", runs, "--jobs", 2, *extra, expected=expected)
        return work, json.loads((work / "report.json").read_text())

    work, report = invoke("valid", "--batch-size", 2, runs=3)
    assert report["patch_validated"] and report["baseline_tests_passed"] == report["candidate_tests_passed"] == 1
    assert report["candidate_unity_build"] and not report["unity_build"]
    assert not report["precompiled_headers"]
    assert len(report["baseline"]["samples"]) == len(report["candidate"]["samples"]) == 3
    assert report["patch_recommended"] == (report["performance_result"] == "improved")
    plan = json.loads((work / "unity.json").read_text())
    assert plan["source_target_coverage_verified"] and plan["original_compilations"] == 6
    assert plan["baseline_compiler_invocations"] == 6 and plan["candidate_compiler_invocations"] == 4
    assert len(plan["groups"]) == 2 and all(len(g["sources"]) == 2 for g in plan["groups"])
    assert all(not any("third.cpp" in f for f in g["sources"]) for g in plan["groups"])
    assert report["measurement_context"]["unity_build"] is False
    assert report["candidate_measurement_context"]["unity_build"] is True
    assert digest(source) == original
    patch = work / "unity.patch"
    run("git", "apply", "--check", patch, cwd=source)
    run("git", "apply", patch, cwd=source)
    # The published patch is opt-in and also works without the Buildray launcher.
    for enabled in ["OFF", "ON"]:
        applied = root / ("applied-" + enabled)
        run("cmake", "-S", source, "-B", applied, "-G", "Ninja",
            f"-DCMAKE_CXX_COMPILER={compiler}", f"-DBUILDRAY_UNITY={enabled}",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
        run("cmake", "--build", applied, "--parallel", 2)
        run("ctest", "--test-dir", applied, "--output-on-failure")
        commands = json.loads((applied / "compile_commands.json").read_text())
        assert any("/Unity/" in c["file"] for c in commands) == (enabled == "ON")
    run("git", "apply", "--reverse", patch, cwd=source)
    assert digest(source) == original

    # One pair validates, but cannot recommend a performance optimization.
    _, short = invoke("one-pair", "--unity-exclude", "lib/main file.cpp")
    assert short["patch_validated"] and short["performance_result"] == "insufficient_samples"
    assert not short["patch_recommended"]
    _, directory = invoke("directory-exclusion", "--unity-exclude", "lib/c")
    assert directory["patch_validated"]
    assert all(not any(f.endswith(".c") for f in g["sources"]) for g in directory["unity_plan"]["groups"])

    # A clean baseline cannot hide unity collisions or changed runtime behavior.
    saved_main, saved_second = main.read_text(), (lib / "second.cpp").read_text()
    main.write_text("namespace { int duplicate = 0; }\n" + saved_main)
    (lib / "second.cpp").write_text("namespace { int duplicate = 1; }\n" + saved_second)
    _, failed = invoke("collision", expected=1)
    assert "Build failed" in failed["error"] and not failed["patch_validated"]
    _, fixed = invoke("excluded-collision", "--unity-exclude", "lib/main file.cpp")
    assert fixed["patch_validated"]
    main.write_text(saved_main)
    (lib / "second.cpp").write_text('''int second() {
#if __INCLUDE_LEVEL__ > 0
return 1;
#else
return 0;
#endif
}
''')
    _, failed = invoke("runtime", expected=1)
    assert "ctest exited" in failed["error"] and not failed["patch_validated"]
    (lib / "second.cpp").write_text(saved_second)

    # Removing a source only in the candidate must fail even if it still links.
    saved_top = top.read_text()
    top.write_text(saved_top + '''if(BUILDRAY_UNITY)
  set_property(TARGET other PROPERTY SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/replacement.cpp")
endif()
''')
    (source / "replacement.cpp").write_text("int main() { return 0; }\n")
    _, failed = invoke("changed-sources", expected=1)
    assert "source/target coverage" in failed["error"] and not failed["patch_validated"]
    top.write_text(saved_top)
    (source / "replacement.cpp").unlink()

    # Refuse unrelated unity targets, launcher bypasses, and ineffective grouping.
    saved_cmake = cmake.read_text()
    for name, addition, message in [
        ("outside-target", "if(BUILDRAY_UNITY)\nset_property(TARGET other PROPERTY UNITY_BUILD ON)\nendif()\n", "unity builds"),
        ("bypass-c", "set_property(TARGET sample PROPERTY C_COMPILER_LAUNCHER \"\")\n", "Compiler coverage"),
        ("no-groups", "set_source_files_properties(\"main file.cpp\" second.cpp c/a.c c/b.c PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)\n", "did not combine"),
    ]:
        # other is declared in the parent after lib, so put target overrides there.
        top.write_text(saved_top + addition if name == "outside-target" else saved_top)
        cmake.write_text(saved_cmake + addition if name != "outside-target" else saved_cmake)
        _, failed = invoke(name, expected=1)
        assert message in failed["error"], failed
    top.write_text(saved_top)
    cmake.write_text(saved_cmake)
    _, scanned = invoke("scanned-modules", "--unity-exclude", "lib/c/a.c", "--unity-exclude", "lib/c/b.c",
                        "--", "-DCMAKE_CXX_STANDARD=26", "-DCMAKE_CXX_SCAN_FOR_MODULES=ON", expected=1)
    assert "did not combine" in scanned["error"], scanned
    assert digest(source) == original

    for extra in [("--batch-size", "0"), ("--batch-size", "1"), ("--batch-size", "33"),
                  ("--unity-exclude", "../escape.cpp")]:
        run(binary, "unity", "--target", "sample", *extra, expected=2)
    run(binary, "unity", expected=2)
    run(binary, "measure", "--batch-size", 4, expected=2)
    run(binary, "unity", "--source", source, "--target", "bad;target", expected=1)
    run(binary, "unity", "--source", source, "--target", "sample", "--", "-DBUILDRAY_UNITY=ON", expected=1)

print("Unity builds: C/C++ coverage, independent patch, collisions, runtime and source preservation passed")
