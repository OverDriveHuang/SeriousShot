"""The optional updater must never be omitted on missing or stale evidence."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
import test_execution_policy as policy


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tree = self.root / 'build'
        self.tree.mkdir()
        (self.tree / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Release\n')
        for name in ['src/domain/update/update.cpp', 'src/ui/qt/pinned_screenshot_window.cpp',
                     'src/ports/export_ports.hpp', 'CMakeLists.txt']:
            file = self.root / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text('original')
        (self.tree / 'hdrshot_update_tests').write_bytes(b'compiled test')
        (self.tree / 'hdrshot_update_runtime_tests').write_bytes(b'compiled runtime test')
        self.names = ['hdrshot_update_tests', 'hdrshot_color_tests', 'hdrshot_perf_gpu']

    def plan(self, force=False):
        return policy.select(self.root, self.tree, self.names, force)

    def accept(self):
        plan = self.plan()
        policy.remember(self.root, self.tree, plan, self.names, True)
        return plan

    def test_first_run_and_unknown_input_run_everything(self):
        self.assertFalse(self.plan()['omitted'])
        self.accept()
        (self.root / 'src/new_shared.hpp').write_text('new')
        self.assertFalse(self.plan()['omitted'])

    def test_identical_verified_input_reuses_only_updater(self):
        self.accept()
        self.assertEqual(self.plan()['omitted'], ['hdrshot_update_tests'])
        self.assertIn('hdrshot_perf_gpu', self.plan()['selected'])
        self.assertIn('hdrshot_color_tests', self.plan()['selected'])

    def test_pin_change_can_reuse_but_shared_port_change_cannot(self):
        self.accept()
        (self.root / 'src/ui/qt/pinned_screenshot_window.cpp').write_text('changed')
        self.assertTrue(self.plan()['omitted'])
        (self.root / 'src/ports/export_ports.hpp').write_text('changed')
        self.assertFalse(self.plan()['omitted'])

    def test_uncommitted_update_delete_and_config_changes_invalidate(self):
        for name in ['src/domain/update/update.cpp', 'build/CMakeCache.txt', 'CMakeLists.txt', 'build/hdrshot_update_tests']:
            with self.subTest(name=name):
                self.accept()
                file = self.root / name
                original = file.read_text()
                file.write_text(original + 'changed')
                self.assertFalse(self.plan()['omitted'])
                file.write_text(original)
        (self.root / 'src/domain/update/update.cpp').unlink()
        self.assertFalse(self.plan()['omitted'])

    def test_failed_incomplete_or_midrun_edits_never_cache(self):
        plan = self.plan()
        policy.remember(self.root, self.tree, plan, self.names, False)
        self.assertFalse(self.plan()['omitted'])
        policy.remember(self.root, self.tree, plan, ['hdrshot_color_tests'], True)
        self.assertFalse(self.plan()['omitted'])
        (self.root / 'src/domain/update/update.cpp').write_text('during test')
        policy.remember(self.root, self.tree, plan, self.names, True)
        self.assertFalse(self.plan()['omitted'])

    def test_corrupt_cache_force_and_new_update_target_run(self):
        self.accept()
        self.assertFalse(self.plan(True)['omitted'])
        self.names.append('hdrshot_update_runtime_tests')
        self.assertFalse(self.plan()['omitted'])
        for invalid in ['{broken', '[]', 'null', '3']:
            policy.state_path(self.tree).write_text(invalid)
            self.assertFalse(self.plan()['omitted'])

    def test_required_skip_partial_and_missing_are_incomplete(self):
        for body in ['<skipped/>', '<system-out>[SKIP] locked</system-out>']:
            result = policy.summarize(f'<testsuite><testcase name="a">{body}</testcase></testsuite>', ['a'])
            self.assertFalse(policy.complete(result))
        self.assertFalse(policy.complete(policy.summarize('<testsuite/>', ['a'])))

    def test_truncated_output_and_missing_performance_record_are_incomplete(self):
        xml = '<testsuite><testcase name="a"><system-out>PERFORMANCE_REPORT {broken\n[This part of the test output was removed since it exceeds the threshold of 1024 bytes.]</system-out></testcase></testsuite>'
        self.assertFalse(policy.complete(policy.summarize(xml, ['a'])))
        xml = '<testsuite><testcase name="hdrshot_perf_gpu"/></testsuite>'
        self.assertFalse(policy.complete(policy.summarize(xml, ['hdrshot_perf_gpu'])))
        xml = '<testsuite><testcase name="unit_tests"><system-out>PERFORMANCE_REPORT {"name":"fake"}</system-out></testcase></testsuite>'
        self.assertEqual(policy.summarize(xml, ['unit_tests'])['performance'], [])


class FixtureSelectionTests(unittest.TestCase):
    plan = SelectionTests.plan
    def setUp(self):
        SelectionTests.setUp(self)
        self.fixture = self.root / 'sample.png'
        self.fixture.write_bytes(b'fixed PQ pixels')
        self.binary = self.tree / policy.UHDR_FIXTURE_TEST
        self.binary.write_bytes(b'statically linked codec test')
        self.names.append(policy.UHDR_FIXTURE_TEST)
        self.env = patch.dict(os.environ, {'HDRSHOT_ULTRAHDR_FIXTURE': str(self.fixture)})
        self.env.start()
        self.addCleanup(self.env.stop)

    def test_fixture_cached_independently_and_manual_force(self):
        plan = self.plan()
        self.assertIn(policy.UHDR_FIXTURE_TEST, plan['selected'])
        policy.remember(self.root, self.tree, plan, [policy.UHDR_FIXTURE_TEST], False)
        self.assertIn(policy.UHDR_FIXTURE_TEST, self.plan()['omitted'])
        self.assertIn(policy.UHDR_FIXTURE_TEST, policy.select(
            self.root, self.tree, self.names, force_uhdr_fixture=True)['selected'])
        os.environ.pop('HDRSHOT_ULTRAHDR_FIXTURE')
        self.assertEqual(self.plan()['uhdr_fixture']['path'], str(self.fixture.resolve()))
        self.assertIn(policy.UHDR_FIXTURE_TEST, self.plan()['omitted'])

    def test_failed_forced_rerun_invalidates_previous_success(self):
        plan = self.plan()
        policy.remember(self.root, self.tree, plan, [policy.UHDR_FIXTURE_TEST], True)
        forced = policy.select(self.root, self.tree, self.names, force_uhdr_fixture=True)
        policy.remember(self.root, self.tree, forced, [], False)
        self.assertIn(policy.UHDR_FIXTURE_TEST, self.plan()['selected'])

    def test_corrupt_cache_and_local_patch_changes_require_run(self):
        for invalid in ('{broken', '[]', 'null'):
            policy.fixture_state_path(self.tree).write_text(invalid)
            self.assertIn(policy.UHDR_FIXTURE_TEST, self.plan()['selected'])
        plan = self.plan()
        policy.remember(self.root, self.tree, plan, [policy.UHDR_FIXTURE_TEST], True)
        cmake = self.root / 'cmake'
        cmake.mkdir()
        (cmake / 'libultrahdr_test.cmake').write_text('new patch')
        self.assertIn(policy.UHDR_FIXTURE_TEST, self.plan()['selected'])
        explicit = policy.select(self.root, self.tree, self.names, uhdr_fixture=self.fixture)
        self.assertIn(policy.UHDR_FIXTURE_TEST, explicit['selected'])

    def test_fixture_changes_and_unknown_evidence_require_run(self):
        for file in (self.binary, self.fixture, self.root / 'CMakeLists.txt'):
            plan = self.plan()
            policy.remember(self.root, self.tree, plan, [policy.UHDR_FIXTURE_TEST], False)
            file.write_bytes(file.read_bytes() + b'changed')
            self.assertIn(policy.UHDR_FIXTURE_TEST, self.plan()['selected'])
        self.fixture.unlink()
        with self.assertRaisesRegex(ValueError, 'file does not exist'):
            self.plan()
        result = policy.summarize(f'<testsuite><testcase name="{policy.UHDR_FIXTURE_TEST}"><skipped/></testcase></testsuite>', [policy.UHDR_FIXTURE_TEST])
        self.assertFalse(policy.complete(result))

    def test_fixture_failure_and_changed_during_run_not_cached(self):
        plan = self.plan()
        policy.remember(self.root, self.tree, plan, [], True)
        self.assertIn(policy.UHDR_FIXTURE_TEST, self.plan()['selected'])
        self.binary.write_bytes(b'changed during run')
        policy.remember(self.root, self.tree, plan, [policy.UHDR_FIXTURE_TEST], True)
        self.assertIn(policy.UHDR_FIXTURE_TEST, self.plan()['selected'])


class WindowsEncodingTests(unittest.TestCase):
    def test_ctest_utf8_report_is_read_independently_of_windows_locale(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            logs = root / 'logs'
            name = 'core_\u8272\u5f69'
            plan = {'selected': [name], 'omitted': [], 'uhdr_fixture': None}
            def ctest(*args, **kwargs):
                (logs / 'ctest.xml').write_bytes(
                    f'<testsuite><testcase name="{name}"><system-out>\u6d4b\u91cf\u901a\u8fc7</system-out></testcase></testsuite>'.encode('utf-8'))
                return subprocess.CompletedProcess([], 0)
            with patch.object(sys, 'argv', ['policy', '--build-dir', str(root), '--logs', str(logs)]), \
                    patch.object(policy.subprocess, 'check_output', return_value=json.dumps({'tests': [{'name': name}]})), \
                    patch.object(policy.subprocess, 'run', side_effect=ctest), \
                    patch.object(policy, 'select', return_value=plan), patch.object(policy, 'remember'):
                self.assertEqual(policy.main(), 0)
            report = json.loads((logs / 'summary.json').read_text(encoding='utf-8'))
            self.assertEqual(report['passed'], [name])


class PerformanceTests(unittest.TestCase):
    def test_utf8_native_output_preserves_bytes_and_measurements(self):
        import record_performance as perf
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            raw = '\u6d4b\u91cf\u8272\u5f69\n{"gpu_ms": 2}\n'.encode('utf-8')
            command = [sys.executable, '-c', f'import os; os.write(1, {raw!r})']
            self.assertEqual(perf.record('unicode', command, root), 0)
            self.assertEqual(next((root / 'runs').glob('*/output.log')).read_bytes(), raw)
            report = json.loads(next((root / 'runs').glob('*/report.json')).read_text(encoding='utf-8'))
            self.assertEqual(report['measurements']['gpu_ms'], 2)

    def test_metrics_extract_json_csv_and_analysis_measurements(self):
        import record_performance as perf
        metrics = perf.metrics('{"gpu":{"p95_ms":2},"peak_bytes":100}\n')
        self.assertEqual(metrics['gpu.p95_ms'], 2)
        self.assertEqual(metrics['peak_bytes'], 100)
        metrics = perf.metrics('width,height,range,iteration,codec_ms\n5,3,HDR,0,9\n5,3,HDR,1,3\n')
        self.assertEqual(metrics['5x3/HDR/codec_ms'], 6)
        metrics = perf.metrics('BENCH 5120x2880 initial wall_ms=12 retained=88 sampling_ms=4.1e-05\n')
        self.assertEqual(metrics['5120x2880/initial/wall_ms'], 12)
        self.assertEqual(metrics['5120x2880/initial/sampling_ms'], 4.1e-5)
        with self.assertRaises(ValueError):
            perf.metrics('BENCH 5x3 initial wall_ms=nan')

    def test_process_failure_and_missing_file_cannot_create_baseline(self):
        import record_performance as perf
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertNotEqual(perf.record('failure', [sys.executable, '-c', 'raise SystemExit(7)'], root), 0)
            self.assertFalse(list((root / 'baselines').glob('**/*.json')))
            self.assertNotEqual(perf.record('missing', [sys.executable, '-c', 'pass', '{output}'], root), 0)

    def test_first_measurement_then_fixed_baseline_not_rolling(self):
        import record_performance as perf
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            command = [sys.executable, '-c', 'print(\'{"gpu_ms": 2}\')']
            self.assertEqual(perf.record('valid', command, root), 0)
            baseline = next((root / 'baselines').glob('**/*.json'))
            before = baseline.read_bytes()
            self.assertEqual(perf.record('valid', command, root), 0)
            self.assertEqual(baseline.read_bytes(), before)
            reports = [json.loads(p.read_text()) for p in (root / 'runs').glob('*/report.json')]
            self.assertEqual({r['comparison'] for r in reports}, {'first_measurement', 'compared'})


if __name__ == '__main__':
    unittest.main()
