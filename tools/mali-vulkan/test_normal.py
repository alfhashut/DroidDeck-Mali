"""Checkpoint 7 ownership contracts. Host callbacks are a model, not phone evidence."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parent
class NormalOwnershipTests(unittest.TestCase):
    def test_android_owned_release_timeout_and_restart(self):
        with tempfile.TemporaryDirectory(prefix="mali-normal-") as directory:
            binary = Path(directory) / "ownership"
            subprocess.run(shlex.split(os.environ.get("HOST_CC", "cc")) + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "tests"), str(ROOT / "tests/normal_ownership.c"), str(ROOT / "../../app/src/main/cpp/malivulkan/normal_ownership.c"), "-pthread", "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("bounded missing release", result.stdout)
            self.assertIn("restart passed", result.stdout)

    def test_actual_android_wayland_release_fences(self):
        with tempfile.TemporaryDirectory(prefix="mali-normal-release-") as directory:
            binary = Path(directory) / "release"
            subprocess.run(shlex.split(os.environ.get("HOST_CC", "cc")) + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-missing-field-initializers", "-ffunction-sections", "-fdata-sections", "-I" + str(ROOT / "tests"), "-I" + str(ROOT / "../../app/src/main/cpp/waylandcomp/generated"), str(ROOT / "tests/normal_android_release.c"), str(ROOT / "../../app/src/main/cpp/malivulkan/normal_ownership.c"), "-Wl,--gc-sections", "-lwayland-server", "-pthread", "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("actual Wayland AHB release:", result.stdout)
