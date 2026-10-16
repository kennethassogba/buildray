"""Real builds for compare/check; synthetic samples only exercise deterministic policy edges."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary, compiler = sys.argv[1:]


def run(*args, expected=0):
    result = subprocess.run([str(item) for item in args], text=True, capture_output=True)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result


def project(path, *, delay=False, fail=False):
    path.mkdir()
    (path / "main.cpp").write_text("int main() { return " + ("1" if fail else "0") + "; }\n")
    (path / "CMakeLists.txt").write_text(r"""cmake_minimum_required(VERSION 3.30)
project(comparison LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffile-prefix-map=\"${CMAKE_CURRENT_SOURCE_DIR}=.\" -fdebug-prefix-map=\"${CMAKE_CURRENT_BINARY_DIR}=.\"" CACHE STRING "fixture flags" FORCE)
add_executable(sample main.cpp)
enable_testing()
add_test(NAME sample COMMAND sample)
""" + ("add_custom_target(delay ALL COMMAND ${CMAKE_COMMAND} -E sleep 1)\n" if delay else ""))


with tempfile.TemporaryDirectory(prefix="buildray compare ") as temp:
    root = Path(temp)
    before = root / "base"
    after = root / "current"
    project(before)
    project(after, delay=True)
    work = root / "run"
    command = [binary, "compare", "--baseline-source", before, "--source", after,
               "--compiler", compiler, "--work-dir", work, "--runs", "3", "--jobs", "2"]
    run(*command, expected=3)
    report = json.loads((work / "report.json").read_text())
    assert report["status"] == "passed" and not report["gate_passed"], report
    assert report["validation"] == "build_and_tests_passed"
    comparison = json.loads((work / "comparison.json").read_text())
    assert comparison["performance_result"] == "regressed"
    assert (work / "comparison.md").read_text().startswith("### Buildray")
    for name in ["baseline-report.json", "current-report.json"]:
        item = json.loads((work / name).read_text())
        assert item["command"] == "measure" and item["status"] == "passed"
        assert len(item["baseline"]["samples"]) == 3
        assert item["measurement_context"]["sampling"] == "paired_alternating"
        flags = item["measurement_context"]["cache_flags"]["CMAKE_CXX_FLAGS"]
        assert "<SOURCE>" in flags and "<BUILD>" in flags, flags
    check = [binary, "check", "--baseline", work / "baseline-report.json",
             "--current", work / "current-report.json", "--output", root / "check.json"]
    run(*check, expected=3)
    assert json.loads((root / "check.json").read_text())["performance_result"] == "regressed"
    # Context changes never silently pass a regression gate.
    current = json.loads((work / "current-report.json").read_text())
    current["measurement_context"]["jobs"] += 1
    incompatible = root / "incompatible.json"
    incompatible.write_text(json.dumps(current))
    run(*check, "--current", incompatible, expected=4)
    refused = json.loads((root / "check.json").read_text())
    assert refused["status"] == "incomparable"
    assert "different or missing measurement_context" in refused["error"]
    # Refuse output aliases that would destroy the baseline input.
    original = (work / "baseline-report.json").read_bytes()
    run(*check, "--output", work / "baseline-report.json", expected=1)
    assert (work / "baseline-report.json").read_bytes() == original
    run(*check, "--output", root / "collision.md", expected=1)
    assert not (root / "collision.md").exists()
    alias = root / "alias.json"
    os.link(work / "baseline-report.json", alias)
    run(*check, "--output", alias, expected=1)
    assert (work / "baseline-report.json").read_bytes() == original
    run(binary, "check", "--baseline", work / "baseline-report.json", expected=2)
    run(binary, "check", "--baseline", work / "baseline-report.json", "--current", incompatible,
        "--max-regression-percent", "nan", expected=2)
    run(binary, "compare", "--source", before, "--runs", "1", expected=2)
    failed = root / "failed"
    project(failed, fail=True)
    failed_work = root / "failed-run"
    run(binary, "compare", "--baseline-source", before, "--source", failed,
        "--compiler", compiler, "--work-dir", failed_work, expected=1)
    assert json.loads((failed_work / "report.json").read_text())["phase"] == "candidate_validation"
    assert not (failed_work / "comparison.json").exists()

print("Paired source comparison, regression exit, offline replay and refusal checks passed")
