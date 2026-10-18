"""Composite-action entry point. All project inputs are argv or JSON, never shell code."""

from __future__ import annotations

from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import uuid


def execute(args, *, cwd=None, capture=False):
    return subprocess.run([str(item) for item in args], cwd=cwd, text=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None, check=True)


def integer(key, default, maximum, minimum=1):
    value = os.environ.get(key, default)
    if not re.fullmatch(r"[0-9]+", value) or not minimum <= int(value) <= maximum:
        raise ValueError(f"{key} must be between {minimum} and {maximum}")
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
    except BaseException as error:
        interrupted = isinstance(error, KeyboardInterrupt)
        state.update(status="interrupted" if interrupted else "failed",
                     error=str(error) or type(error).__name__)
        if interrupted:
            state["exit_code"] = 130
        status.write_text(json.dumps(state) + "\n")
        output("result", state["status"])
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


def retain_run_evidence(run, artifacts, report, emit_outputs=True):
    if report.get('parallelism_sweep'):
        for level in report.get('levels', []):
            jobs = level.get('jobs')
            if type(jobs) is not int or not 1 <= jobs <= 1024:
                continue
            child = run / f'jobs-{jobs}'
            path = child / 'report.json'
            if not path.is_file() or path.is_symlink():
                continue
            destination = artifacts / child.name
            destination.mkdir(exist_ok=True)
            child_report = json.loads(path.read_text())
            retain_run_evidence(child, destination, child_report, emit_outputs=False)
            for item in (child / 'logs').glob('*'):
                if item.is_file() and not item.is_symlink() and item.suffix in ('.json', '.xml', '.log'):
                    target = destination / 'logs' / item.name
                    target.parent.mkdir(exist_ok=True)
                    shutil.copyfile(item, target)
    for name, key in [("report.json", "report"), ("comparison.json", "comparison"),
                      ("parallelism.json", "parallelism"), ("parallelism.md", "parallelism-summary"),
                      ("baseline-report.json", "baseline-report"), ("current-report.json", "current-report"),
                      ("includes.patch", "patch"), ("pch.patch", "patch"), ("cmake.patch", "patch"), ("headers.patch", "patch"),
                      ("unity.patch", "patch"), ("unity.json", "unity"),
                      ("incremental.json", "incremental"),
                      ("incremental.md", "incremental-summary"),
                      ("headers.json", "headers"), ("dependencies.json", "dependencies"),
                      ("dependencies.dot", "dependency-dot"), ("selection.json", "selection"),
                      ("links.json", "links"), ("archive-profile.json", "archive-profile"), ("cmake-before.json", "cmake-before"),
                      ("cmake-after.json", "cmake-after"), ("header-cleanup.json", "header-cleanup")]:
        item = run / name
        if item.is_file() and not item.is_symlink():
            shutil.copyfile(item, artifacts / name)
            if emit_outputs:
                output(key, artifacts / name)
    for item in run.glob("unity-attempt-*.json"):
        if item.is_file() and not item.is_symlink():
            shutil.copyfile(item, artifacts / item.name)
    for item in (run / "logs").glob("incremental-*"):
        if item.is_file() and not item.is_symlink():
            destination = artifacts / "logs" / item.name
            destination.parent.mkdir(exist_ok=True)
            shutil.copyfile(item, destination)
    if report.get('command') in ('prune-links', 'prune-deps'):
        for item in (run / 'logs').glob('*.json'):
            if item.is_file() and not item.is_symlink():
                destination = artifacts / 'logs' / item.name
                destination.parent.mkdir(exist_ok=True)
                shutil.copyfile(item, destination)
        for item in [*(run / 'dependency-trials').rglob('*'), *(run / 'dependency-profiles').rglob('*')]:
            if item.is_file() and not item.is_symlink() and item.suffix in ('.log', '.json', '.tsv', '.patch'):
                destination = artifacts / item.relative_to(run)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(item, destination)
    for attempt in report.get("unity_attempts", []):
        # Only our fixed attempt labels are accepted as artifact paths.
        index = attempt.get("attempt")
        if type(index) is not int or not 0 <= index <= 8:
            continue
        label = "candidate-validation" if index == 0 else f"candidate-retry-{index}"
        item = run / "logs" / (label + "-build.log")
        if item.is_file() and not item.is_symlink():
            destination = artifacts / "logs" / item.name
            destination.parent.mkdir(exist_ok=True)
            with item.open("rb") as stream:
                stream.seek(max(0, item.stat().st_size - 65536))
                destination.write_bytes(stream.read(65536))


def run_cli(command, run, artifacts):
    process = subprocess.Popen([str(item) for item in command])
    try:
        return subprocess.CompletedProcess(command, process.wait())
    except KeyboardInterrupt:
        # Forward a parent-only interruption, then let the CLI flush its report.
        try:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        finally:
            report_file = run / "report.json"
            try:
                report = json.loads(report_file.read_text()) if report_file.is_file() else {}
            except (OSError, ValueError):
                report = {}
            retain_run_evidence(run, artifacts, report)
            retain_failure_logs(run, artifacts)
        raise


def main():
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    action = Path(os.environ.get("GITHUB_ACTION_PATH", Path(__file__).parent.parent)).resolve()
    source = (workspace / os.environ.get("BUILDRAY_SOURCE", ".")).resolve()
    if not source.is_relative_to(workspace) or not (source / "CMakeLists.txt").is_file():
        raise ValueError("Source must be a CMake tree inside GITHUB_WORKSPACE")
    mode = os.environ.get("BUILDRAY_COMMAND", "measure")
    if mode not in ("measure", "optimize", "headers", "profile", "pch", "deps", "prune-links", "prune-deps", "unity"):
        raise ValueError("command must be measure, optimize, headers, profile, pch, deps, prune-links, prune-deps or unity")
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
    rebuild_files = json.loads(os.environ.get("BUILDRAY_REBUILD_FILES", "[]"))
    incremental_only = boolean("BUILDRAY_INCREMENTAL_ONLY", "false")
    if not isinstance(rebuild_files, list) or not all(isinstance(item, str) and item for item in rebuild_files):
        raise ValueError("rebuild-files must be a JSON array of nonempty relative paths")
    if (rebuild_files or incremental_only) and mode not in ("pch", "unity", "prune-deps"):
        raise ValueError("incremental options require command pch, unity or prune-deps")
    if incremental_only and not rebuild_files:
        raise ValueError("incremental-only requires rebuild-files")
    if auto_pch and runs < 3:
        raise ValueError("pch-auto requires at least three runs")
    link_target = os.environ.get("BUILDRAY_LINK_TARGET", "")
    link_dependency = os.environ.get("BUILDRAY_LINK_DEPENDENCY", "")
    if mode not in ("prune-links", "prune-deps") and (link_target or link_dependency):
        raise ValueError("link-target and link-dependency require command prune-links or prune-deps")
    if mode in ("prune-links", "prune-deps") and runs < 3:
        raise ValueError("Link cleanup requires at least three runs")
    dependency_trials = integer("BUILDRAY_MAX_DEPENDENCY_TRIALS", "64", 10000)
    dependency_seconds = integer("BUILDRAY_DEPENDENCY_TIME_BUDGET", "300", 86400)
    if mode != "prune-deps" and (dependency_trials != 64 or dependency_seconds != 300):
        raise ValueError("dependency search budgets require command prune-deps")
    unity_target = os.environ.get("BUILDRAY_UNITY_TARGET", "")
    batch_size = integer("BUILDRAY_UNITY_BATCH_SIZE", "4", 32)
    unity_retries = integer("BUILDRAY_UNITY_RETRIES", "0", 8, minimum=0)
    unity_exclusions = json.loads(os.environ.get("BUILDRAY_UNITY_EXCLUDE", "[]"))
    if batch_size < 2:
        raise ValueError("unity-batch-size must be at least 2")
    if not isinstance(unity_exclusions, list) or not all(isinstance(item, str) and item for item in unity_exclusions):
        raise ValueError("unity-exclude must be a JSON array of nonempty relative source paths")
    if mode == "unity" and not unity_target:
        raise ValueError("unity requires unity-target")
    if mode != "unity" and (unity_target or unity_exclusions or batch_size != 4 or unity_retries):
        raise ValueError("unity options require command unity")
    forward_declarations = boolean("BUILDRAY_FORWARD_DECLARATIONS", "false")
    header_paths = json.loads(os.environ.get("BUILDRAY_HEADER_PATHS", "[]"))
    max_header_edits = integer("BUILDRAY_MAX_HEADER_EDITS", "8", 1000)
    if not isinstance(header_paths, list) or not all(isinstance(item, str) and item for item in header_paths):
        raise ValueError("header-paths must be a JSON array of nonempty relative paths")
    if mode != "headers" and (forward_declarations or header_paths or max_header_edits != 8):
        raise ValueError("header options require command headers")
    jobs = integer("BUILDRAY_JOBS", "2", 1024)
    job_levels = json.loads(os.environ.get("BUILDRAY_JOB_LEVELS", "[]"))
    if not isinstance(job_levels, list) or any(type(j) is not int or not 1 <= j <= 1024 for j in job_levels):
        raise ValueError("job-levels must be a JSON array of integer job counts (1-1024)")
    if job_levels and (not 2 <= len(job_levels) <= 8 or len(set(job_levels)) != len(job_levels)
                       or mode not in ("unity", "pch") or auto_pch or unity_retries or runs < 3):
        raise ValueError("job-levels requires unity or manual pch, 2-8 distinct counts, three or more runs and no automatic retries/selection")
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
    if exclusions and mode not in ("optimize", "headers", "prune-links", "prune-deps"):
        raise ValueError("exclude-paths requires command optimize, headers, prune-links or prune-deps")
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
                   "--compiler", compiler, "--runs", str(runs),
                   "--session", str(uuid.uuid4())]
        if job_levels:
            command.extend(["--job-levels", ','.join(str(j) for j in job_levels)])
        else:
            command.extend(["--jobs", str(jobs)])
        for path in rebuild_files:
            command.extend(["--rebuild-file", path])
        if incremental_only:
            command.append("--incremental-only")
        if reference and mode == "measure":
            command[1] = "compare"
            command.extend(["--baseline-source", base_tree(source, reference, work),
                            "--max-regression-percent", str(budget)])
        if mode in ("optimize", "headers"):
            command.extend(["--clang-tidy", os.environ.get("BUILDRAY_CLANG_TIDY", "clang-tidy-21")])
        for path in exclusions:
            command.extend(["--exclude", path])
        if mode in ("prune-links", "prune-deps"):
            if link_target:
                command.extend(["--target", link_target])
            if link_dependency:
                command.extend(["--dependency", link_dependency])
        if mode == "prune-deps":
            command.extend(["--max-dependency-trials", str(dependency_trials),
                            "--dependency-time-budget", str(dependency_seconds)])
        if mode == "unity":
            command.extend(["--target", unity_target, "--batch-size", str(batch_size),
                            "--unity-retries", str(unity_retries)])
            for path in unity_exclusions:
                command.extend(["--unity-exclude", path])
        if mode == "headers":
            if forward_declarations:
                command.append("--forward-declarations")
            command.extend(["--max-header-edits", str(max_header_edits)])
            for path in header_paths:
                command.extend(["--header-path", path])
        if mode in ("deps", "headers", "unity", "prune-links", "prune-deps") or (mode == "pch" and rebuild_files):
            command.extend(["--c-compiler", os.environ.get("BUILDRAY_C_COMPILER", "clang-21")])
        if mode == "deps":
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
        result = run_cli(command, work / "run", artifacts)
        report_file = work / "run/report.json"
        report = json.loads(report_file.read_text()) if report_file.is_file() else {"status": "failed"}
        action_status.update(status="complete" if report["status"] == "passed" else "failed",
                             exit_code=result.returncode)
        if report["status"] != "passed":
            retain_failure_logs(work / "run", artifacts)
        output("result", ("cycles" if result.returncode == 5 else report.get("performance_result", "passed"))
               if report["status"] == "passed" else report["status"])
        retain_run_evidence(work / "run", artifacts, report)
        summary = work / ("run/parallelism.md" if job_levels else "run/headers.md" if mode == "profile" else
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
            if mode == "headers":
                text += f"\nModified headers: {report.get('modified_headers', 0)}. Include changes: {report.get('header_changes', 0)}.\n"
                if report.get("candidate"):
                    text += f"\nCandidate median: {report['candidate']['median_seconds']:.3f} s. Change: {report['wall_change_percent']:+.3f}% ({report['performance_result']}).\n"
                text += f"\nPatch validated: {report.get('patch_validated', False)}. Recommended: {report.get('patch_recommended', False)}.\n"
            if mode == "pch" and report.get("candidate"):
                text += f"\nPCH median: {report['candidate']['median_seconds']:.3f} s. Change: {report['wall_change_percent']:+.3f}% ({report['performance_result']}).\n"
                text += f"\nPatch validated: {report.get('patch_validated', False)}. Recommended: {report.get('patch_recommended', False)}.\n"
            if mode == "unity":
                text += f"\nUnity target: `{unity_target}`, batch size {batch_size}.\n"
                if report.get("unity_auto_exclusions"):
                    text += f"\nKept {len(report['unity_auto_exclusions'])} failed-group sources separate after compiler failures.\n"
                if report.get("candidate"):
                    text += f"\nCandidate median: {report['candidate']['median_seconds']:.3f} s. Change: {report['wall_change_percent']:+.3f}% ({report['performance_result']}).\n"
                text += f"\nPatch validated: {report.get('patch_validated', False)}. Recommended: {report.get('patch_recommended', False)}.\n"
            if mode in ("prune-links", "prune-deps"):
                selected = report.get("selected_link")
                if selected:
                    if mode == 'prune-deps':
                        text += f"\nValidated link removals: {report.get('removed_links', 1)}.\n"
                    else:
                        text += f"\nCMake candidate: `{selected['target']}` → `{selected['dependency']}`.\n"
                else:
                    text += "\nNo confirmed eligible link removal; inspect links.json for rejected trials.\n"
                if report.get('dependency_search'):
                    search = report['dependency_search']
                    text += f"\nSearch: {search['profiled_links']} archive profiles, {search['configurations']} configurations, {search['cache_hits']} cached rejections. Budget exhausted: {search['budget_exhausted']}.\n"
                if report.get("candidate"):
                    text += f"\nCandidate median: {report['candidate']['median_seconds']:.3f} s. Change: {report['wall_change_percent']:+.3f}% ({report['performance_result']}).\n"
                text += f"\nPatch validated: {report.get('patch_validated', False)}. Recommended: {report.get('patch_recommended', False)}.\n"
                if report.get('baseline', {}).get('linker_driver_wall_sum_median_seconds') is not None:
                    before = report['baseline']['linker_driver_wall_sum_median_seconds']
                    if report.get('candidate'):
                        after = report['candidate']['linker_driver_wall_sum_median_seconds']
                        text += f"\nLink-driver time sum, median: {before:.3f} → {after:.3f} s (parallel links may overlap).\n"
                    else:
                        text += f"\nBaseline link-driver time sum, median: {before:.3f} s; no candidate comparison (parallel links may overlap).\n"
            if report.get("automatic_selection"):
                selected = report["automatic_selection"]
                text += f"\nAutomatic candidate: `{selected['header']}` on `{selected['target']}`. Selection profiling is outside the measurements.\n"
        if report.get("incremental"):
            text += "\nIncremental rebuilds after single-file comment edits (compilation and linking; tests excluded):\n\n"
            text += "| File | Before | After | Change | Result |\n| --- | ---: | ---: | ---: | --- |\n"
            for scenario in report["incremental"]["scenarios"]:
                if scenario.get("status") == "passed":
                    path = scenario["file"].replace("|", "&#124;").replace("\n", " ").replace("\r", " ").replace("`", "&#96;")
                    text += f"| `{path}` | {scenario['baseline']['median_seconds']:.3f} s | {scenario['candidate']['median_seconds']:.3f} s | {scenario['wall_change_percent']:+.2f}% | {scenario['performance_result']} |\n"
            text += "\nThese probes measure dependency invalidation, not arbitrary code changes. Peak RSS in samples is per compiler, not total memory.\n"
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as stream:
                stream.write(text)
        return result.returncode

if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("buildray action: interrupted", file=sys.stderr)
        sys.exit(130)
    except (ValueError, OSError, subprocess.CalledProcessError, tarfile.TarError) as error:
        print(f"buildray action: {error}", file=sys.stderr)
        sys.exit(1)
