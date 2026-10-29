"""Assemble required fixture input for every direct CTest execution; never skip."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

from test_fixture_inputs import FixtureInputError, register_uhdr_fixture, resolve_uhdr_fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-root', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--uhdr-fixture')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('native test command is required after --')
    try:
        fixture = resolve_uhdr_fixture(args.project_root, args.uhdr_fixture, args.build_dir)
    except FixtureInputError as error:
        print(f'stage=input: {error}', file=sys.stderr)
        return 1
    env = dict(os.environ, HDRSHOT_ULTRAHDR_FIXTURE=fixture['path'])
    print(f'fixture source={fixture["source"]} path={fixture["path"]}', flush=True)
    try:
        result = subprocess.run(command, env=env)
    except OSError as error:
        print(f'stage=launch: {error}', file=sys.stderr)
        return 1
    if result.returncode:
        print(f'stage=native native_exit={result.returncode}; location was not registered', file=sys.stderr)
        return result.returncode if result.returncode > 0 else 128 - result.returncode
    try:
        register_uhdr_fixture(args.project_root, fixture['path'])
    except FixtureInputError as error:
        print(f'stage=registration native_exit=0: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
