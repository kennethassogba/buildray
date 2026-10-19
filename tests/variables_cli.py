"""Evaluate CMake variables in their original scopes; validate guarded patches."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary, compiler, linker = sys.argv[1:4]


def run(*args, **kwargs):
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)
    assert result.returncode == 0, (args, result.returncode, result.stdout)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='buildray variables ') as folder:
    root = Path(folder)
    source = root / 'source'
    source.mkdir()
    (source / 'main.cpp').write_text('int needed(); int main() { return needed(); }\n')
    (source / 'needed.cpp').write_text('int needed() { return 0; }\n')
    (source / 'unused.cpp').write_text('int unused() { return 17; }\n')
    cmake = source / 'CMakeLists.txt'
    prefix = '''cmake_minimum_required(VERSION 3.30)
project(variables LANGUAGES CXX)
enable_testing()
add_library(needed STATIC EXCLUDE_FROM_ALL needed.cpp)
add_library(dead STATIC EXCLUDE_FROM_ALL unused.cpp)
add_library(extra STATIC EXCLUDE_FROM_ALL unused.cpp)
'''

    def invoke(label, text, *args):
        cmake.write_text(text)
        before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in source.rglob('*') if p.is_file()}
        work = root / label
        run(binary, 'prune-deps', '--source', source, '--work-dir', work,
            '--compiler', compiler, '--runs', 3, '--jobs', 2, '--incremental-only',
            '--rebuild-file', 'unused.cpp', *args,
            '--', f'-DCMAKE_EXE_LINKER_FLAGS=--ld-path={linker}')
        assert before == {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in source.rglob('*') if p.is_file()}
        return work, json.loads((work / 'report.json').read_text())

    def apply(work, label, *args):
        run('git', 'apply', '--check', work / 'cmake.patch', cwd=source)
        run('git', 'apply', work / 'cmake.patch', cwd=source)
        build = root / (label + '-build')
        run('cmake', '-S', source, '-B', build, '-G', 'Ninja', f'-DCMAKE_CXX_COMPILER={compiler}', *args)
        run('cmake', '--build', build, '--parallel', 2)
        run('ctest', '--test-dir', build, '--output-on-failure')
        return build

    # The same function and list serve several consumers. Keep compile usage,
    # including for the target deliberately left outside the selected experiment.
    loop = prefix + '''set(deps needed dead)
if(WITH_EXTRA)
  list(APPEND deps extra)
endif()
function(link_sample name)
  TARGET_LINK_LIBRARIES("${name}"
    PRIVATE ${deps}
  ) # keep comment
endfunction()
foreach(suffix IN ITEMS one two)
  add_executable(sample_${suffix} main.cpp)
  link_sample(sample_${suffix})
  add_test(NAME works_${suffix} COMMAND sample_${suffix})
  get_target_property(actual sample_${suffix} LINK_LIBRARIES)
  file(WRITE "${CMAKE_BINARY_DIR}/${suffix}.txt" "${actual}")
endforeach()
if(NOT "${deps}" MATCHES "^needed;dead")
  message(FATAL_ERROR "Shared variable changed")
endif()
target_compile_definitions(dead INTERFACE USAGE_REQUIREMENT=1)
'''
    work, result = invoke('selected-consumer', loop, '--target', 'sample_one')
    assert result['patch_validated'] and result['removed_links'] == 1
    assert result['selected_link']['compile_only']
    build = apply(work, 'selected-consumer')
    assert (build / 'one.txt').read_text() == 'needed;$<COMPILE_ONLY:dead>'
    assert (build / 'two.txt').read_text() == 'needed;dead'
    assert '# keep comment' in cmake.read_text()
    # A changed list falls back to the original command, including new entries.
    changed = root / 'changed-list'
    run('cmake', '-S', source, '-B', changed, '-G', 'Ninja', f'-DCMAKE_CXX_COMPILER={compiler}', '-DWITH_EXTRA=ON')
    assert (changed / 'one.txt').read_text() == 'needed;dead;extra'
    run('cmake', '--build', changed, '--parallel', 2)
    run('ctest', '--test-dir', changed, '--output-on-failure')
    # Exercise the compatibility fallback without requiring an old local CMake.
    cmake.write_text(cmake.read_text().replace('AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.27', 'AND FALSE'))
    legacy = root / 'legacy-version'
    run('cmake', '-S', source, '-B', legacy, '-G', 'Ninja', f'-DCMAKE_CXX_COMPILER={compiler}')
    assert (legacy / 'one.txt').read_text() == 'needed;dead'
    run('cmake', '--build', legacy, '--parallel', 2)
    run('ctest', '--test-dir', legacy, '--output-on-failure')

    work, result = invoke('all-consumers', loop)
    assert result['patch_validated'] and result['removed_links'] == 2
    assert result['dependency_search']['configurations'] == 2
    assert result['baseline_tests_passed'] == result['candidate_tests_passed'] == 2
    build = apply(work, 'all-consumers')
    assert all((build / f'{name}.txt').read_text() == 'needed;$<COMPILE_ONLY:dead>' for name in ['one', 'two'])

    # A list of forty entries remains a single batch, without editing its set().
    names = [f'lib{i}' for i in range(40)]
    many = prefix + ''.join(f'add_library({n} STATIC EXCLUDE_FROM_ALL unused.cpp)\n' for n in names)
    many += 'set(deps needed ' + ' '.join(names) + ')\n'
    many += '''set(program sample)
add_executable(${program} main.cpp)
target_link_libraries(${program} PRIVATE "${deps}")
add_test(NAME works COMMAND sample)
'''
    work, result = invoke('forty-variables', many)
    assert result['patch_validated'] and result['removed_links'] == 40
    assert result['dependency_search']['configurations'] == 1
    assert result['dependency_search']['profiled_links'] == 1
    build = apply(work, 'forty-variables')
    assert 'set(deps needed ' + ' '.join(names) + ')' in cmake.read_text()

    # Different dependencies in one variable share a source offset. A rejected
    # removal must not poison the cache entry for another library in that list.
    interaction = prefix + '''add_executable(sample main.cpp)
set(libs needed dead extra)
target_link_libraries(sample PRIVATE ${libs})
get_target_property(deps sample LINK_LIBRARIES)
if(NOT "dead" IN_LIST deps)
  target_compile_definitions(sample PRIVATE UNEXPECTED_CHANGE=1)
endif()
add_test(NAME works COMMAND sample)
'''
    work, result = invoke('independent-cache-entries', interaction)
    assert result['patch_validated'] and result['removed_links'] == 1
    assert result['selected_link']['dependency'] == 'extra'
    apply(work, 'independent-cache-entries')

    # Configuration keywords, generator expressions and PUBLIC lists stay intact.
    simple = prefix + '''add_executable(sample main.cpp)
set(deps CONTENTS)
target_link_libraries(sample PRIVATE ${deps})
add_test(NAME works COMMAND sample)
'''
    for label, value in [('generator', 'needed "$<1:dead>"'),
                         ('selector', 'needed optimized dead'),
                         ('public', 'needed PUBLIC dead')]:
        work, result = invoke(label, simple.replace('CONTENTS', value))
        assert not result['patch_validated'] and not (work / 'cmake.patch').exists()

    repeated = prefix + '''add_executable(sample main.cpp)
function(link_again)
  set(deps needed dead)
  target_link_libraries(sample PRIVATE ${deps})
endfunction()
link_again()
link_again()
add_test(NAME works COMMAND sample)
'''
    work, result = invoke('repeated-call', repeated)
    assert not result['patch_validated'] and not (work / 'cmake.patch').exists()

print('CMake variable checks passed: scoped lists, loops, forty removals and fallback configurations')
