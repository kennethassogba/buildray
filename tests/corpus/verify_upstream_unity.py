"""Check the measured groups against the real XGrammar PR's CMake option."""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--artifacts', type=Path, required=True)
parser.add_argument('--levels', required=True)
args = parser.parse_args()
source, build = args.source.resolve(), args.build.resolve()
database = json.loads((build / 'compile_commands.json').read_text())
actual = {}
for entry in database:
    path = Path(entry['file'])
    if path.parent.name != 'Unity':
        continue
    relative = path.relative_to(build).as_posix()
    assert relative.startswith('CMakeFiles/xgrammar.dir/Unity/'), relative
    files = []
    for line in path.read_text().splitlines():
        if line.startswith('#include "') and line.endswith('"'):
            included = Path(line[10:-1]).relative_to(source).as_posix()
            files.append('<SOURCE>/' + included)
    assert files and relative not in actual
    actual['<BUILD>/' + relative] = files
assert len(actual) == 8, actual
tests = ET.parse(args.artifacts / 'upstream-pr995-tests.xml').getroot()
cases = list(tests.iter('testcase'))
assert len(cases) == 83 and not list(tests.iter('failure')) and not list(tests.iter('skipped'))
for jobs in json.loads(args.levels):
    plan = json.loads((args.artifacts / f'jobs-{jobs}/unity.json').read_text())
    expected = {g['file']: g['sources'] for g in plan['groups']}
    assert expected == actual, (jobs, expected, actual)
result = {'upstream_revision': 'f95f769b2eca1ca10a2820ff6566eca20efddc23',
          'base_revision': '7e269d56c74c89de52bc8c31e42191155416371f',
          'option': 'XGRAMMAR_ENABLE_UNITY_BUILD=ON', 'batch_size': 4,
          'levels': json.loads(args.levels), 'groups_match': True,
          'upstream_tests_passed': len(cases), 'groups': actual}
(args.artifacts / 'upstream-equivalence.json').write_text(json.dumps(result, indent=2) + '\n')
print('Upstream PR #995: all eight measured groups match; 83 independent tests pass')
