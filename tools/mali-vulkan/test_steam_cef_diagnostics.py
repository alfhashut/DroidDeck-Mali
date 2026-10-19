"""Bounded, read-only option-string capture; no installed Steam or phone needed."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import diagnose_steam_cef as cef


class CefInspectionTests(unittest.TestCase):
    def test_reads_exact_strings_and_offsets_without_modifying_binary(self):
        data = b'\x7fELF\0Use vulkan ANGLE backend\0-cef-use-vulkan\0--use-angle=vulkan\0'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'steamclient.so'
            path.write_bytes(data)
            before = path.stat()
            with contextlib.redirect_stdout(io.StringIO()) as output:
                cef.scan(path)
            self.assertEqual(path.read_bytes(), data)
            self.assertEqual(path.stat().st_mtime_ns, before.st_mtime_ns)
        rows = [json.loads(line.split('=', 1)[1]) for line in output.getvalue().splitlines()
                if line.startswith('option-strings=')]
        options = {row['token']: row for row in rows}
        self.assertEqual(options['cef-use-angle']['hits'], [])
        self.assertEqual(options['cef-use-gl']['hits'], [])
        self.assertEqual(options['cef-use-vulkan']['hits'][0]['text'], '-cef-use-vulkan')
        self.assertEqual(options['use-angle']['hits'][0]['offset'], hex(data.index(b'use-angle')))

    def test_binary_bytes_without_nuls_cannot_expand_capture_past_bounds(self):
        data = b'x' * 10000 + b'use-angle' + b'y' * 10000
        text = cef.cstring(data, 10000)
        self.assertLessEqual(len(text), 1536)
        self.assertIn('use-angle', text)


if __name__ == '__main__':
    unittest.main()
