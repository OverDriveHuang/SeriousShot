"""Verify the build gate with an isolated fake toolchain, never sign or launch apps."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/build-macos.py'
spec = importlib.util.spec_from_file_location('macos_build', SCRIPT)
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        (self.root / "publishing").mkdir()
        (self.root / "publishing/package_macos.py").touch()
        self.calls = []
        self.call_environments = []
        self.test_xml = '<testsuite><testcase name="real"/></testsuite>'
        self.test_names = ['real']
        self.fail_stage = None

    def run_tool(self, command, **kwargs):
        self.calls.append([str(x) for x in command])
        self.call_environments.append(dict(kwargs.get("env", {})))
        if '--show-only=json-v1' in command:
            return subprocess.CompletedProcess(command, 0, json.dumps({'tests': [{'name': x} for x in self.test_names]}))
        if '--output-junit' in command:
            Path(command[command.index('--output-junit') + 1]).write_text(self.test_xml)
        code = 1 if self.fail_stage and self.fail_stage in command else 0
        return subprocess.CompletedProcess(command, code)

    def pipeline(self, package=False):
        args = build.arguments(['--build-dir', str(self.root / 'build')] +
            (['--package', '--work-dir', str(self.root / 'stage'), '--output-dir', str(self.root / 'out')] if package else []))
        clean_env = {key: value for key, value in os.environ.items() if not key.startswith('HDRSHOT_')}
        with patch.dict(os.environ, clean_env, clear=True), \
                patch.object(build, 'PROJECT', self.root), patch.object(build.subprocess, 'run', self.run_tool):
            code = build.pipeline(args, self.root / 'logs')
        report = json.loads((self.root / 'logs/summary.json').read_text())
        return code, report

    def test_build_test_then_package_exact_same_directory(self):
        code, report = self.pipeline(True)
        self.assertEqual(code, 0)
        self.assertEqual(report['passed'], ['real'])
        self.assertIn('--build', self.calls[1])
        self.assertIn('--output-junit', self.calls[3])
        self.assertEqual(self.calls[-1][1], str(self.root / 'publishing/package_macos.py'))
        self.assertIn(str(self.root / 'build'), self.calls[1])
        self.assertIn(str(self.root / 'build'), self.calls[3])
        self.assertIn('-DBUILD_TESTING=ON', self.calls[0])

    def test_build_failure_stops_before_testing_or_packaging(self):
        self.fail_stage = '--build'
        code, report = self.pipeline(True)
        self.assertEqual(code, 1)
        self.assertEqual(report['stage'], 'build')
        self.assertEqual(len(self.calls), 2)

    def test_failed_test_blocks_package(self):
        self.test_xml = '<testsuite><testcase name="real"><failure message="broken"/></testcase></testsuite>'
        self.fail_stage = '--output-junit'
        code, report = self.pipeline(True)
        self.assertEqual(code, 1)
        self.assertEqual(report['failed'], ['real'])
        self.assertEqual(len(self.calls), 4)

    def test_native_skip_is_not_success(self):
        self.test_xml = '<testsuite><testcase name="real"><skipped message="locked"/></testcase></testsuite>'
        code, report = self.pipeline(True)
        self.assertEqual(code, 2)
        self.assertEqual(report['required_skipped'], ['real'])
        self.assertEqual(len(self.calls), 4)

    def test_selected_external_fixture_missing_input_fails_before_test(self):
        name = 'hdrshot_macos_ultra_hdr_fixture_tests'
        self.test_names.append(name)
        self.test_xml = f'<testsuite><testcase name="real"/><testcase name="{name}"><skipped/></testcase></testsuite>'
        code, report = self.pipeline()
        self.assertEqual(code, 1)
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['stage'], 'inventory')
        self.assertIn('not configured', report['error'])
        self.assertIn('register --uhdr-fixture', report['error'])
        self.assertEqual(len(self.calls), 3)  # no CTest execution or package

    def test_registered_fixture_is_assembled_for_build_entry_with_clean_environment(self):
        from test_fixture_inputs import register_uhdr_fixture, registry_path
        fixture = self.root / '中文 PQ.png'
        fixture.write_bytes(b'assembly input, not a PQ content oracle')
        register_uhdr_fixture(self.root, fixture)
        before = registry_path(self.root).read_bytes()
        name = 'hdrshot_macos_ultra_hdr_fixture_tests'
        self.test_names.append(name)
        self.test_xml = f'<testsuite><testcase name="real"/><testcase name="{name}"/></testsuite>'
        code, report = self.pipeline()
        self.assertEqual(code, 0)
        self.assertIn(name, report['passed'])
        self.assertEqual(self.call_environments[3]['HDRSHOT_ULTRAHDR_FIXTURE'], str(fixture))
        self.assertEqual(registry_path(self.root).read_bytes(), before)
        self.assertIn(name, report['selection']['selected'])

    def test_empty_or_missing_results_fail_closed(self):
        for xml in ['<testsuite/>', '<testsuite><testcase name="unexpected"/></testsuite>']:
            with self.subTest(xml=xml):
                result = build.summarize(xml, ['real'])
                self.assertEqual(result['not_run'], ['real'])
                self.assertFalse(build.complete(result))
        with self.assertRaises(ValueError):
            build.summarize('<testsuite/>', [])

    def test_partial_gpu_skip_and_visual_limits_are_retained(self):
        result = build.summarize('<testsuite><testcase name="real"><system-out>Metal unavailable: GPU case not run</system-out></testcase></testsuite>', ['real'])
        self.assertEqual(result['partial_not_run'], ['real'])
        self.assertFalse(build.complete(result))
        result = build.summarize('<testsuite><testcase name="real"><system-out>[NOT VERIFIED] Final WindowServer pixels</system-out></testcase></testsuite>', ['real'])
        self.assertTrue(result['limitations'])
        self.assertTrue(build.complete(result))

    def test_package_requires_explicit_directories(self):
        with self.assertRaises(SystemExit):
            build.arguments(['--package'])


if __name__ == '__main__':
    unittest.main()
