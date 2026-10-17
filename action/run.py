"""Composite-action entry point. All project inputs are argv or JSON, never shell code."""

from __future__ import annotations

from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import uuid


def execute(args, *, cwd=None, capture=False):
    return subprocess.run([str(item) for item in args], cwd=cwd, text=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None, check=True)


def integer(key, default, maximum):
    value = os.environ.get(key, default)
    if not re.fullmatch(r"[0-9]+", value) or not 1 <= int(value) <= maximum:
        raise ValueError(f"{key} must be between 1 and {maximum}")
    return int(value)


def boolean(key, default):
    value = os.environ.get(key, default)
    if value not in ("true", "false"):
        raise ValueError(f"{key} must be true or false")
    return value == "true"


def output(key, value):
    if "\n" in str(value) or "\r" in str(value):
        raise ValueError("Action outputs must be single-line values")
    destination = os.environ.get("GITHUB_OUTPUT")
    if destination:
        with Path(destination).open("a") as stream:
            stream.write(f"{key}={value}\n")


def base_tree(source, reference, work):
    if reference.startswith("-") or any(c.isspace() or ord(c) < 32 for c in reference):
        raise ValueError("Invalid baseline Git revision")
    repository = Path(execute(["git", "rev-parse", "--show-toplevel"], cwd=source, capture=True).stdout.strip()).resolve()
    relative = source.relative_to(repository)
    command = ["git", "rev-parse", "--verify", "--end-of-options", reference + "^{commit}"]
    try:
        revision = execute(command, cwd=repository, capture=True).stdout.strip()
    except subprocess.CalledProcessError:
        execute(["git", "fetch", "--no-tags", "--depth=1", "origin", reference], cwd=repository)
        revision = execute(["git", "rev-parse", "--verify", "FETCH_HEAD^{commit}"], cwd=repository, capture=True).stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{40,64}", revision):
        raise ValueError("Cannot resolve baseline to a commit")
    archive = work / "baseline.tar"
    with archive.open("wb") as stream:
        subprocess.run(["git", "archive", "--format=tar", revision, "--", str(relative)],
                       cwd=repository, stdout=stream, check=True)
    tree = work / "base"
    tree.mkdir()
    with tarfile.open(archive) as contents:
        contents.extractall(tree, filter="data")
    archive.unlink()
    output("baseline-sha", revision)
    return tree / relative


def bootstrap(action, work, compiler, binary_input, jobs):
    if binary_input:
        path = Path(binary_input)
        if not path.is_absolute():
            path = Path(os.environ["GITHUB_WORKSPACE"]) / path
        if not path.is_file():
            raise ValueError(f"Buildray binary does not exist: {path}")
        return path
    directory = work / "tool"
    args = ["cmake", "-S", action, "-B", directory, "-G", "Ninja", "-DBUILD_TESTING=OFF",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=" + compiler]
    config = shutil.which("llvm-config-21") or shutil.which("llvm-config")
    if config:
        args.append("-DLLVM_DIR=" + execute([config, "--cmakedir"], capture=True).stdout.strip())
    execute(args)
    execute(["cmake", "--build", directory, "--parallel", str(jobs)])
    return directory / "buildray"


@contextmanager
def evidence(artifacts):
    status = artifacts / "action-status.json"
    state = {"status": "running"}
    status.write_text(json.dumps(state) + "\n")
    try:
        yield state
    except Exception as error:
        state.update(status="failed", error=str(error))
        status.write_text(json.dumps(state) + "\n")
        output("result", "failed")
        raise
    else:
        status.write_text(json.dumps(state) + "\n")


def retain_failure_logs(run, artifacts):
    logs = [p for p in (run / "logs").glob("*.log") if p.is_file() and not p.is_symlink()]
    logs.sort(key=lambda p: p.stat().st_mtime_ns)
    for path in logs[-3:]:
        destination = artifacts / "failure-logs" / path.name
        destination.parent.mkdir(exist_ok=True)
        with path.open("rb") as stream:
            stream.seek(max(0, path.stat().st_size - 65536))
            tail = stream.read(65536)
        destination.write_bytes(tail)
        if path == logs[-1]:
            print(f"\nBuildray failure log: {path.name}\n" + tail[-16384:].decode(errors="replace"),
                  file=sys.stderr, flush=True)


def main():
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    action = Path(os.environ.get("GITHUB_ACTION_PATH", Path(__file__).parent.parent)).resolve()
    source = (workspace / os.environ.get("BUILDRAY_SOURCE", ".")).resolve()
    if not source.is_relative_to(workspace) or not (source / "CMakeLists.txt").is_file():
        raise ValueError("Source must be a CMake tree inside GITHUB_WORKSPACE")
    mode = os.environ.get("BUILDRAY_COMMAND", "measure")
    if mode not in ("measure", "optimize", "profile", "pch", "deps", "prune-links"):
        raise ValueError("command must be measure, optimize, profile, pch, deps or prune-links")
    fail_on_cycles = boolean("BUILDRAY_FAIL_ON_CYCLES", "false")
    if fail_on_cycles and mode != "deps":
        raise ValueError("fail-on-cycles requires command deps")
    target = os.environ.get("BUILDRAY_PCH_TARGET", "")
    headers = json.loads(os.environ.get("BUILDRAY_PCH_HEADERS", "[]"))
    auto_pch = boolean("BUILDRAY_PCH_AUTO", "false")
    if not isinstance(headers, list) or not all(isinstance(item, str) and item for item in headers):
        raise ValueError("pch-headers must be a JSON array of nonempty strings")
    if auto_pch and (mode != "pch" or headers):
        raise ValueError("pch-auto requires command pch and empty pch-headers")
    if mode == "pch" and not auto_pch and (not target or not headers):
        raise ValueError("pch needs pch-target and pch-headers")
    if mode != "pch" and (target or headers):
        raise ValueError("pch-target and pch-headers require command pch")
    runs = integer("BUILDRAY_RUNS", "3", 100)
    if auto_pch and runs < 3:
        raise ValueError("pch-auto requires at least three runs")
    link_target = os.environ.get("BUILDRAY_LINK_TARGET", "")
    link_dependency = os.environ.get("BUILDRAY_LINK_DEPENDENCY", "")
    if mode != "prune-links" and (link_target or link_dependency):
        raise ValueError("link-target and link-dependency require command prune-links")
    if mode == "prune-links" and runs < 3:
        raise ValueError("prune-links requires at least three runs")
    jobs = integer("BUILDRAY_JOBS", "2", 1024)
    integer("BUILDRAY_RETENTION_DAYS", "30", 90)
    boolean("BUILDRAY_INSTALL_DEPENDENCIES", "true")
    allow_no_tests = boolean("BUILDRAY_ALLOW_NO_TESTS", "false")
    budget = float(os.environ.get("BUILDRAY_MAX_REGRESSION_PERCENT", "5"))
    if not math.isfinite(budget) or not 0 <= budget <= 1000:
        raise ValueError("max-regression-percent must be between 0 and 1000")
    cmake_args = json.loads(os.environ.get("BUILDRAY_CMAKE_ARGS", "[]"))
    if not isinstance(cmake_args, list) or not all(isinstance(item, str) and item.startswith("-D") and "=" in item for item in cmake_args):
        raise ValueError("cmake-args must be a JSON array of -DNAME=VALUE strings")
    exclusions = json.loads(os.environ.get("BUILDRAY_EXCLUDE_PATHS", "[]"))
    if not isinstance(exclusions, list) or not all(isinstance(item, str) and item for item in exclusions):
        raise ValueError("exclude-paths must be a JSON array of nonempty relative paths")
    if exclusions and mode not in ("optimize", "prune-links"):
        raise ValueError("exclude-paths requires command optimize or prune-links")
    reference = os.environ.get("BUILDRAY_BASELINE_REF", "").strip()
    if reference and mode == "measure" and runs < 3:
        raise ValueError("Revision comparisons need at least three runs")
    compiler = os.environ.get("BUILDRAY_COMPILER", "clang++-21")
    work = Path(tempfile.mkdtemp(prefix="buildray-action-", dir=os.environ["RUNNER_TEMP"]))
    output("artifact-path", work / "artifacts")
    artifacts = work / "artifacts"
    artifacts.mkdir()
    with evidence(artifacts) as action_status:
        binary = bootstrap(action, work, compiler, os.environ.get("BUILDRAY_BINARY", ""), jobs)
        command = [binary, mode, "--source", source, "--work-dir", work / "run",
                   "--compiler", compiler, "--runs", str(runs), "--jobs", str(jobs),
                   "--session", str(uuid.uuid4())]
        if reference and mode == "measure":
            command[1] = "compare"
            command.extend(["--baseline-source", base_tree(source, reference, work),
                            "--max-regression-percent", str(budget)])
        if mode == "optimize":
            command.extend(["--clang-tidy", os.environ.get("BUILDRAY_CLANG_TIDY", "clang-tidy-21")])
        for path in exclusions:
            command.extend(["--exclude", path])
        if mode == "prune-links":
            if link_target:
                command.extend(["--target", link_target])
            if link_dependency:
                command.extend(["--dependency", link_dependency])
        if mode == "deps":
            command.extend(["--c-compiler", os.environ.get("BUILDRAY_C_COMPILER", "clang-21")])
            if fail_on_cycles:
                command.append("--fail-on-cycles")
        if mode == "pch":
            if auto_pch:
                command.append("--auto")
            if target:
                command.extend(["--target", target])
            for header in headers:
                command.extend(["--header", header])
        if allow_no_tests:
            command.append("--allow-no-tests")
        command.extend(["--", *cmake_args])
        result = subprocess.run([str(item) for item in command], check=False)
        report_file = work / "run/report.json"
        report = json.loads(report_file.read_text()) if report_file.is_file() else {"status": "failed"}
        action_status.update(status="complete" if report["status"] == "passed" else "failed",
                             exit_code=result.returncode)
        if report["status"] != "passed":
            retain_failure_logs(work / "run", artifacts)
        output("result", ("cycles" if result.returncode == 5 else report.get("performance_result", "passed"))
               if report["status"] == "passed" else report["status"])
        for name, key in [("report.json", "report"), ("comparison.json", "comparison"),
                          ("baseline-report.json", "baseline-report"), ("current-report.json", "current-report"),
                          ("includes.patch", "patch"), ("pch.patch", "patch"), ("cmake.patch", "patch"),
                          ("headers.json", "headers"), ("dependencies.json", "dependencies"),
                          ("dependencies.dot", "dependency-dot"), ("selection.json", "selection"),
                          ("links.json", "links"), ("cmake-before.json", "cmake-before"),
                          ("cmake-after.json", "cmake-after")]:
            item = work / "run" / name
            if item.is_file():
                shutil.copyfile(item, artifacts / name)
                output(key, artifacts / name)
        summary = work / ("run/headers.md" if mode == "profile" else
                          "run/dependencies.md" if mode == "deps" else "run/comparison.md")
        if summary.is_file():
            text = summary.read_text()
            shutil.copyfile(summary, artifacts / summary.name)
        else:
            text = "### Buildray\n\nRun status: " + report["status"] + ".\n"
            if report.get("baseline"):
                text += f"\nClean-build median: {report['baseline']['median_seconds']:.3f} s.\n"
            if mode == "optimize":
                text += f"\nRemoved includes: {report.get('removed_includes', 0)}. Patch validated: {report.get('patch_validated', False)}.\n"
            if mode == "pch" and report.get("candidate"):
                text += f"\nPCH median: {report['candidate']['median_seconds']:.3f} s. Change: {report['wall_change_percent']:+.3f}% ({report['performance_result']}).\n"
                text += f"\nPatch validated: {report.get('patch_validated', False)}. Recommended: {report.get('patch_recommended', False)}.\n"
            if mode == "prune-links":
                selected = report.get("selected_link")
                if selected:
                    text += f"\nCMake candidate: `{selected['target']}` → `{selected['dependency']}`.\n"
                else:
                    text += "\nNo eligible redundant literal link; no patch or benchmark.\n"
                if report.get("candidate"):
                    text += f"\nCandidate median: {report['candidate']['median_seconds']:.3f} s. Change: {report['wall_change_percent']:+.3f}% ({report['performance_result']}).\n"
                text += f"\nPatch validated: {report.get('patch_validated', False)}. Recommended: {report.get('patch_recommended', False)}.\n"
            if report.get("automatic_selection"):
                selected = report["automatic_selection"]
                text += f"\nAutomatic candidate: `{selected['header']}` on `{selected['target']}`. Selection profiling is outside the measurements.\n"
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as stream:
                stream.write(text)
        return result.returncode

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError, tarfile.TarError) as error:
        print(f"buildray action: {error}", file=sys.stderr)
        sys.exit(1)
