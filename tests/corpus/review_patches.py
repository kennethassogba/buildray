"""One-off validation of the exact proposed upstream branches."""
import json
import re
import shutil
import statistics
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

project = sys.argv[1]
root = Path.cwd()
output = root / 'review-results'
output.mkdir(exist_ok=True)
sources = root / 'fixtures' / project
sequence = 0


def run(command, name):
    global sequence
    sequence += 1
    path = output / f'{sequence:03d}-{name}.log'
    start = time.monotonic()
    with path.open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    elapsed = time.monotonic() - start
    print(name, result.returncode, round(elapsed, 3), flush=True)
    if result.returncode:
        print(path.read_text()[-16000:], flush=True)
        raise RuntimeError(name)
    return elapsed, path


def configure(source, build, extra, compiler='21'):
    run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER=clang++-' + compiler,
         '-DCMAKE_C_COMPILER=clang-' + compiler,
         '-DCMAKE_EXE_LINKER_FLAGS=--ld-path=/usr/lib/llvm-21/bin/ld.lld',
         '-DCMAKE_SHARED_LINKER_FLAGS=--ld-path=/usr/lib/llvm-21/bin/ld.lld',
         *extra], 'configure-' + build.name)


def tests(build, name, extra=()):
    junit = output / (name + '.xml')
    run(['ctest', '--test-dir', str(build), '--parallel', '2',
         '--output-on-failure', '--output-junit', str(junit), *extra], name)
    document = ET.parse(junit).getroot()
    assert not list(document.iter('failure')) and not list(document.iter('error'))
    cases = sorted((case.get('name'), case.find('skipped') is not None)
                   for case in document.iter('testcase'))
    assert cases
    return cases


if project == 'highway':
    build = root / 'highway-build'
    configure(sources, build, ['-DHWY_ENABLE_TESTS=ON', '-DHWY_ENABLE_EXAMPLES=ON',
                              '-DHWY_ENABLE_CONTRIB=ON', '-DHWY_TEST_STANDALONE=OFF',
                              '-DBUILD_SHARED_LIBS=OFF'], '22')
    patch = (root / 'docs/evidence/highway-variable-dependencies.patch').read_text()
    targets = sorted(set(re.findall(r'^\+    target_link_libraries\((\w+) PRIVATE', patch, re.M)))
    assert len(targets) == 35
    run(['cmake', '--build', str(build), '--parallel', '4', '--target', *targets], 'build-35-programs')
    inventory = json.loads(subprocess.check_output(['ctest', '--test-dir', str(build),
                                                   '--show-only=json-v1'], text=True))
    names = [test['name'] for test in inventory['tests']
             if Path(test.get('command', [''])[0]).name in targets]
    assert names
    selection = output / 'selected-tests.txt'
    selection.write_text('\n'.join(names) + '\n')
    cases = tests(build, 'changed-programs', ['--tests-from-file', str(selection)])
    result = {'scope': '35 changed programs on current upstream base', 'targets': targets,
              'passed': sum(not skipped for _, skipped in cases),
              'skipped': [name for name, skipped in cases if skipped]}
else:
    variants = {}
    inventories = []
    for label, enabled in [('baseline', 'OFF'), ('candidate', 'ON')]:
        source = root / ('sentencepiece-' + label)
        shutil.copytree(sources, source, ignore=shutil.ignore_patterns('.git'))
        build = root / (label + '-build')
        configure(source, build, ['-DSPM_BUILD_TEST=ON', '-DSPM_ENABLE_SHARED=OFF',
                                 '-DSPM_ENABLE_TCMALLOC=OFF', '-DSPM_ENABLE_PCH=' + enabled])
        run(['cmake', '--build', str(build), '--parallel', '2'], 'full-' + label)
        inventories.append(tests(build, 'full-tests-' + label))
        variants[label] = (source, build)
    assert inventories[0] == inventories[1] and not any(skip for _, skip in inventories[0])
    result = {'scope': 'current upstream; static build; Clang 21; 2 build jobs',
              'tests': len(inventories[0]), 'scenarios': []}
    for probe in ['src/sentencepiece_processor.cc', 'src/common.h']:
        samples = {'baseline': [], 'candidate': []}
        for pair in range(3):
            for label in (['baseline', 'candidate'] if pair % 2 == 0 else ['candidate', 'baseline']):
                source, build = variants[label]
                _, idle = run(['ninja', '-C', str(build), '-n'], 'idle-' + label)
                assert 'no work to do' in idle.read_text()
                path = source / probe
                original = path.read_bytes()
                path.write_bytes(original + b'\n// Incremental rebuild probe.\n')
                elapsed, _ = run(['cmake', '--build', str(build), '--parallel', '2'], 'probe-' + label)
                samples[label].append(elapsed)
                assert tests(build, f'probe-tests-{len(result["scenarios"])}-{pair}-{label}') == inventories[0]
                path.write_bytes(original)
                run(['cmake', '--build', str(build), '--parallel', '2'], 'restore-' + label)
        medians = {key: statistics.median(values) for key, values in samples.items()}
        mad = {key: statistics.median(abs(value - medians[key]) for value in values)
               for key, values in samples.items()}
        threshold = max(.02 * medians['baseline'], 3 * max(mad.values()))
        delta = medians['candidate'] - medians['baseline']
        verdict = 'inconclusive' if abs(delta) <= threshold else ('improved' if delta < 0 else 'regressed')
        result['scenarios'].append({'file': probe, 'samples': samples, 'medians': medians,
                                    'mad': mad, 'noise_threshold': threshold, 'verdict': verdict})

(output / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2), flush=True)
