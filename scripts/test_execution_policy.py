"""Conservative cross-platform regression selection; updater and fixed-image codec results can be reused."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import xml.etree.ElementTree as ET

from test_fixture_inputs import register_uhdr_fixture, resolve_uhdr_fixture

OPTIONAL_TESTS = set()
UHDR_FIXTURE_TEST = 'hdrshot_macos_ultra_hdr_fixture_tests'
UPDATE_TESTS = {'hdrshot_update_tests', 'hdrshot_update_runtime_tests',
                'hdrshot_update_storage_retry_tests', 'hdrshot_windows_update_move_retry_tests'}
# Positive independence list, NOT a list of all possible updater dependencies.
# Unknown new code is included in the updater fingerprint by default.
INDEPENDENT = ('src/domain/analysis/', 'src/domain/color/', 'src/domain/png/',
               'src/domain/annotation/', 'src/ui/qt/pinned_screenshot',
               'src/ui/qt/analyzer_', 'src/ui/qt/source_presentation',
               'src/platform/macos/macos_qt_pinned_screenshot',
               'src/platform/windows/windows_qt_pinned_screenshot')


def state_path(tree):
    return tree / '.verified-update-input.json'


def fingerprint(project, tree, update_tests=()):
    try:
        digest = hashlib.sha256()
        digest.update((platform.system() + platform.release() + platform.version() + platform.machine()).encode())
        files = []
        for name in ('src', 'tests', 'scripts', 'cmake', 'tools', 'assets', 'publishing'):
            folder = project / name
            if folder.exists():
                files.extend(f for f in folder.rglob('*') if f.is_file() and
                             '__pycache__' not in f.parts and f.suffix not in ('.md', '.pyc'))
        files.extend(f for f in project.iterdir() if f.is_file() and
                     (f.name.startswith('CMake') or f.suffix in ('.cmake', '.command', '.ps1', '.json')))
        if not (project / 'src').is_dir() or not (tree / 'CMakeCache.txt').is_file():
            return None
        for file in sorted(set(files)):
            relative = file.relative_to(project).as_posix()
            if relative.startswith(INDEPENDENT):
                continue
            digest.update(relative.encode() + b'\0' + file.read_bytes() + b'\0')
        config = (tree / 'CMakeCache.txt').read_text(encoding='utf-8')
        digest.update(config.encode())
        # Verify the actual linked test programs, including static dependencies.
        for name in update_tests:
            candidates = [tree / name, tree / (name + '.exe'), tree / 'Release' / (name + '.exe')]
            executable = next((f for f in candidates if f.is_file()), None)
            if executable is None:
                return None
            digest.update(executable.read_bytes())
        # Qt is dynamically linked: an in-place runtime update is not a source diff.
        match = re.search(r'^Qt6_DIR:[^=]+=(.+)$', config, re.MULTILINE)
        if match:
            qt = Path(match.group(1).strip()).parents[2]
            for module in ('Core', 'Gui', 'Widgets', 'Network'):
                candidates = [qt / 'lib' / f'Qt{module}.framework/Versions/A/Qt{module}',
                              qt / 'bin' / f'Qt6{module}.dll']
                runtime = next((f for f in candidates if f.is_file()), None)
                if runtime is None:
                    return None
                digest.update(runtime.read_bytes())
        # Configuration environment is not necessarily represented by a source diff.
        for key, value in sorted(os.environ.items()):
            if key.startswith(('HDRSHOT_', 'QT_')) or key in ('CC', 'CXX', 'SDKROOT', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS'):
                digest.update(f'{key}={value}\0'.encode())
        return digest.hexdigest()
    except (OSError, ValueError):
        return None


def fixture_state_path(tree):
    return tree / '.verified-uhdr-fixture.json'


def fixture_fingerprint(project, tree, path):
    """The built test statically links libuhdr and the production codec adapter."""
    try:
        digest = hashlib.sha256()
        digest.update((platform.system() + platform.release() + platform.version() + platform.machine()).encode())
        files = [tree / UHDR_FIXTURE_TEST, tree / 'CMakeCache.txt',
                 project / 'CMakeLists.txt', Path(path)]
        files.extend(project / 'scripts' / name for name in
                     ('test_fixture_inputs.py', 'run_uhdr_fixture_test.py')
                     if (project / 'scripts' / name).is_file())
        files.extend(sorted((project / 'cmake').glob('libultrahdr*.cmake')))
        for file in files:
            digest.update(str(file.resolve()).encode() + b'\0' + file.read_bytes() + b'\0')
        return digest.hexdigest()
    except (OSError, ValueError, TypeError):
        return None


def select_fixture(project, tree, force=False, path=None):
    try:
        previous = json.loads(fixture_state_path(tree).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        previous = {}
    if not isinstance(previous, dict):
        previous = {}
    explicit_path = path is not None
    resolved = resolve_uhdr_fixture(project, path, tree)
    path = resolved['path']
    value = fixture_fingerprint(project, tree, path)
    reuse = not (force or explicit_path) and value is not None and previous.get('fingerprint') == value
    return {'fingerprint': value, 'path': path, 'source': resolved['source'], 'reuse': reuse,
            'reason': 'verified identical codec, system and image' if reuse else
                      'manual request, changed inputs or no verified fixture result'}


def remember_fixture(project, tree, plan, passed):
    fixture = plan.get('uhdr_fixture')
    if not fixture or fixture['reuse']:
        return
    verified = (UHDR_FIXTURE_TEST in passed and fixture['fingerprint'] and
                fixture_fingerprint(project, tree, fixture['path']) == fixture['fingerprint'])
    if verified:
        register_uhdr_fixture(project, fixture['path'])
    # A failed forced rerun invalidates prior success, but retains the local path.
    path = fixture_state_path(tree)
    if not tree.is_dir():
        return
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps({'fingerprint': fixture['fingerprint'] if verified else None,
                                    'path': fixture['path']}) + '\n')
    temporary.replace(path)



def select(project, tree, names, force=False, force_uhdr_fixture=False, uhdr_fixture=None):
    update = sorted(set(names) & UPDATE_TESTS)
    value = fingerprint(project, tree, update)
    try:
        previous = json.loads(state_path(tree).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        previous = {}
    if not isinstance(previous, dict):
        previous = {}
    reuse = not force and value is not None and previous.get('fingerprint') == value and previous.get('tests') == update
    omitted = list(update) if reuse else []
    fixture = select_fixture(project, tree, force_uhdr_fixture, uhdr_fixture) if UHDR_FIXTURE_TEST in names else None
    if fixture and fixture['reuse']:
        omitted.append(UHDR_FIXTURE_TEST)
    return {'uhdr_fixture': fixture, 'fingerprint': value, 'update_tests': update, 'omitted': omitted,
            'selected': [n for n in names if n not in omitted],
            'reason': 'verified identical updater inputs' if reuse else 'new, changed, unknown or explicitly required inputs'}


def remember(project, tree, plan, passed, complete_run):
    remember_fixture(project, tree, plan, passed)
    if (not complete_run or not plan['fingerprint'] or set(plan['omitted']) & UPDATE_TESTS or
            not plan['update_tests'] or not set(plan['update_tests']).issubset(passed) or
            fingerprint(project, tree, plan['update_tests']) != plan['fingerprint']):
        return
    path = state_path(tree)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps({'fingerprint': plan['fingerprint'], 'tests': plan['update_tests']}) + '\n')
    temporary.replace(path)


def exclusion_arguments(plan):
    return ['-E', '^(' + '|'.join(re.escape(n) for n in plan['omitted']) + ')$'] if plan['omitted'] else []


def summarize(xml, expected):
    if not expected or len(expected) != len(set(expected)):
        raise ValueError('No tests registered, or duplicate CTest names')
    result = {key: [] for key in ('passed', 'failed', 'required_skipped', 'optional_skipped',
                                  'not_run', 'partial_not_run', 'limitations', 'unexpected')}
    result['performance'] = []
    seen = set()
    for case in ET.fromstring(xml).iter('testcase'):
        name = case.get('name')
        if name not in expected or name in seen:
            result['unexpected'].append(name)
            continue
        seen.add(name)
        if case.find('failure') is not None or case.find('error') is not None:
            result['failed'].append(name)
        elif case.find('skipped') is not None or case.get('status') in ('notrun', 'disabled'):
            key = 'optional_skipped' if name in OPTIONAL_TESTS else 'required_skipped'
            result[key].append(name)
        else:
            result['passed'].append(name)
        # Existing native targets sometimes run only a subset. Retain their
        # explicit evidence limits even when CTest marks the executable passed.
        output = case.findtext('system-out', '')
        performance_count = len(result['performance'])
        for line in output.splitlines():
            if name.startswith('hdrshot_perf_') and line.startswith('PERFORMANCE_REPORT '):
                try:
                    result['performance'].append(json.loads(line[len('PERFORMANCE_REPORT '):]))
                except ValueError:
                    result['partial_not_run'].append(name)
        if ('test output was removed' in output or
                (name.startswith('hdrshot_perf_') and name in result['passed'] and
                 len(result['performance']) == performance_count)):
            result['partial_not_run'].append(name)
        notes = [line for line in output.splitlines()
                 if '[NOT VERIFIED]' in line or '[SKIP]' in line or 'GPU case not run' in line]
        if notes:
            result['limitations'].append({'test': name, 'messages': notes})
        if name in result['passed'] and any('[SKIP]' in line or 'GPU case not run' in line for line in notes):
            result['partial_not_run'].append(name)
    result['partial_not_run'] = sorted(set(result['partial_not_run']))
    result['not_run'] = sorted(set(expected) - seen)
    return result


def complete(result):
    return not any(result[key] for key in ('failed', 'required_skipped', 'not_run', 'partial_not_run', 'unexpected'))



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--logs', type=Path, required=True)
    parser.add_argument('--ctest', default='ctest')
    parser.add_argument('--force-updates', action='store_true')
    parser.add_argument('--force-uhdr-fixture', action='store_true')
    parser.add_argument('--uhdr-fixture', type=Path, help='Run the fixed-image codec test with this image and remember its local path after success')
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    tree = args.build_dir.resolve()
    args.logs.mkdir(parents=True, exist_ok=True)
    report = {'status': 'failed'}
    try:
        listing = subprocess.check_output([args.ctest, '--test-dir', str(tree), '--show-only=json-v1'], text=True, encoding='utf-8')
        names = [t['name'] for t in json.loads(listing)['tests']]
        plan = select(project, tree, names, args.force_updates, args.force_uhdr_fixture, args.uhdr_fixture)
        report['selection'] = plan
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
        if plan['uhdr_fixture'] and plan['uhdr_fixture']['path']:
            env['HDRSHOT_ULTRAHDR_FIXTURE'] = plan['uhdr_fixture']['path']
        xml = args.logs.resolve() / 'ctest.xml'
        with (args.logs / 'ctest.log').open('w') as log:
            tested = subprocess.run([args.ctest, '--test-dir', str(tree), '-j', '1', '--output-on-failure',
                                     '--no-tests=error', '--test-output-size-passed', '10485760',
                              '--test-output-size-failed', '10485760', '--output-junit', str(xml), *exclusion_arguments(plan)],
                                    stdout=log, stderr=subprocess.STDOUT,
                                    env=env)
        result = summarize(xml.read_text(encoding='utf-8'), plan['selected'])
        report.update(result)
        code = 1 if tested.returncode or result['failed'] else (0 if complete(result) else 2)
        report['status'] = 'failed' if code == 1 else 'incomplete' if code == 2 else 'passed_with_limits' if result['limitations'] or result['optional_skipped'] else 'passed'
        remember(project, tree, plan, set(result['passed']) - set(result['partial_not_run']), code == 0)
        return code
    except (OSError, ValueError, KeyError, ET.ParseError, subprocess.CalledProcessError) as error:
        report['error'] = str(error)
        return 1
    finally:
        (args.logs / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    raise SystemExit(main())
