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
    ]:
        execute([sys.executable, script], env={**environment, **changes}, expected=1)
    assert not (source / "injected").exists()

    # Bootstrap failures still retain a useful artifact instead of an empty upload.
    failed_outputs = root / "failed-outputs"
    execute([sys.executable, script], env={**environment, "BUILDRAY_BINARY": str(root / "missing"),
                                         "GITHUB_OUTPUT": str(failed_outputs)}, expected=1)
    failed = dict(line.split("=", 1) for line in failed_outputs.read_text().splitlines())
    failure = json.loads((Path(failed["artifact-path"]) / "action-status.json").read_text())
    assert failed["result"] == failure["status"] == "failed"
    assert "does not exist" in failure["error"]

print("Action entry point: Git archive, paired builds, failure outputs, summary and source preservation passed")
