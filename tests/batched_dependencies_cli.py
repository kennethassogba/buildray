"""Real archive screening, cumulative batch search, budgets and Action integration."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary, compiler, linker, action = sys.argv[1:5]
binary, compiler, linker, action = [str(Path(p).absolute()) for p in [binary, compiler, linker, action]]


def run(*args, expected=0, **kwargs):
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def digest(tree):
    return {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in tree.rglob('*') if p.is_file()}


with tempfile.TemporaryDirectory(prefix='buildray batches ') as folder:
    root = Path(folder)
    source = root / 'source tree'
    source.mkdir()
    cmake = source / 'CMakeLists.txt'
    (source / 'main.cpp').write_text('int needed(); int main() { return needed(); }\n')
    (source / 'needed.cpp').write_text('int needed() { return 0; }\n')
    (source / 'unused.cpp').write_text('int unused() { return 17; }\n')
    prefix = '''cmake_minimum_required(VERSION 3.30)
project(batches LANGUAGES CXX)
enable_testing()
add_library(needed STATIC EXCLUDE_FROM_ALL needed.cpp)
add_executable(sample main.cpp)
add_test(NAME works COMMAND sample)
'''

    def invoke(label, *args):
        work = root / label
        before = digest(source)
        run(binary, 'prune-deps', '--source', source, '--work-dir', work,
            '--compiler', compiler, '--runs', 3, '--jobs', 2, *args,
            '--', f'-DCMAKE_EXE_LINKER_FLAGS=--ld-path={linker}')
        assert before == digest(source), 'Original project was modified'
        return work, json.loads((work / 'report.json').read_text())

    def apply_and_test(work, label):
        run('git', 'apply', '--check', work / 'cmake.patch', cwd=source)
        run('git', 'apply', work / 'cmake.patch', cwd=source)
        build = root / (label + '-applied')
        run('cmake', '-S', source, '-B', build, '-G', 'Ninja', f'-DCMAKE_CXX_COMPILER={compiler}')
        run('cmake', '--build', build, '--parallel', 2)
        run('ctest', '--test-dir', build, '--output-on-failure')

    # Forty dependencies in one call: one archive profile, one trial configure,
    # one mandatory final verification, then one complete build/test validation.
    names = [f'dead{i:02}' for i in range(40)]
    large = prefix + ''.join(f'add_library({n} STATIC EXCLUDE_FROM_ALL unused.cpp)\n' for n in names)
    large += 'target_link_libraries(sample PRIVATE needed ' + ' '.join(names) + ') # preserve\n'
    cmake.write_text(large)
    work, report = invoke('forty')
    assert report['patch_validated'] and report['removed_links'] == 40
    assert len(report['selected_links']) == 40
    search = report['dependency_search']
    assert search['profiled_links'] == search['configurations'] == search['final_verifications'] == 1, search
    assert len(report['dependency_trials']) == 1 and report['dependency_trials'][0]['batch_size'] == 40
    assert any(r['dependency'] == 'needed' and r['status'] == 'contributes_objects'
               for r in report['dependency_screening'])
    assert all(s['compiler_invocations'] == 42 for s in report['baseline']['samples'])
    assert all(s['compiler_invocations'] == 2 for s in report['candidate']['samples'])
    assert len(report['link_evidence']['changed_links']) == 1
    assert not (work / 'dependency-profiles/binaries').exists()
    apply_and_test(work, 'forty')
    assert '# preserve' in cmake.read_text()

    # Multiple calls in one file and a consumer in a subdirectory. One archive
    # carries a compile definition, so the whole group uses COMPILE_ONLY.
    multiple = prefix + '''add_library(a STATIC EXCLUDE_FROM_ALL unused.cpp)
add_library(b STATIC EXCLUDE_FROM_ALL unused.cpp)
add_library(c STATIC EXCLUDE_FROM_ALL unused.cpp)
add_library(d STATIC EXCLUDE_FROM_ALL unused.cpp)
target_compile_definitions(a INTERFACE REQUIRED_DEFINE=1)
target_link_libraries(sample PRIVATE needed a)
target_link_libraries(sample PRIVATE b d)
add_subdirectory(sub)
'''
    sub = source / 'sub'
    sub.mkdir()
    (sub / 'CMakeLists.txt').write_text('''add_executable(second ../main.cpp)
target_link_libraries(second PRIVATE needed c)
add_test(NAME second_works COMMAND second)
''')
    cmake.write_text(multiple)
    work, report = invoke('multiple')
    assert report['removed_links'] == 4 and report['patch_validated']
    assert report['dependency_search']['profiled_links'] == 2
    assert report['dependency_search']['configurations'] == 2
    assert all(row['compile_only'] for row in report['selected_links'])
    assert len(report['link_evidence']['changed_links']) == 2
    assert report['baseline_tests_passed'] == report['candidate_tests_passed'] == 2
    apply_and_test(work, 'multiple')
    assert cmake.read_text().count('if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)') == 2
    assert (sub / 'CMakeLists.txt').read_text().count('if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)') == 1
    (sub / 'CMakeLists.txt').unlink()
    sub.rmdir()

    # Both removals are individually harmless; removing both changes the build.
    # Every later trial must include the already accepted removal.
    interaction = prefix + '''add_library(a STATIC EXCLUDE_FROM_ALL unused.cpp)
add_library(b STATIC EXCLUDE_FROM_ALL unused.cpp)
target_link_libraries(sample PRIVATE needed a b)
get_target_property(deps sample LINK_LIBRARIES)
if(NOT "a" IN_LIST deps AND NOT "b" IN_LIST deps)
  target_compile_definitions(sample PRIVATE UNEXPECTED_CHANGE=1)
endif()
'''
    cmake.write_text(interaction)
    work, report = invoke('interaction')
    assert report['patch_validated'] and report['removed_links'] == 1
    assert report['dependency_search']['cache_hits'] >= 1
    assert not report['dependency_search']['budget_exhausted']
    assert report['dependency_search']['final_verifications'] == 1
    apply_and_test(work, 'interaction')

    cmake.write_text(interaction)
    work, report = invoke('partial', '--max-dependency-trials', 3)
    assert report['patch_validated'] and report['removed_links'] == 1
    search = report['dependency_search']
    assert search['budget_exhausted'] and search['configurations'] == 3, search
    assert search['final_verifications'] == 1
    apply_and_test(work, 'partial')

    # Time is a soft budget: finish the running command, start no later trial.
    cmake.write_text(interaction.replace('target_compile_definitions(sample PRIVATE UNEXPECTED_CHANGE=1)',
                                        'execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)\n'
                                        '  target_compile_definitions(sample PRIVATE UNEXPECTED_CHANGE=1)'))
    work, report = invoke('time-budget', '--dependency-time-budget', 1)
    assert report['dependency_search']['budget_exhausted']
    assert report['dependency_search']['configurations'] <= 1
    assert not report['patch_validated'] and not (work / 'cmake.patch').exists()

    for flag in ['--max-dependency-trials', '--dependency-time-budget']:
        for value in ['0', '-1', '999999', 'oops']:
            run(binary, 'prune-deps', flag, value, expected=2)
        run(binary, 'profile', flag, '3', expected=2)

    # A global compiler-inventory blocker must stop before expensive link replay
    # or repeated configure attempts, with a reason for every skipped candidate.
    (source / 'flags.rsp').write_text('-DRESPONSE_VALUE=1\n')
    cmake.write_text(prefix + '''add_library(a STATIC EXCLUDE_FROM_ALL unused.cpp)
target_link_libraries(sample PRIVATE needed a)
target_compile_options(sample PRIVATE "@${CMAKE_CURRENT_SOURCE_DIR}/flags.rsp")
''')
    work, report = invoke('unsupported', '--incremental-only', '--rebuild-file', 'main.cpp')
    assert not report['patch_validated']
    assert 'response files' in report['dependency_search_blocker']
    assert report['dependency_search']['profiled_links'] == report['dependency_search']['configurations'] == 0
    assert all(row['status'] == 'unsupported_configuration' for row in report['dependency_screening'])
    (source / 'flags.rsp').unlink()

    # Run the actual Action entry point; check its inputs, retained evidence and
    # grouped summary, rather than testing a second copy of its implementation.
    cmake.write_text(prefix + '''add_library(a STATIC EXCLUDE_FROM_ALL unused.cpp)
add_library(b STATIC EXCLUDE_FROM_ALL unused.cpp)
target_link_libraries(sample PRIVATE needed a b)
''')
    temp = root / 'runner'
    temp.mkdir()
    output, summary = root / 'outputs.txt', root / 'summary.md'
    env = {k: v for k, v in os.environ.items() if not k.startswith('BUILDRAY_')}
    env.update(GITHUB_WORKSPACE=str(source), RUNNER_TEMP=str(temp), GITHUB_OUTPUT=str(output),
               GITHUB_STEP_SUMMARY=str(summary), GITHUB_ACTION_PATH=str(Path(action).parent.parent),
               BUILDRAY_BINARY=binary, BUILDRAY_COMPILER=compiler, BUILDRAY_COMMAND='prune-deps',
               BUILDRAY_JOBS='2', BUILDRAY_RUNS='3', BUILDRAY_MAX_DEPENDENCY_TRIALS='7',
               BUILDRAY_DEPENDENCY_TIME_BUDGET='60',
               BUILDRAY_CMAKE_ARGS=json.dumps([f'-DCMAKE_EXE_LINKER_FLAGS=--ld-path={linker}']))
    run(sys.executable, action, env=env)
    outputs = dict(line.split('=', 1) for line in output.read_text().splitlines())
    result = json.loads(Path(outputs['report']).read_text())
    assert result['removed_links'] == 2 and result['patch_validated']
    assert result['dependency_search']['trial_limit'] == 7
    assert result['dependency_search']['time_budget_seconds'] == 60
    assert Path(outputs['archive-profile']).is_file()
    artifacts = Path(outputs['artifact-path'])
    assert list((artifacts / 'dependency-profiles').rglob('*.tsv'))
    assert not list(artifacts.rglob('*.elf'))
    assert '2' in summary.read_text() and 'configuration' in summary.read_text().lower()
    env['BUILDRAY_MAX_DEPENDENCY_TRIALS'] = '0'
    run(sys.executable, action, env=env, expected=1)

print('Batched dependency experiments passed: 40 links, multiple consumers, interactions, budgets and Action')
