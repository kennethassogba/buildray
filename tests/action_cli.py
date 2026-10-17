"""Exercise the action entry point with the real C++ CLI and local Git revisions."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary, compiler, script = [str(Path(item).absolute()) for item in sys.argv[1:]]


def execute(args, *, cwd=None, env=None, expected=0):
    result = subprocess.run([str(item) for item in args], cwd=cwd, env=env, text=True, capture_output=True)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout.strip()


with tempfile.TemporaryDirectory(prefix="buildray action ") as temporary:
    root = Path(temporary)
    source = root / "checkout"
    source.mkdir()
    (source / "main.cpp").write_text("int main() { return 0; }\n")
    cmake = source / "CMakeLists.txt"
    cmake.write_text("""cmake_minimum_required(VERSION 3.30)
project(action_fixture LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
add_executable(sample main.cpp)
enable_testing()
add_test(NAME works COMMAND sample)
""")
    execute(["git", "init", "-q"], cwd=source)
    execute(["git", "-c", "user.name=Buildray test", "-c", "user.email=test@example.invalid", "add", "."], cwd=source)
    execute(["git", "-c", "user.name=Buildray test", "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture"], cwd=source)
    revision = execute(["git", "rev-parse", "HEAD"], cwd=source)
    with cmake.open("a") as stream:
        stream.write("add_custom_target(delay ALL COMMAND ${CMAKE_COMMAND} -E sleep 1)\n")
    status = execute(["git", "status", "--porcelain"], cwd=source)
    outputs = root / "outputs"
    summary = root / "summary"
    environment = {**os.environ, "GITHUB_WORKSPACE": str(source), "RUNNER_TEMP": str(root),
                   "GITHUB_OUTPUT": str(outputs), "GITHUB_STEP_SUMMARY": str(summary),
                   "BUILDRAY_BINARY": binary, "BUILDRAY_COMPILER": compiler,
                   "BUILDRAY_BASELINE_REF": revision, "BUILDRAY_INSTALL_DEPENDENCIES": "false"}
    execute([sys.executable, script], env=environment, expected=3)
    fields = dict(line.split("=", 1) for line in outputs.read_text().splitlines())
    assert fields["result"] == "regressed", fields
    assert Path(fields["comparison"]).is_file()
    assert Path(fields["baseline-report"]).is_file() and Path(fields["current-report"]).is_file()
    assert "regressed" in summary.read_text()
    assert json.loads((Path(fields["artifact-path"]) / "action-status.json").read_text())["status"] == "complete"
    assert execute(["git", "status", "--porcelain"], cwd=source) == status, "action changed the checkout"
    # Paths with shell syntax and invalid refs/JSON are rejected, not evaluated.
    for changes in [
        {"BUILDRAY_CMAKE_ARGS": "-DFOO=$(touch injected)"},
        {"BUILDRAY_BASELINE_REF": "--upload-pack=touch injected"},
        {"BUILDRAY_RUNS": "0"},
        {"BUILDRAY_SOURCE": "../"},
        {"BUILDRAY_ALLOW_NO_TESTS": "yes"},
        {"BUILDRAY_COMMAND": "pch", "BUILDRAY_PCH_HEADERS": "<vector>"},
        {"BUILDRAY_COMMAND": "profile", "BUILDRAY_PCH_TARGET": "sample"},
        {"BUILDRAY_FAIL_ON_CYCLES": "true"},
        {"BUILDRAY_PCH_AUTO": "true"},
        {"BUILDRAY_EXCLUDE_PATHS": '["3rdparty"]'},
        {"BUILDRAY_COMMAND": "optimize", "BUILDRAY_EXCLUDE_PATHS": '"3rdparty"'},
        {"BUILDRAY_COMMAND": "pch", "BUILDRAY_PCH_AUTO": "true", "BUILDRAY_PCH_HEADERS": '["<vector>"]'},
        {"BUILDRAY_LINK_TARGET": "sample"},
        {"BUILDRAY_COMMAND": "prune-links", "BUILDRAY_RUNS": "1"},
        {"BUILDRAY_FORWARD_DECLARATIONS": "true"},
        {"BUILDRAY_COMMAND": "headers", "BUILDRAY_HEADER_PATHS": '"include"'},
        {"BUILDRAY_COMMAND": "headers", "BUILDRAY_MAX_HEADER_EDITS": "0"},
        {"BUILDRAY_COMMAND": "unity"},
        {"BUILDRAY_UNITY_TARGET": "sample"},
        {"BUILDRAY_COMMAND": "unity", "BUILDRAY_UNITY_TARGET": "sample", "BUILDRAY_UNITY_BATCH_SIZE": "1"},
        {"BUILDRAY_COMMAND": "unity", "BUILDRAY_UNITY_TARGET": "sample", "BUILDRAY_UNITY_EXCLUDE": '"main.cpp"'},
    ]:
        execute([sys.executable, script], env={**environment, **changes}, expected=1)
    assert not (source / "injected").exists()

    for mode in ["profile", "pch", "deps"]:
        output_file = root / (mode + "-outputs")
        extra = {"BUILDRAY_PCH_TARGET": "sample", "BUILDRAY_PCH_HEADERS": '["<vector>"]'} if mode == "pch" else {}
        execute([sys.executable, script], env={**environment, **extra,
                "BUILDRAY_COMMAND": mode, "BUILDRAY_RUNS": "1", "GITHUB_OUTPUT": str(output_file)})
        result_fields = dict(line.split("=", 1) for line in output_file.read_text().splitlines())
        assert "baseline-sha" not in result_fields, "PR baseline applies only to measure"
        key = "headers" if mode == "profile" else "dependencies" if mode == "deps" else "patch"
        assert Path(result_fields[key]).is_file()
        if mode == "deps":
            assert Path(result_fields["dependency-dot"]).is_file()
        state = json.loads(Path(result_fields["report"]).read_text())
        assert state["status"] == "passed"
        assert execute(["git", "status", "--porcelain"], cwd=source) == status

    # Automatic PCH retains both its selection profile and measured experiment.
    original_cmake = cmake.read_text()
    (source / "common.hpp").write_text('#pragma once\n#include <vector>\ninline int answer() { return std::vector<int>{}.size(); }\n')
    (source / "helper.cpp").write_text('#include "common.hpp"\nint helper() { return answer(); }\n')
    (source / "main.cpp").write_text('#include "common.hpp"\nint helper(); int main() { return answer() + helper(); }\n')
    cmake.write_text(original_cmake + '\ntarget_sources(sample PRIVATE helper.cpp)\n')
    automatic_outputs = root / "automatic-outputs"
    execute([sys.executable, script], env={**environment, "BUILDRAY_COMMAND": "pch",
            "BUILDRAY_PCH_AUTO": "true", "GITHUB_OUTPUT": str(automatic_outputs)})
    automatic = dict(line.split("=", 1) for line in automatic_outputs.read_text().splitlines())
    auto_report = json.loads(Path(automatic["report"]).read_text())
    assert auto_report["pch_auto"] and auto_report["patch_validated"]
    assert auto_report["automatic_selection"]["header"] == "common.hpp"
    assert all(Path(automatic[key]).is_file() for key in ["headers", "selection", "patch"])
    assert "Automatic candidate:" in summary.read_text()
    unity_outputs = root / "unity-outputs"
    execute([sys.executable, script], env={**environment, "BUILDRAY_COMMAND": "unity",
            "BUILDRAY_UNITY_TARGET": "sample", "BUILDRAY_UNITY_BATCH_SIZE": "2",
            "BUILDRAY_CMAKE_ARGS": '["-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"]',
            "BUILDRAY_RUNS": "1", "GITHUB_OUTPUT": str(unity_outputs)})
    unity = dict(line.split("=", 1) for line in unity_outputs.read_text().splitlines())
    assert "baseline-sha" not in unity and all(Path(unity[key]).is_file() for key in ["unity", "patch", "report"])
    unity_report = json.loads(Path(unity["report"]).read_text())
    assert unity_report["patch_validated"] and unity_report["performance_result"] == "insufficient_samples"
    assert "Unity target:" in summary.read_text()
    cmake.write_text(original_cmake)
    (source / "main.cpp").write_text("int main() { return 0; }\n")
    (source / "common.hpp").unlink()
    (source / "helper.cpp").unlink()
    assert execute(["git", "status", "--porcelain"], cwd=source) == status

    # CMake experiments retain the plan, graphs and patch without using the PR baseline.
    cmake.write_text(original_cmake.replace('add_custom_target(delay ALL COMMAND ${CMAKE_COMMAND} -E sleep 1)', '') + '''
add_library(api INTERFACE)
add_library(bridge INTERFACE)
target_link_libraries(bridge INTERFACE api)
target_link_libraries(sample PRIVATE bridge api)
''')
    link_outputs = root / "link-outputs"
    execute([sys.executable, script], env={**environment, "BUILDRAY_COMMAND": "prune-links",
            "BUILDRAY_LINK_TARGET": "sample", "BUILDRAY_LINK_DEPENDENCY": "api",
            "BUILDRAY_EXCLUDE_PATHS": '["vendor"]', "GITHUB_OUTPUT": str(link_outputs)})
    links = dict(line.split("=", 1) for line in link_outputs.read_text().splitlines())
    assert "baseline-sha" not in links
    assert all(Path(links[key]).is_file() for key in ["links", "patch", "cmake-before", "cmake-after"])
    state = json.loads(Path(links["report"]).read_text())
    assert state["patch_validated"] and state["cmake_graph_change_verified"]
    assert state["selected_link"]["dependency"] == "api"
    assert "CMake candidate:" in summary.read_text()
    cmake.write_text(original_cmake)
    assert execute(["git", "status", "--porcelain"], cwd=source) == status

    # Header mode retains the exact declarations and patch, independently of PR history.
    (source / "record.hpp").write_text('#pragma once\nnamespace fixture { class Record {}; }\n')
    (source / "declaration.hpp").write_text('#pragma once\n#include <vector>\n#include "record.hpp"\nnamespace fixture { void use(Record*); }\n')
    (source / "main.cpp").write_text('#include "declaration.hpp"\nint main() { return 0; }\n')
    cmake.write_text(original_cmake.replace('add_custom_target(delay ALL COMMAND ${CMAKE_COMMAND} -E sleep 1)', ''))
    header_outputs = root / "header-outputs"
    execute([sys.executable, script], env={**environment, "BUILDRAY_COMMAND": "headers",
            "BUILDRAY_FORWARD_DECLARATIONS": "true", "BUILDRAY_HEADER_PATHS": '["declaration.hpp"]',
            "BUILDRAY_MAX_HEADER_EDITS": "2", "BUILDRAY_RUNS": "1", "GITHUB_OUTPUT": str(header_outputs)})
    fields = dict(line.split("=", 1) for line in header_outputs.read_text().splitlines())
    assert "baseline-sha" not in fields and all(Path(fields[key]).is_file() for key in ["patch", "header-cleanup", "report"])
    state = json.loads(Path(fields["report"]).read_text())
    assert state["patch_validated"] and state["forwarded_includes"] == 1
    assert state["header_unused_includes"] == 1
    (source / "record.hpp").unlink()
    (source / "declaration.hpp").unlink()
    (source / "main.cpp").write_text("int main() { return 0; }\n")
    cmake.write_text(original_cmake)
    assert execute(["git", "status", "--porcelain"], cwd=source) == status

    # The explicit cycle policy fails CI while retaining the completed graph.
    (source / "cycle.h").write_text('#pragma once\n#include "cycle.h"\n')
    (source / "main.cpp").write_text('#include "cycle.h"\nint main() { return 0; }\n')
    cycle_outputs = root / "cycle-outputs"
    execute([sys.executable, script], env={**environment, "BUILDRAY_COMMAND": "deps",
            "BUILDRAY_FAIL_ON_CYCLES": "true", "GITHUB_OUTPUT": str(cycle_outputs)}, expected=5)
    cycle_fields = dict(line.split("=", 1) for line in cycle_outputs.read_text().splitlines())
    assert cycle_fields["result"] == "cycles" and Path(cycle_fields["dependencies"]).is_file()
    assert json.loads(Path(cycle_fields["report"]).read_text())["cycle_gate_passed"] is False
    (source / "cycle.h").unlink()

    # Bootstrap failures still retain a useful artifact instead of an empty upload.
    failed_outputs = root / "failed-outputs"
    execute([sys.executable, script], env={**environment, "BUILDRAY_BINARY": str(root / "missing"),
                                         "GITHUB_OUTPUT": str(failed_outputs)}, expected=1)
    failed = dict(line.split("=", 1) for line in failed_outputs.read_text().splitlines())
    failure = json.loads((Path(failed["artifact-path"]) / "action-status.json").read_text())
    assert failed["result"] == failure["status"] == "failed"
    assert "does not exist" in failure["error"]

    # A real project test failure retains diagnostics even on an ephemeral runner.
    (source / "main.cpp").write_text("int main() { return 1; }\n")
    native_outputs = root / "native-failure-outputs"
    execute([sys.executable, script], env={**environment, "GITHUB_OUTPUT": str(native_outputs)}, expected=1)
    native = dict(line.split("=", 1) for line in native_outputs.read_text().splitlines())
    native_artifacts = Path(native["artifact-path"])
    state = json.loads((native_artifacts / "action-status.json").read_text())
    assert native["result"] == state["status"] == "failed" and state["exit_code"] == 1
    logs = list((native_artifacts / "failure-logs").glob("*.log"))
    assert 1 <= len(logs) <= 3 and all(p.stat().st_size <= 65536 for p in logs)
    assert any("FAILED" in p.read_text() for p in logs)

print("Action entry point: Git archive, paired builds, failure outputs, summary and source preservation passed")
