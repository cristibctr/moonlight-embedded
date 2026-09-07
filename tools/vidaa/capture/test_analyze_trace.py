import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('analyze_trace', Path(__file__).with_name('analyze-trace.py'))
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


class TraceScopeTests(unittest.TestCase):
    def test_tv_only_does_not_use_source_clock(self):
        row = dict(receive_us=1000, submit_us=2000, complete_us=3500)
        self.assertEqual(analyzer.trace_timing(row, float('nan'), {}, True),
                         dict(receive_to_submit_ms=1, submit_call_ms=1.5))

    def test_source_mode_keeps_host_bounds(self):
        row = dict(receive_us=10000, submit_us=11000, complete_us=12000)
        report = dict(clock_offset_low_us=0, clock_offset_high_us=0, source_uncertainty_ms=1)
        self.assertIn('host_to_receive_low_ms', analyzer.trace_timing(row, 1, report))

    def test_missing_validation_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'Validate the source clock'):
                analyzer.require_source_validation(Path(tmp), dict(source_clock='tv'))

    def test_matching_validation_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            report = dict(source_clock='tv', source_uncertainty_ms=2)
            validation = dict(source_uncertainty_ms=2, capture_start_ms=10, capture_end_ms=20)
            (root / 'source-clock-validation.json').write_text(json.dumps(validation))
            (root / 'samples.csv').write_text('source_ms,tv_start_us,tv_end_us\n1,10000,20000\n')
            analyzer.require_source_validation(root, report)
            report['source_uncertainty_ms'] = 3
            with self.assertRaisesRegex(ValueError, 'does not match'):
                analyzer.require_source_validation(root, report)
            report['source_uncertainty_ms'] = 2
            (root / 'samples.csv').write_text('source_ms,tv_start_us,tv_end_us\n1,10001,20000\n')
            with self.assertRaisesRegex(ValueError, 'different capture'):
                analyzer.require_source_validation(root, report)


if __name__ == '__main__':
    unittest.main()
