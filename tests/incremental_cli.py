"""Measure actual Ninja invalidation with Unity/PCH, including failure and restoration."""
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

binary, compiler = sys.argv[1:]


def run(*args, expected=0):
    result = subprocess.run([str(a) for a in args], check=False, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def digest(tree):
    return {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in tree.rglob("*") if p.is_file()}


with tempfile.TemporaryDirectory(prefix="buildray incremental ") as folder:
    root = Path(folder)
    source = root / "source tree"
    (source / "lib").mkdir(parents=True)
    top = source / "CMakeLists.txt"
    top.write_text('''cmake_minimum_required(VERSION 3.30)
project(incremental_fixture LANGUAGES C CXX)
add_library(core lib/a.cpp lib/b.cpp lib/helper.c)
target_include_directories(core PUBLIC lib)
add_executable(sample main.cpp)
target_link_libraries(sample PRIVATE core)
enable_testing()
add_test(NAME works COMMAND sample)
''')
    (source / "lib/public header.hpp").write_text("#pragma once\nint a(); int b();\n")
    (source / "lib/private.hpp").write_text('#pragma once\n#include "public header.hpp"\nconstexpr int answer = 0;\n')
    (source / "lib/a.cpp").write_text('#include "private.hpp"\nint a() { return answer; }\n')
    (source / "lib/b.cpp").write_text('#include "public header.hpp"\nint b() { return 0; }\n')
    (source / "lib/helper.c").write_text("int helper(void) { return 0; }\n")
    (source / "lib/unused.hpp").write_text("#pragma once\n")
    (source / "main.cpp").write_text('#include "public header.hpp"\nint main() { return a() + b(); }\n')
    original = digest(source)
    paths = ["lib/a.cpp", "lib/private.hpp", "lib/public header.hpp", "lib/helper.c"]

    def invoke(name, mode="unity", files=paths, runs=3, only=True, expected=0):
        work = root / name
        args = [binary, mode, "--source", source, "--target", "core", "--compiler", compiler,
                "--work-dir", work, "--runs", runs, "--jobs", 2]
        if mode == "pch":
            args += ["--header", "lib/private.hpp"]
        if only:
            args += ["--incremental-only"]
        for path in files:
            args += ["--rebuild-file", path]
        run(*args, expected=expected)
        return work, json.loads((work / "report.json").read_text())

    for mode, counts in [("unity", [1, 1, 2, 1]), ("pch", [1, 3, 4, 1])]:
        work, report = invoke(mode, mode)
        assert report["patch_validated"] and not report["patch_recommended"]
        assert report["performance_result"] == "not_measured" and "baseline" not in report
        inc = report["incremental"]
        assert inc == json.loads((work / "incremental.json").read_text())
        assert inc["status"] == "passed" and len(inc["scenarios"]) == 4
        for index, scenario in enumerate(inc["scenarios"]):
            assert scenario["file"] == paths[index] and scenario["input_restored"]
            for variant, expected_count in [("baseline", [1, 1, 3, 1][index]), ("candidate", counts[index])]:
                samples = scenario[variant]["samples"]
                assert len(samples) == 3
                assert all(s["compiler_invocations"] == expected_count for s in samples), scenario
                assert all(s["tests_passed"] == 1 and s["max_compiler_rss_kib"] > 0 for s in samples)
                original_count = ([1, 1, 3, 1] if variant == "baseline" else
                                  [2, 2, 3, 1] if mode == "unity" else [1, 2, 3, 1])[index]
                assert all(s["original_source_count"] == len(s["original_source_files"]) == original_count for s in samples)
                assert all((work / (s["log_prefix"] + "-compilers.json")).is_file() for s in samples)
            if mode == "pch" and index in [1, 2]:
                assert all(s["pch_creations"] == 1 for s in scenario["candidate"]["samples"])
            for variant in ["baseline", "candidate"]:
                assert (work / (variant + "-source") / paths[index]).read_bytes() == (source / paths[index]).read_bytes()
        assert digest(source) == original

    # Additive mode retains clean samples; one incremental pair cannot establish a gain.
    _, report = invoke("additive", files=[paths[0]], runs=1, only=False)
    assert report["baseline"]["samples"] and report["candidate"]["samples"]
    assert report["incremental"]["scenarios"][0]["performance_result"] == "insufficient_samples"
    assert not report["patch_recommended"]

    # A header outside the build must not produce a misleading fast sample.
    work, failed = invoke("unused", files=["lib/unused.hpp"], runs=1, expected=1)
    assert "No compiler measurements" in failed["error"] and not failed["patch_recommended"]
    assert failed["incremental"]["status"] == "failed"
    assert (work / "candidate-source/lib/unused.hpp").read_bytes() == (source / "lib/unused.hpp").read_bytes()
    assert (work / "baseline-source/lib/unused.hpp").read_bytes() == (source / "lib/unused.hpp").read_bytes()

    # Source restoration and partial evidence survive a runtime failure after a probe.
    saved = top.read_text()
    top.write_text(saved + f'''add_test(NAME reject_probe COMMAND "{sys.executable}" -c
"import pathlib,sys; sys.exit('Buildray incremental probe' in pathlib.Path(r'${{CMAKE_CURRENT_SOURCE_DIR}}/lib/a.cpp').read_text())")
''')
    work, failed = invoke("runtime", files=[paths[0]], runs=1, expected=1)
    assert "ctest exited" in failed["error"] and failed["incremental"]["status"] == "failed"
    assert not failed["patch_recommended"]
    assert (work / "baseline-source/lib/a.cpp").read_bytes() == (source / "lib/a.cpp").read_bytes()
    assert list((work / "logs").glob("incremental-*-compilers.json"))
    top.write_text(saved + '\nadd_custom_target(always ALL COMMAND "${CMAKE_COMMAND}" -E echo dirty)\n')
    _, failed = invoke("dirty", files=[paths[0]], runs=1, expected=1)
    assert "not up to date" in failed["error"]
    top.write_text(saved)
    assert digest(source) == original

    for files in [["../escape.cpp"], ["CMakeLists.txt"], ["lib/missing.hpp"], [paths[0], paths[0]]]:
        expected = 2 if files[0].startswith("..") else 1
        args = [binary, "unity", "--source", source, "--target", "core"]
        for path in files:
            args += ["--rebuild-file", path]
        run(*args, expected=expected)
    run(binary, "unity", "--target", "core", "--incremental-only", expected=2)
    run(binary, "measure", "--rebuild-file", "lib/a.cpp", expected=2)

print("Incremental rebuilds: Unity/PCH/C coverage, actual invalidation, no-op guards, runtime failure and restoration passed")
