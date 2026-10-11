import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('summarize_affinity', Path(__file__).with_name('summarize_affinity.py'))
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class SummaryTests(unittest.TestCase):
    def test_android_uses_only_native_counter_points_inside_this_arm(self):
        lines = '\n'.join(f'{stamp}.000 MaliPerf Android: presented-total={count} released-total={count-1}'
                          for stamp, count in [(98, 50), (101, 100), (103, 140), (106, 200)])
        result = summary.android_rate(lines, 100, 105)
        self.assertEqual(result['counter_span_s'], 2)
        self.assertEqual(result['handoff_hz'], 20)
        self.assertEqual(result['release_hz'], 20)
        self.assertFalse(summary.android_rate(lines, 100, 102)['available'])

    def test_failed_restoration_refuses_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'guest-report.txt').write_text('affinity-restored={"ok":false}\n')
            with self.assertRaisesRegex(ValueError, 'restoration'):
                summary.summarize(path)


if __name__ == '__main__':
    unittest.main()
