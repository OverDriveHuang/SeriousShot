#!/usr/bin/env python3
"""Build and test one macOS tree, optionally continue to the private DMG packager."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid
import xml.etree.ElementTree as ET
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_execution_policy import summarize, complete, select, remember, exclusion_arguments

PROJECT = Path(__file__).resolve().parents[1]

def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=PROJECT / 'build-qt')
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--force-updates', action='store_true', help='Run updater tests even when identical inputs previously passed')
    parser.add_argument('--force-uhdr-fixture', action='store_true', help='Run the fixed-image libuhdr error test even when unchanged')
    parser.add_argument('--uhdr-fixture', type=Path, help='Run the libuhdr error test with this image; remember the local path after success')
    parser.add_argument('--package', action='store_true', help='After tests, sign/notarize/package a DMG; requires separate release authorization')
    parser.add_argument('--work-dir', type=Path, help='Explicit new private packaging staging directory')
    parser.add_argument('--output-dir', type=Path, help='Explicit task deliverables directory')
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.package and (not args.work_dir or not args.output_dir):
        parser.error('--package requires --work-dir and --output-dir')
    if not args.package and (args.work_dir or args.output_dir):
        parser.error('Packaging directories require --package')
    return args


def pipeline(args, logs):
    logs.mkdir(parents=True, exist_ok=False)
    tree = args.build_dir.resolve()
    cmake = str(PROJECT / '.build-tools/cmake-venv/bin/cmake')
    ctest = str(PROJECT / '.build-tools/cmake-venv/bin/ctest')
    report = {'build_dir': str(tree), 'status': 'failed', 'stage': 'configure'}
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')

    def run(stage, command, capture=False):
        report['stage'] = stage
        print(f'[{stage}] ' + ' '.join(map(str, command)), flush=True)
        with (logs / f'{stage}.log').open('w') as stream:
            completed = subprocess.run(command, cwd=PROJECT, env=env,
                                       stdout=subprocess.PIPE if capture else stream,
                                       stderr=stream if capture else subprocess.STDOUT,
                                       text=True)
            if capture:
                stream.write(completed.stdout or '')
        return completed

    try:
        for stage, command in (
            ('configure', [cmake, '--preset', 'mac-qt-release', '-B', str(tree), '-DBUILD_TESTING=ON']),
            ('build', [cmake, '--build', str(tree), '-j', str(args.jobs)]),
        ):
            if run(stage, command).returncode:
                return 1
        listing = run('inventory', [ctest, '--test-dir', str(tree), '--show-only=json-v1'], True)
        if listing.returncode:
            return 1
        expected = [item['name'] for item in json.loads(listing.stdout)['tests']]
        plan = select(PROJECT, tree, expected, args.force_updates or args.package, args.force_uhdr_fixture, args.uhdr_fixture)
        report['selection'] = plan
        if plan['uhdr_fixture'] and plan['uhdr_fixture']['path']:
            env['HDRSHOT_ULTRAHDR_FIXTURE'] = plan['uhdr_fixture']['path']
        junit = logs / 'ctest.xml'
        # Serial by default: native tests may share focus, WindowServer and input.
        tested = run('test', [ctest, '--test-dir', str(tree), '-j', '1', '--output-on-failure',
                              '--no-tests=error', '--test-output-size-passed', '10485760',
                              '--test-output-size-failed', '10485760', '--output-junit', str(junit), *exclusion_arguments(plan)])
        result = summarize(junit.read_text(), plan['selected'])
        report.update(result, registered=len(expected))
        remember(PROJECT, tree, plan, set(result['passed']) - set(result['partial_not_run']),
                 tested.returncode == 0 and complete(result))
        if tested.returncode:
            return 1
        if not complete(result):
            report['status'] = 'incomplete'
            return 2
        report['status'] = 'passed_with_limits' if result['optional_skipped'] or result['limitations'] else 'passed'
        if args.package:
            packager = PROJECT / 'publishing/package_macos.py'
            if not packager.is_file():
                raise ValueError('Private DMG packager is unavailable in this checkout')
            packaged = run('package', [sys.executable, str(packager), '--app', str(tree / 'SeriousShot.app'),
                                       '--work-dir', str(args.work_dir.resolve()), '--output-dir', str(args.output_dir.resolve())])
            if packaged.returncode:
                report['status'] = 'failed'
                return 1
        report['stage'] = 'complete'
        return 0
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        report['error'] = str(error)
        report['status'] = 'failed'
        return 1
    finally:
        (logs / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        print(f'Status: {report["status"]}; stage: {report["stage"]}')
        for key in ('passed', 'failed', 'required_skipped', 'optional_skipped', 'not_run', 'partial_not_run'):
            if key in report:
                print(f'{key}: {len(report[key])}' + (f' {report[key]}' if key != 'passed' and report[key] else ''))
        for item in report.get('limitations', []):
            print(f'LIMITATION {item["test"]}: ' + ' | '.join(item['messages']))
        if report.get('performance'):
            first = sum(p['comparison'] == 'first_measurement' for p in report['performance'])
            print(f"Performance measurements: {len(report['performance'])}; first baselines: {first}; review deltas in summary.json")
        print(f'Logs: {logs}')


def main():
    args = arguments()
    if sys.platform != 'darwin':
        raise SystemExit('This build entry requires macOS')
    # Do not launch, quit, re-sign or replace any resident user application.
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    logs = PROJECT / '.agent/logs' / f'macos-{stamp}-{uuid.uuid4().hex[:6]}'
    return pipeline(args, logs)


if __name__ == '__main__':
    raise SystemExit(main())
