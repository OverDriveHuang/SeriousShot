"""Real process and CTest coverage of machine-local required input assembly."""
import argparse
import json
import os
from pathlib import Path
import shutil
import shlex
import io
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[1]
SCRIPTS = PROJECT / 'scripts'
sys.path.insert(0, str(SCRIPTS))
import test_fixture_inputs as inputs
import test_execution_policy as policy

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--cmake', default=shutil.which('cmake'))
parser.add_argument('--ctest', default=shutil.which('ctest'))
TOOLS, TEST_ARGS = parser.parse_known_args()


class FixtureInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.fixture = self.root / '中文 sample.png'
        self.fixture.write_bytes(b'input assembly test, not a PQ quality oracle')
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('HDRSHOT_')}
        self.env['PYTHONIOENCODING'] = 'utf-8'
        self.receipt = self.root / 'receipt'
        self.child = self.root / 'child.py'
        self.child.write_text('import os, pathlib, sys\np=pathlib.Path(os.environ["HDRSHOT_ULTRAHDR_FIXTURE"])\np.read_bytes()\nwith pathlib.Path(sys.argv[1]).open("a", encoding="utf-8") as f: f.write(str(p)+"\\n")\nsys.exit(int(sys.argv[2]))\n', encoding='utf-8')

    def run_wrapper(self, explicit=None, code=0):
        args = [sys.executable, str(SCRIPTS / 'run_uhdr_fixture_test.py'),
                '--project-root', str(self.root), '--build-dir', str(self.root / 'new-build')]
        if explicit is not None:
            args += ['--uhdr-fixture', str(explicit)]
        return subprocess.run(args + ['--', sys.executable, str(self.child), str(self.receipt), str(code)],
                              env=self.env, text=True, encoding='utf-8', capture_output=True)

    def register(self):
        return subprocess.run([sys.executable, str(SCRIPTS / 'test_fixture_inputs.py'), 'register',
                               '--project-root', str(self.root), '--uhdr-fixture', str(self.fixture)],
                              env=self.env, text=True, encoding='utf-8', capture_output=True)

    def test_unconfigured_actionable_then_register_and_same_wrapper_recovers(self):
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 1)
        self.assertIn('not configured', result.stderr)
        self.assertIn('register --uhdr-fixture', result.stderr)
        self.assertFalse(self.receipt.exists())
        recovery_text = result.stderr.split('with: ', 1)[1].split('. Registration records', 1)[0]
        if os.name == 'nt':
            shell = shutil.which('pwsh') or shutil.which('powershell')
            self.assertIsNotNone(shell)
            recovery_text = recovery_text.replace("'/path/to/PQ.png'",
                "'" + str(self.fixture).replace("'", "''") + "'")
            recovery_command = [shell, '-NoProfile', '-NonInteractive', '-Command',
                '[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); ' + recovery_text]
        else:
            recovery_command = shlex.split(recovery_text)
            recovery_command[-1] = str(self.fixture)
        recovered = subprocess.run(recovery_command, env=self.env, text=True, encoding='utf-8', capture_output=True)
        self.assertEqual(recovered.returncode, 0, recovered.stderr)
        self.assertEqual(self.run_wrapper().returncode, 0)
        self.assertEqual(self.receipt.read_text(encoding='utf-8').strip(), str(self.fixture))
        self.assertFalse((self.root / 'new-build/.verified-uhdr-fixture.json').exists())

    def test_bad_registry_explicit_override_and_register_repairs(self):
        registry = inputs.registry_path(self.root)
        registry.parent.mkdir()
        registry.write_bytes(b'\xffbroken')
        self.assertNotEqual(self.run_wrapper().returncode, 0)
        result = self.run_wrapper(self.fixture)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(registry.read_text(encoding='utf-8'))['uhdr_fixture']['path'], str(self.fixture))
        registry.write_text('{broken')
        result = self.register()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('location only', result.stdout)
        self.assertNotIn('fingerprint', registry.read_text(encoding='utf-8'))

    def test_selected_bad_explicit_or_env_does_not_fall_back(self):
        self.assertEqual(self.register().returncode, 0)
        missing = self.root / 'missing.png'
        result = self.run_wrapper(missing)
        self.assertEqual(result.returncode, 1)
        self.assertIn('file does not exist', result.stderr)
        self.assertFalse(self.receipt.exists())
        self.env['HDRSHOT_ULTRAHDR_FIXTURE'] = str(missing)
        self.assertNotEqual(self.run_wrapper().returncode, 0)
        self.assertFalse(self.receipt.exists())
        self.assertEqual(self.run_wrapper(self.fixture).returncode, 0)

    def test_nonempty_environment_bypasses_unselected_bad_registry(self):
        registry = inputs.registry_path(self.root)
        registry.parent.mkdir()
        registry.write_text('{broken')
        self.env['HDRSHOT_ULTRAHDR_FIXTURE'] = str(self.fixture)
        self.assertEqual(self.run_wrapper().returncode, 0)

    def test_native_failure_preserved_and_registration_failure_separate(self):
        registry = inputs.registry_path(self.root)
        registry.mkdir(parents=True)  # explicit input bypasses this unusable destination
        result = self.run_wrapper(self.fixture, 7)
        self.assertEqual(result.returncode, 7)
        self.assertIn('stage=native native_exit=7', result.stderr)
        self.assertNotIn('stage=registration', result.stderr)
        result = self.run_wrapper(self.fixture)
        self.assertEqual(result.returncode, 1)
        self.assertIn('stage=registration native_exit=0', result.stderr)
        self.assertTrue(registry.is_dir())

    def test_failed_atomic_registration_preserves_existing_configuration(self):
        inputs.register_uhdr_fixture(self.root, self.fixture)
        registry = inputs.registry_path(self.root)
        original = registry.read_bytes()
        other = self.root / 'other.png'
        other.write_bytes(b'other')
        with patch.object(Path, 'replace', side_effect=PermissionError('injected atomic replace denied')):
            with self.assertRaisesRegex(inputs.FixtureInputError, 'write failed'):
                inputs.register_uhdr_fixture(self.root, other)
        self.assertEqual(registry.read_bytes(), original)
        self.assertEqual(list(registry.parent.iterdir()), [registry])

    def test_cleanup_error_does_not_replace_registration_error(self):
        inputs.register_uhdr_fixture(self.root, self.fixture)
        registry = inputs.registry_path(self.root)
        original = registry.read_bytes()
        other = self.root / 'second.png'
        other.write_bytes(b'other')
        diagnostic = io.StringIO()
        with patch.object(Path, 'replace', side_effect=PermissionError('primary replace denied')), \
                patch.object(Path, 'unlink', side_effect=PermissionError('secondary cleanup denied')), \
                patch.object(sys, 'stderr', diagnostic):
            with self.assertRaisesRegex(inputs.FixtureInputError, 'write failed.*primary replace denied'):
                inputs.register_uhdr_fixture(self.root, other)
        self.assertIn('secondary cleanup denied', diagnostic.getvalue())
        self.assertEqual(registry.read_bytes(), original)

    def test_unreadable_and_nonfile_classification(self):
        with patch.object(Path, 'open', side_effect=PermissionError(13, 'Permission denied')):
            with self.assertRaisesRegex(inputs.FixtureInputError, 'unreadable.*Permission denied'):
                inputs.resolve_uhdr_fixture(self.root, self.fixture, environ={})
        with self.assertRaisesRegex(inputs.FixtureInputError, 'not a regular file'):
            inputs.resolve_uhdr_fixture(self.root, self.root, environ={})
        if os.name != 'nt':
            self.fixture.chmod(0)
            try:
                result = self.run_wrapper(self.fixture)
                if os.geteuid() == 0:
                    self.assertEqual(result.returncode, 0)  # root bypasses POSIX mode bits
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('unreadable', result.stderr)
            finally:
                self.fixture.chmod(0o600)

    def test_legacy_migration_and_new_tree_has_no_verified_result(self):
        tree = self.root / 'old-build'
        tree.mkdir()
        (tree / '.verified-uhdr-fixture.json').write_text(json.dumps({'path': str(self.fixture), 'fingerprint': 'old'}))
        with patch.dict(os.environ, self.env, clear=True):
            selected = policy.select_fixture(self.root, tree)
            self.assertFalse(selected['reuse'])
            self.assertEqual(selected['source'], 'legacy build registration')
            inputs.register_uhdr_fixture(self.root, selected['path'])
            self.assertFalse(policy.select_fixture(self.root, self.root / 'fresh')['reuse'])
            self.assertIsNone(policy.select(self.root, self.root / 'windows', ['windows_test'])['uhdr_fixture'])

    def test_real_ctest_clean_environment_two_new_build_trees_and_always_runs(self):
        cmake = TOOLS.cmake
        ctest = TOOLS.ctest
        self.assertIsNotNone(cmake)
        self.assertIsNotNone(ctest)
        self.assertEqual(self.register().returncode, 0)
        source = self.root / 'mini'
        source.mkdir()
        command = [sys.executable, str(SCRIPTS / 'run_uhdr_fixture_test.py'), '--project-root', str(self.root),
                   '--build-dir', '${CMAKE_BINARY_DIR}', '--', sys.executable, str(self.child), str(self.receipt), '0']
        quoted = ' '.join('"' + part.replace('\\', '/') + '"' for part in command)
        (source / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(InputAssembly NONE)\nenable_testing()\nadd_test(NAME fixture COMMAND ' + quoted + ')\n', encoding='utf-8')
        for name in ('fresh-a', 'fresh-b'):
            tree = self.root / name
            configured = subprocess.run([cmake, '-S', str(source), '-B', str(tree)], env=self.env, capture_output=True, text=True, encoding='utf-8')
            self.assertEqual(configured.returncode, 0, configured.stderr)
            (tree / '.verified-uhdr-fixture.json').write_text(json.dumps({'path': str(self.fixture), 'fingerprint': 'success-ledger-does-not-skip-wrapper'}))
            for _ in range(2):
                tested = subprocess.run([ctest, '--test-dir', str(tree), '-C', 'Release', '--output-on-failure'], env=self.env, capture_output=True, text=True, encoding='utf-8')
                self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
        self.assertEqual(len(self.receipt.read_text(encoding='utf-8').splitlines()), 4)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *TEST_ARGS])
