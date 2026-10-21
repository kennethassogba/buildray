"""Sequential Unity/PCH comparisons: frozen inputs, fixed candidate, failures and Action."""
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

binary, compiler, action = [str(Path(p).absolute()) for p in sys.argv[1:4]]


def run(*args, expected=0, **kwargs):
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='buildray parallelism ') as folder:
    root = Path(folder)
    source = root / 'source tree'
    source.mkdir()
    cmake = source / 'CMakeLists.txt'
    original = '''cmake_minimum_required(VERSION 3.30)
project(parallelism LANGUAGES CXX)
set(core_sources a.cpp b.cpp)
add_library(core ${core_sources})
add_executable(sample main.cpp)
target_link_libraries(sample PRIVATE core)
enable_testing()
add_test(NAME works COMMAND sample)
'''
    cmake.write_text(original)
    (source / 'api.hpp').write_text('#pragma once\n#include <vector>\nint a(); int b();\n')
    (source / 'a.cpp').write_text('#include "api.hpp"\nint a() { return 0; }\n')
    (source / 'b.cpp').write_text('#include "api.hpp"\nint b() { return 0; }\n')
    (source / 'main.cpp').write_text('#include "api.hpp"\nint main() { return a() + b(); }\n')
    saved = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir()}

    def args(work, mode='unity', levels='1,2'):
        command = [binary, mode, '--source', source, '--target', 'core', '--compiler', compiler,
                   '--work-dir', work, '--job-levels', levels, '--runs', '3']
        if mode == 'pch':
            command += ['--header', 'api.hpp']
        return command

    for mode in ['unity', 'pch']:
        work = source / 'measurements' if mode == 'unity' else root / mode
        run(*args(work, mode), '--rebuild-file', 'a.cpp')
        report = json.loads((work / 'report.json').read_text())
        assert report == json.loads((work / 'parallelism.json').read_text())
        assert report['status'] == 'passed' and report['parallelism_sweep']
        assert report['candidate_consistency_verified'] and report['patch_validated']
        assert report['job_levels'] == [1, 2]
        assert 'baseline' not in report, 'Different parallelism levels must never be pooled'
        for level in report['levels']:
            assert level['status'] == 'passed' and level['exit_code'] == 0
            child = json.loads((work / level['report']).read_text())
            assert child['jobs'] == level['jobs'] == child['measurement_context']['jobs']
            assert child['baseline_tests_passed'] == child['candidate_tests_passed'] == 1
            assert len(level['baseline']['samples']) == len(level['candidate']['samples']) == 3
            assert level['incremental']['status'] == 'passed'
            assert level['effective_cpu_capacity'] > 0
            assert level['exceeds_detected_cpu_capacity'] == (level['jobs'] > level['effective_cpu_capacity'])
            assert (work / f'jobs-{level["jobs"]}' / f'{mode}.patch').read_bytes() == (work / f'{mode}.patch').read_bytes()
            if mode == 'unity':
                assert not (work / f'jobs-{level["jobs"]}/baseline-source/measurements').exists()
        assert not (work / 'input/measurements').exists()
        assert all(f'| {j} |' in (work / 'parallelism.md').read_text() for j in [1, 2])
        assert 'Edited file' in (work / 'parallelism.md').read_text()
        assert {p: hashlib.sha256((source / p).read_bytes()).hexdigest() for p in saved} == saved
        run(*args(work, mode), expected=1)
        if mode == 'unity':
            work.rename(root / 'unity')
            work = root / 'unity'
        # Independently apply each generated patch and execute it.
        applied = root / (mode + '-source')
        applied.mkdir()
        for name in saved:
            (applied / name).write_bytes((source / name).read_bytes())
        run('git', 'apply', '--check', work / f'{mode}.patch', cwd=applied)
        run('git', 'apply', work / f'{mode}.patch', cwd=applied)
        build = root / (mode + '-build')
        extra = ['-DBUILDRAY_UNITY=ON'] if mode == 'unity' else ['-DBUILDRAY_PCH=ON']
        run('cmake', '-S', applied, '-B', build, '-G', 'Ninja', f'-DCMAKE_CXX_COMPILER={compiler}', *extra)
        run('cmake', '--build', build, '--parallel', '2')
        run('ctest', '--test-dir', build, '--output-on-failure')

    # One level can validate on its own yet change the source ordering. Reject
    # comparing different generated groups even when both child suites pass.
    cmake.write_text(original.replace('add_library(core ${core_sources})', '''if(CMAKE_CURRENT_SOURCE_DIR MATCHES "/jobs-2/")
  set(core_sources b.cpp a.cpp)
endif()
add_library(core ${core_sources})'''))
    work = root / 'different-groups'
    run(*args(work), expected=1)
    report = json.loads((work / 'report.json').read_text())
    assert 'Unity source groups changed' in report['error']
    assert all(row['status'] == 'passed' for row in report['levels'])
    assert not report['patch_validated'] and not (work / 'unity.patch').exists()

    cmake.write_text(original + '''if(CMAKE_CURRENT_SOURCE_DIR MATCHES "/jobs-2/")
  add_test(NAME failure COMMAND "${CMAKE_COMMAND}" -E false)
endif()
''')
    work = root / 'failure'
    run(*args(work, levels='1,2,3'), expected=1)
    report = json.loads((work / 'report.json').read_text())
    assert [row['status'] for row in report['levels']] == ['passed', 'failed', 'not_run']
    assert not report['patch_validated'] and not report['patch_recommended']
    assert not (work / 'unity.patch').exists()
    assert (work / 'jobs-1/report.json').is_file() and (work / 'jobs-2/report.json').is_file()

    # Interrupt a later level; earlier measurements and the failed checkpoint survive.
    cmake.write_text(original + '''if(CMAKE_CURRENT_SOURCE_DIR MATCHES "/jobs-2/")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 20)
endif()
''')
    work = root / 'interrupt'
    with (root / 'interrupt.log').open('w') as stream:
        process = subprocess.Popen([str(a) for a in args(work)], stdout=stream, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                try:
                    state = json.loads((work / 'report.json').read_text())
                    if state['levels'][1]['status'] == 'running' and (work / 'jobs-2/logs/baseline-configure.log').exists():
                        break
                except (OSError, ValueError):
                    pass
                assert process.poll() is None, (root / 'interrupt.log').read_text()
                time.sleep(0.05)
            else:
                raise AssertionError('Second job level never started')
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=10) == 130
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    state = json.loads((work / 'report.json').read_text())
    assert state['status'] == 'interrupted' and state['levels'][0]['status'] == 'passed'
    assert not state['patch_recommended'] and not (work / 'unity.patch').exists()
    cmake.write_text(original)

    for invalid in ['1', '1,1', '0,2', '1,', ',2', '1,no', '1,1025', '1,2,3,4,5,6,7,8,9']:
        run(*args(root / 'invalid', levels=invalid), expected=2)
    for extra in [['--jobs', '2'], ['--unity-retries', '1'], ['--runs', '1']]:
        run(*args(root / 'invalid'), *extra, expected=2)
    run(binary, 'measure', '--job-levels', '1,2', expected=2)
    run(binary, 'pch', '--auto', '--job-levels', '1,2', expected=2)

    # Action outputs must point to the parent, while retaining every child report.
    output, summary = root / 'outputs', root / 'summary'
    env = {k: v for k, v in os.environ.items() if not k.startswith('BUILDRAY_')}
    env.update(GITHUB_WORKSPACE=str(source), RUNNER_TEMP=str(root), GITHUB_OUTPUT=str(output),
               GITHUB_STEP_SUMMARY=str(summary), BUILDRAY_BINARY=binary, BUILDRAY_COMPILER=compiler,
               BUILDRAY_COMMAND='unity', BUILDRAY_UNITY_TARGET='core', BUILDRAY_JOB_LEVELS='[1,2]')
    run(sys.executable, action, env=env)
    outputs = dict(line.split('=', 1) for line in output.read_text().splitlines())
    parent = json.loads(Path(outputs['report']).read_text())
    assert parent['parallelism_sweep'] and parent['patch_validated']
    assert Path(outputs['parallelism']).is_file() and Path(outputs['parallelism-summary']).is_file()
    artifacts = Path(outputs['artifact-path'])
    assert Path(outputs['report']).parent == Path(outputs['patch']).parent == artifacts
    for jobs in [1, 2]:
        assert (artifacts / f'jobs-{jobs}/report.json').is_file()
        assert list((artifacts / f'jobs-{jobs}/logs').glob('*-compilers.json'))
    assert not list(artifacts.rglob('*.o'))
    assert 'Parallelism comparison' in summary.read_text()
    for bad in ['[true,2]', '[1,1]', '[1]', '[0,2]', '"1,2"', 'null']:
        run(sys.executable, action, env={**env, 'BUILDRAY_JOB_LEVELS': bad}, expected=1)
    run(sys.executable, action, env={**env, 'BUILDRAY_COMMAND': 'measure', 'BUILDRAY_UNITY_TARGET': ''}, expected=1)

print('Parallelism comparisons passed: Unity/PCH, fixed groups, nested output, failures, interruption and Action')
