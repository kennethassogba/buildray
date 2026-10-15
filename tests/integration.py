"""Exercise the actual binary, CMake, Clang, clang-tidy, CTest and patch application."""

import hashlib
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

binary, compiler, tidy = sys.argv[1:]


def run(*args, expected=0, **kwargs):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def digest(tree):
    return {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in tree.rglob("*") if p.is_file()}


def project(path, *, fail=False, tests=True, multi=False):
    path.mkdir()
    (path / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.30)
project(fixture LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(sample "main file.cpp")
{'''add_executable(second "main file.cpp")
target_compile_definitions(second PRIVATE USE_VECTOR=1)''' if multi else ''}
{'enable_testing()' if tests else ''}
{'add_test(NAME works COMMAND sample)' if tests else ''}
""")
    (path / "main file.cpp").write_text("""#include <vector>
#include <map> // IWYU pragma: keep
#ifdef USE_VECTOR
int main() { std::vector<int> values{1}; return values.size() != 1; }
#else
int main() { return """ + ("1" if fail else "0") + "; }\n#endif\n")


def optimize(source, work, *extra, expected=0, runs=1):
    return run(binary, "optimize", "--source", str(source), "--work-dir", str(work),
               "--compiler", compiler, "--clang-tidy", tidy, "--runs", str(runs),
               "--jobs", "2", *extra, expected=expected)


with tempfile.TemporaryDirectory(prefix="buildray integration ") as directory:
    root = Path(directory)
    source = root / "source tree"
    project(source)
    original = digest(source)
    output = root / "result"
    optimize(source, output, runs=3)
    report = json.loads((output / "report.json").read_text())
    assert report["status"] == "passed" and report["validation"] == "build_and_tests_passed", report
    assert report["patch_validated"] is True
    assert report["removed_includes"] == 1, report
    assert report["baseline_tests_passed"] == report["candidate_tests_passed"] == 1
    assert len(report["baseline"]["samples"]) == len(report["candidate"]["samples"]) == 3
    assert report["baseline"]["median_seconds"] > 0
    assert report["baseline"]["samples"][0]["compiler_cpu_seconds"] > 0
    assert report["baseline"]["samples"][0]["max_compiler_rss_kib"] > 0
    assert digest(source) == original, "original sources changed"
    patch = output / "includes.patch"
    assert "-#include <vector>" in patch.read_text()
    assert "-#include <map>" not in patch.read_text()
    run("git", "apply", "--check", str(patch), cwd=source)
    run("git", "apply", str(patch), cwd=source)
    assert (source / "main file.cpp").read_bytes() == (output / "candidate-source/main file.cpp").read_bytes()

    # An existing workspace is never overwritten, even after a successful run.
    optimize(source, output, expected=1)
    assert json.loads((output / "report.json").read_text()) == report

    # No-op cleanup stays a successful result with an empty patch, no invented gain.
    noop = root / "noop"
    optimize(source, noop)
    no_report = json.loads((noop / "report.json").read_text())
    assert no_report["removed_includes"] == 0 and not (noop / "includes.patch").read_text()
    assert not no_report["patch_recommended"]
    assert no_report["performance_result"] == "no_changes"

    measured = root / "measurement"
    run(binary, "measure", "--source", str(source), "--work-dir", str(measured),
        "--compiler", compiler, "--runs", "1")
    measured_report = json.loads((measured / "report.json").read_text())
    assert measured_report["status"] == "passed" and "candidate" not in measured_report
    assert measured_report["baseline"]["median_seconds"] > 0

    # A failing baseline test must stop the pipeline before any patch is proposed.
    failing = root / "failing"
    project(failing, fail=True)
    rejected = root / "rejected"
    optimize(failing, rejected, expected=1)
    assert json.loads((rejected / "report.json").read_text())["phase"] == "baseline_validation"
    assert not (rejected / "includes.patch").exists()

    # No tests must be explicit and must never be called test validation.
    untested = root / "untested"
    project(untested, tests=False)
    optimize(untested, root / "no-tests-rejected", expected=1)
    allowed = root / "no-tests-allowed"
    optimize(untested, allowed, "--allow-no-tests")
    assert json.loads((allowed / "report.json").read_text())["validation"] == "build_only"

    # The same file is used by another target with USE_VECTOR: no consensus removal.
    multiple = root / "multiple"
    project(multiple, multi=True)
    multi_result = root / "multiple-result"
    optimize(multiple, multi_result)
    multi_report = json.loads((multi_result / "report.json").read_text())
    assert multi_report["compile_commands_analyzed"] == 2
    assert multi_report["removed_includes"] == 0, multi_report

    # Reject launcher overrides and targets absent from the default build:
    # every C++ compilation in the database must actually be measured.
    override = root / "override"
    project(override)
    with (override / "CMakeLists.txt").open("a") as stream:
        stream.write('set_property(TARGET sample PROPERTY CXX_COMPILER_LAUNCHER "")\n')
    override_output = root / "override-result"
    optimize(override, override_output, expected=1)
    assert "Compiler coverage" in json.loads((override_output / "report.json").read_text())["error"]

    excluded = root / "excluded"
    project(excluded)
    with (excluded / "CMakeLists.txt").open("a") as stream:
        stream.write('add_executable(extra EXCLUDE_FROM_ALL "main file.cpp")\n')
    optimize(excluded, root / "excluded-result", expected=1)

    # Target properties can override global PCH/unity defaults. Refuse these
    # instead of claiming that independent source compilation was validated.
    for kind in ["pch", "unity"]:
        target = root / kind
        project(target)
        with (target / "CMakeLists.txt").open("a") as stream:
            if kind == "pch":
                stream.write('set_property(TARGET sample PROPERTY DISABLE_PRECOMPILE_HEADERS OFF)\n'
                             'target_precompile_headers(sample PRIVATE <vector>)\n')
            else:
                # CMake excludes module-scanned sources from unity batches.
                stream.write('set_property(TARGET sample PROPERTY CXX_SCAN_FOR_MODULES OFF)\n'
                             'set_property(TARGET sample PROPERTY UNITY_BUILD ON)\n')
        optimize(target, root / (kind + "-result"), expected=1)

    # Disabled and skipped tests must not become a false validation success.
    for kind in ["disabled", "skipped"]:
        skipped = root / kind
        project(skipped)
        property_value = "DISABLED TRUE" if kind == "disabled" else "SKIP_RETURN_CODE 0"
        with (skipped / "CMakeLists.txt").open("a") as stream:
            stream.write(f"set_tests_properties(works PROPERTIES {property_value})\n")
        optimize(skipped, root / (kind + "-result"), expected=1)

    # A symlink must not escape the isolated tree, even when it points at a
    # writable external file.
    linked = root / "linked"
    project(linked)
    external = root / "external.cpp"
    external.write_text("int main() {}\n")
    (linked / "outside.cpp").symlink_to(external)
    optimize(linked, root / "linked-result", expected=1)
    assert external.read_text() == "int main() {}\n"

    # Force a plausible include-cleaner false positive, then ensure validation
    # catches it. The fake analyzer is only used in this failure-path test.
    broken = root / "broken-candidate"
    project(broken)
    (broken / "main file.cpp").write_text("#include <vector>\nint main() { std::vector<int> v; return v.size(); }\n")
    fake = root / "fake-tidy"
    fake.write_text("""#!/usr/bin/env python3
import json, pathlib, sys
if '--version' in sys.argv:
    print('test analyzer')
elif '--list-checks' in sys.argv:
    print('misc-include-cleaner')
else:
    path = sys.argv[-1]
    output = next(x.split('=', 1)[1] for x in sys.argv if x.startswith('--export-fixes='))
    pathlib.Path(output).write_text(json.dumps({'Diagnostics': [{'DiagnosticName': 'misc-include-cleaner',
        'DiagnosticMessage': {'Replacements': [{'FilePath': path, 'Offset': 0, 'Length': 18,
        'ReplacementText': ''}]}}]}))
""")
    fake.chmod(0o755)
    broken_work = root / "broken-result"
    optimize(broken, broken_work, "--clang-tidy", str(fake), expected=1)
    broken_report = json.loads((broken_work / "report.json").read_text())
    assert broken_report["phase"] == "candidate_validation" and broken_report["status"] == "failed"
    assert broken_report["patch_recommended"] is False and "baseline" not in broken_report
    assert "#include <vector>" in (broken / "main file.cpp").read_text()

    # Some headers have runtime registration side effects: compilation alone
    # would pass. A candidate test failure must also invalidate the patch.
    runtime = root / "runtime"
    project(runtime)
    (runtime / "registration.hpp").write_text("struct Registration { Registration() { registered = true; } };\ninline Registration registration;\n")
    directive = '#include "registration.hpp"\n'
    registration_prefix = "inline bool registered = false;\n"
    (runtime / "main file.cpp").write_text(registration_prefix + directive +
                                           "int main() { return registered ? 0 : 1; }\n")
    fake_text = fake.read_text().replace("'Offset': 0, 'Length': 18",
                                       f"'Offset': {len(registration_prefix)}, 'Length': {len(directive)}")
    runtime_tidy = root / "runtime-tidy"
    runtime_tidy.write_text(fake_text)
    runtime_tidy.chmod(0o755)
    runtime_work = root / "runtime-result"
    optimize(runtime, runtime_work, "--clang-tidy", str(runtime_tidy), expected=1)
    runtime_report = json.loads((runtime_work / "report.json").read_text())
    assert runtime_report["phase"] == "candidate_validation" and not runtime_report["patch_validated"], runtime_report
    assert "ctest exited" in runtime_report["error"], runtime_report

    # The launcher preserves compiler failure, signal status, and shell literals.
    records = root / "launcher records"
    run(binary, "_launch", str(records), "--", "/bin/sh", "-c", "exit 7", expected=7)
    assert json.loads(next(records.glob("*.json")).read_text())["exit_code"] == 7
    run(binary, "_launch", str(root / "signal-records"), "--", "/bin/sh", "-c", "kill -TERM $$", expected=143)

    # Interrupt a build while it has live subprocesses. Completion must be
    # prompt and the interrupted run must retain an explicit failure report.
    slow = root / "slow"
    project(slow)
    with (slow / "CMakeLists.txt").open("a") as stream:
        stream.write('add_custom_target(wait_for_interrupt ALL COMMAND ${CMAKE_COMMAND} -E sleep 30)\n')
    slow_work = root / "interrupted"
    process = subprocess.Popen([binary, "optimize", "--source", str(slow), "--work-dir", str(slow_work),
                                "--compiler", compiler, "--clang-tidy", tidy],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 15
        while not (slow_work / "logs/baseline-warmup.log").exists():
            assert process.poll() is None, process.communicate()[0]
            assert time.monotonic() < deadline, "build did not start"
            time.sleep(0.05)
        process.send_signal(signal.SIGTERM)
        interrupted_output, _ = process.communicate(timeout=8)
        assert process.returncode == 130, interrupted_output
        assert json.loads((slow_work / "report.json").read_text())["status"] == "interrupted"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()

    for arguments in [("--jobs", "0"), ("--runs", "-1"), ("--jobs", "2junk"), ("--unknown", "x")]:
        run(binary, "optimize", *arguments, expected=2)
    run(binary, "optimize", "--source", str(source), "--", "-DCMAKE_CXX_COMPILER=/bin/false", expected=1)
    run(binary, "--help")
    run(binary, "--version")

print("Integration checks passed: real include removal, tests, patch application, measurements, refusals")
