"""Unused archives: real LLD extraction, identical ELF replay, coverage and rejection cases."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary, compiler, linker = sys.argv[1:4]
old_cmake = sys.argv[4] if len(sys.argv) > 4 else None


def run(*args, expected=0, **kwargs):
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def digest(tree):
    return {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in tree.rglob('*') if p.is_file()}


with tempfile.TemporaryDirectory(prefix='buildray unused ') as folder:
    root = Path(folder)
    source = root / 'source tree'
    source.mkdir()
    (source / 'main.cpp').write_text('extern "C" int part(void); int main() { return part(); }\n')
    (source / 'part.c').write_text('int part(void) { return 0; }\n')
    (source / 'dead.cpp').write_text('int never_called() { return 17; }\n')
    cmake = source / 'CMakeLists.txt'
    original = '''cmake_minimum_required(VERSION 3.18)
project(unused_fixture LANGUAGES C CXX)
add_library(dead STATIC EXCLUDE_FROM_ALL dead.cpp)
add_executable(sample main.cpp part.c)
target_link_libraries(sample PRIVATE dead) # keep this comment
enable_testing()
add_test(NAME works COMMAND sample)
'''
    cmake.write_text(original)

    def invoke(label, *args, expected=0):
        work = root / label
        before = digest(source)
        run(binary, 'prune-deps', '--source', source, '--work-dir', work,
            '--compiler', compiler, '--runs', 3, '--jobs', 2, *args,
            '--', f'-DCMAKE_EXE_LINKER_FLAGS=--ld-path={linker}', expected=expected)
        assert before == digest(source), 'Original project was modified'
        return work, json.loads((work / 'report.json').read_text())

    work, report = invoke('unused', '--target', 'sample', '--dependency', 'dead',
                          '--rebuild-file', 'dead.cpp')
    assert report['patch_validated'] and report['cmake_graph_change_verified']
    assert report['removed_links'] == 1
    proof = report['link_evidence']
    assert proof['compiler_commands_unchanged'] and len(proof['changed_links']) == 1
    link = proof['changed_links'][0]
    assert link['identical_elf'] and link['removed_archives'][0]['extracted'] == 0
    assert link['removed_archives'][0]['members'] == 1
    assert link['removed_archives'][0]['archive'].endswith('libdead.a')
    assert report['baseline_tests_passed'] == report['candidate_tests_passed'] == 1
    for variant in ['baseline', 'candidate']:
        assert len(report[variant]['samples']) == 3
        for sample in report[variant]['samples']:
            assert sample['linker_invocations'] == 1
            assert sample['linker_driver_wall_sum_seconds'] > 0
            assert len(sample['linked_outputs']) == 1
    scenario = report['incremental']['scenarios'][0]
    assert all(s['compiler_invocations'] == s['linker_invocations'] == 1 for s in scenario['baseline']['samples'])
    assert all(s['compiler_invocations'] == s['linker_invocations'] == 0 for s in scenario['candidate']['samples'])
    assert scenario['input_restored']
    run('git', 'apply', '--check', work / 'cmake.patch', cwd=source)
    run('git', 'apply', work / 'cmake.patch', cwd=source)
    run('cmake', '-S', source, '-B', root / 'applied', '-G', 'Ninja', f'-DCMAKE_CXX_COMPILER={compiler}')
    run('cmake', '--build', root / 'applied', '--parallel', 2)
    run('ctest', '--test-dir', root / 'applied', '--output-on-failure')

    def rejected(label, addition='', replacement=None, reason=None):
        cmake.write_text((replacement or original) + addition)
        w, result = invoke(label)
        assert result['performance_result'] == 'no_changes' and not result['patch_validated']
        assert not (w / 'cmake.patch').exists()
        if reason:
            assert any(reason in row.get('reason', '') for row in result['dependency_trials']), result

    # Keep compile usage requirements while dropping an unused archive. Older CMake
    # retains the original declaration rather than seeing an unsupported expression.
    cmake.write_text(original + 'target_compile_definitions(dead INTERFACE UNUSED_DEFINE=1)\n')
    keep_work, kept = invoke('usage')
    assert kept['patch_validated'] and kept['selected_link']['compile_only']
    assert 'compiler commands' in kept['dependency_trials'][0]['reason']
    run('git', 'apply', keep_work / 'cmake.patch', cwd=source)
    configure = old_cmake or 'cmake'
    if not old_cmake:
        cmake.write_text(cmake.read_text().replace('if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)', 'if(FALSE)'))
    run(configure, '-S', source, '-B', root / 'legacy-applied', '-G', 'Ninja', f'-DCMAKE_CXX_COMPILER={compiler}')
    run(configure, '--build', root / 'legacy-applied', '--parallel', 2)
    run('ctest', '--test-dir', root / 'legacy-applied', '--output-on-failure')
    # Whole-archive can load constructors and registration objects without a symbol reference.
    rejected('whole-archive', replacement=original.replace('PRIVATE dead', 'PRIVATE "-Wl,--whole-archive" dead "-Wl,--no-whole-archive"'))
    # A required archive cannot be removed even when another library could satisfy its symbols.
    (source / 'dead.cpp').write_text('int never_called() { return 0; }\n')
    (source / 'main.cpp').write_text('int never_called(); int main() { return never_called(); }\n')
    rejected('required')
    (source / 'main.cpp').write_text('extern "C" int part(void); int main() { return part(); }\n')
    rejected('nondeterministic', 'target_link_options(sample PRIVATE "-Wl,--build-id=uuid")\n', reason='ELF binary')
    # CMake callbacks cannot inflate a gain by excluding an unrelated static target.
    (source / 'other.cpp').write_text('int unrelated() { return 9; }\n')
    rejected('unrelated-work', '''add_library(other STATIC other.cpp)
get_target_property(deps sample LINK_LIBRARIES)
if(NOT "dead" IN_LIST deps)
  set_target_properties(other PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
''', reason='outside the removed archives')
    rejected('public', replacement=original.replace('PRIVATE dead', 'PUBLIC dead'))
    rejected('shared', replacement=original.replace('dead STATIC', 'dead SHARED'))
    rejected('variable', replacement=original.replace('target_link_libraries(sample PRIVATE dead)', 'set(deps dead)\ntarget_link_libraries(sample PRIVATE ${deps})'))
    cmake.write_text(original)
    _, excluded = invoke('excluded', '--exclude', 'CMakeLists.txt')
    assert excluded['performance_result'] == 'no_changes'

    # A different CTest inventory after removal is rejected by final validation.
    cmake.write_text(original + '''get_target_property(deps sample LINK_LIBRARIES)
if(NOT "dead" IN_LIST deps)
  add_test(NAME unexpected COMMAND sample)
endif()
''')
    _, failed = invoke('test-inventory', expected=1)
    assert not failed['patch_validated'] and 'CTest inventory changed' in failed['error']
    run(binary, 'prune-deps', '--runs', 1, expected=2)

print('Unused dependency experiments passed')
