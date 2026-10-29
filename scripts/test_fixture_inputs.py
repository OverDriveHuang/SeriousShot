"""Machine-local fixture locations, deliberately separate from verified test results."""
import argparse
import json
import os
import shlex
import sys
from pathlib import Path
import tempfile


class FixtureInputError(ValueError):
    pass


def registry_path(project):
    return Path(project) / '.agent' / 'test-fixtures.json'


def recovery(project):
    args = [Path(sys.executable).as_posix(), Path(__file__).resolve().as_posix(),
            '--project-root', Path(project).resolve().as_posix(), 'register',
            '--uhdr-fixture', '/path/to/PQ.png']
    if os.name == 'nt':
        # PowerShell needs the call operator for a quoted executable path.
        quoted = lambda value: "'" + value.replace("'", "''") + "'"
        command = (f'& {quoted(args[0])} {quoted(args[1])} --project-root {quoted(args[3])} '
                   f'register --uhdr-fixture {quoted(args[6])}')
    else:
        command = shlex.join(args)
    return (f'Register a readable PQ PNG with: {command}. Registration records location only; '
            'then rerun the original CTest command for the failed required-fixture test.')


def readable_path(project, value, source):
    if not isinstance(value, str) or not value.strip():
        raise FixtureInputError(f'fixture input invalid ({source}): empty path. {recovery(project)}')
    path = Path(value).expanduser().resolve()
    try:
        if not path.is_file():
            reason = 'file does not exist' if not path.exists() else 'not a regular file'
            raise FixtureInputError(f'fixture input {reason} ({source}): {path}. {recovery(project)}')
        with path.open('rb') as stream:
            stream.read(1)
    except OSError as error:
        raise FixtureInputError(f'fixture input unreadable ({source}): {path}: {error}. {recovery(project)}') from error
    return {'path': str(path), 'source': source}


def resolve_uhdr_fixture(project, explicit_path=None, legacy_tree=None, environ=None):
    env = os.environ if environ is None else environ
    if explicit_path is not None:
        return readable_path(project, str(explicit_path), 'explicit --uhdr-fixture')
    if env.get('HDRSHOT_ULTRAHDR_FIXTURE'):
        return readable_path(project, env['HDRSHOT_ULTRAHDR_FIXTURE'], 'HDRSHOT_ULTRAHDR_FIXTURE')
    registry = registry_path(project)
    try:
        text = registry.read_text(encoding='utf-8')
    except FileNotFoundError:
        text = None
    except (OSError, UnicodeError) as error:
        raise FixtureInputError(f'fixture registration unreadable: {registry}: {error}. {recovery(project)}') from error
    if text is not None:
        try:
            data = json.loads(text)
            if data['schema'] != 1:
                raise ValueError('unsupported schema')
            value = data['uhdr_fixture']['path']
        except (ValueError, KeyError, TypeError) as error:
            raise FixtureInputError(f'fixture registration invalid: {registry}: {error}. {recovery(project)}') from error
        return readable_path(project, value, f'local registration {registry}')
    if legacy_tree is not None:
        try:
            data = json.loads((Path(legacy_tree) / '.verified-uhdr-fixture.json').read_text(encoding='utf-8'))
            value = data.get('path') if isinstance(data, dict) else None
        except (OSError, ValueError):
            value = None
        if isinstance(value, str) and value:
            return readable_path(project, value, 'legacy build registration')
    raise FixtureInputError(f'fixture input not configured (no local registration or explicit input). {recovery(project)}')


def register_uhdr_fixture(project, path):
    resolved = readable_path(project, str(path), 'registration')
    registry = registry_path(project)
    content = json.dumps({'schema': 1, 'uhdr_fixture': {'path': resolved['path']}}, ensure_ascii=False, indent=2) + '\n'
    temporary = None
    try:
        registry.parent.mkdir(parents=True, exist_ok=True)
        if registry.is_file() and registry.read_bytes() == content.encode('utf-8'):
            return resolved
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=registry.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        temporary.replace(registry)
    except (OSError, UnicodeError) as error:
        raise FixtureInputError(f'fixture registration write failed: {registry}: {error}') from error
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError as cleanup_error:
                print(f'fixture temporary cleanup failed: {temporary}: {cleanup_error}; '
                      'primary registration result preserved', file=sys.stderr)
    return resolved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['register'])
    parser.add_argument('--project-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--uhdr-fixture', required=True)
    args = parser.parse_args()
    try:
        result = register_uhdr_fixture(args.project_root, args.uhdr_fixture)
        print(f'Registered/repaired fixture location: {result["path"]}; location only, NOT a verified test result.')
        return 0
    except FixtureInputError as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
