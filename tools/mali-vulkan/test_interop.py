#!/usr/bin/env python3
"""v6 checks against the actual broker code; mock Vulkan never represents real Mali results."""
import os
import shlex
from pathlib import Path
import struct
import subprocess
import unittest
import test_icd as fixture


class InteropTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture.IcdTests.setUpClass()
        cls.addClassCleanup(fixture.IcdTests.doClassCleanups)
        cls.directory = fixture.IcdTests.directory
        cls.manifest = fixture.IcdTests.manifest
        root = Path(__file__).resolve().parent
        compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
        java = Path(os.environ.get("JAVA_HOME", "/usr/lib/jvm/default"))
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(root),
                 "-I" + os.environ.get("VULKAN_HEADERS", "/usr/include")]
        subprocess.run(compiler + flags + [str(root / "tests/interop_contract.c"), "-l:libvulkan.so.1", "-o", str(cls.directory / "interop_contract")], check=True)
        subprocess.run(compiler + flags + [str(root / "tests/consumer_contract.c"), "-I" + str(root / "tests"),
                       "-I" + str(java / "include"), "-I" + str(java / "include/linux"), "-pthread",
                       "-o", str(cls.directory / "consumer_contract")], check=True)


    broker = fixture.IcdTests.broker
    connect = fixture.IcdTests.connect
    device_request = fixture.IcdTests.device_request

    def call(self, c, op, data=b"", expected=(0, 0, 0)):
        prefix, reply = fixture.rpc(c, op, data, version=6)
        self.assertEqual(prefix, expected, (op, prefix))
        return reply

    def fields(self, c, op, *args, expected=(0, 0, 0)):
        return self.call(c, op, struct.pack("<" + "I" * len(args), *args), expected)

    def new(self, c, op, *args):
        return struct.unpack("<I", self.fields(c, op, *args, expected=(0, 0, 1)))[0]

    def session(self, path, ahb=False):
        c = self.connect(path)
        c.settimeout(8)
        self.fields(c, 2, 1 << 22)
        self.call(c, 4, expected=(0, 0, 1))
        data = self.device_request() + struct.pack("<I", int(ahb))
        d = struct.unpack("<I", self.call(c, 14, data, (0, 0, 1)))[0]
        return c, d

    def storage(self, c, d, size=4096):
        b = struct.unpack("<I", self.call(c, 32, struct.pack("<IQI", d, size, 3), (0, 0, 1)))[0]
        req = struct.unpack("<QQI", self.fields(c, 34, d, b, expected=(0, 0, 1)))
        self.assertEqual(req, (size, 256, 2))
        m = struct.unpack("<I", self.call(c, 35, struct.pack("<IQI", d, req[0], 1), (0, 0, 1)))[0]
        self.call(c, 37, struct.pack("<IIIQ", d, b, m, 0))
        return b, m

    def command(self, c, d):
        q = self.new(c, 15, d, 0, 0)
        p = self.new(c, 17, d, 0, 0)
        command = self.new(c, 19, d, p, 0, 1)
        self.fields(c, 21, d, command, 0)
        return q, p, command

    def barrier(self, c, d, command, image, old, new, src, dst, sa, da, sf=0xffffffff, df=0xffffffff, expected=(0, 0, 0)):
        self.fields(c, 49, d, command, src, dst, 1, image, sa, da, old, new, sf, df, expected=expected)

    def ahb_frame(self, c, d, quadrants=False):
        reply = self.fields(c, 52, d, 64, 64, expected=(0, 0, 1))
        im, mem, token, width, height, fmt, layers, usage = struct.unpack("<7IQ", reply)
        self.assertEqual((width, height, fmt, layers), (64, 64, 1, 1))
        b, m = self.storage(c, d, 16384)
        if quadrants:
            pixels = bytearray()
            for y in range(64):
                for x in range(64):
                    pixels.extend((255, 0, 0, 255) if y < 32 and x < 32 else (0, 255, 0, 255) if y < 32 else (0, 0, 255, 255) if x < 32 else (255, 255, 255, 255))
            self.call(c, 38, struct.pack("<IIQQ", d, m, 0, 16384))
            for offset in range(0, 16384, 4096):
                self.call(c, 41, struct.pack("<IIQI", d, m, offset, 4096) + pixels[offset:offset + 4096])
            self.fields(c, 39, d, m)
        q, pool, command = self.command(c, d)
        self.barrier(c, d, command, im, 0, 7, 1, 0x1000, 0, 0x1000, 0xfffffffd, 0)
        if quadrants:
            self.fields(c, 51, d, command, im, b, 7, 64, 64, 1)
        else:
            self.call(c, 50, struct.pack("<4I4f", d, command, im, 7, .25, .5, .75, 1))
        self.barrier(c, d, command, im, 7, 6, 0x1000, 0x1000, 0x1000, 0x800)
        self.fields(c, 51, d, command, im, b, 6, 64, 64, 0)
        self.barrier(c, d, command, im, 6, 1, 0x1000, 0x2000, 0x800, 0, 0, 0xfffffffd)
        self.fields(c, 22, d, command)
        fence = self.new(c, 54, d, 8)
        self.fields(c, 31, d, q, command, fence)
        sync = self.new(c, 55, d, fence, 8)
        return im, mem, token, b, m, pool, command, fence, sync

    def test_loader_rejects_unsupported_inputs_without_successful_recording(self):
        with self.broker(42) as path:
            env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(self.manifest),
                       VK_ICD_FILENAMES=str(self.manifest), MALI_VULKAN_INTEROP_TEST="1", VK_LOADER_LAYERS_DISABLE="*")
            test = subprocess.run([str(self.directory / "interop_contract")], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(test.returncode, 0, test.stdout + test.stderr)
            self.assertIn("rejected recording PASS", test.stdout)

    def test_actual_consumer_asynchronous_release_and_surface_teardown(self):
        test = subprocess.run([str(self.directory / "consumer_contract")], capture_output=True, text=True, timeout=15)
        self.assertEqual(test.returncode, 0, test.stdout + test.stderr)
        self.assertIn("FD/reference balance PASS", test.stdout)

    def test_real_gpu_fill_bytes_and_noncoherent_ranges(self):
        for mode in (0, 29):
            with self.subTest(mode=mode), self.broker(mode) as path:
                c, d = self.session(path); b, m = self.storage(c, d)
                self.call(c, 38, struct.pack("<IIQQ", d, m, 0, 4096))
                self.call(c, 41, struct.pack("<IIQI", d, m, 0, 4096) + bytes(4096))
                self.call(c, 42, struct.pack("<IIQQ", d, m, 0, 4096))
                self.fields(c, 39, d, m)
                q, p, command = self.command(c, d)
                self.call(c, 48, struct.pack("<3I2QI", d, command, b, 0, 4096, 0x12345678))
                self.fields(c, 22, d, command); f = self.new(c, 27, d, 0)
                self.fields(c, 31, d, q, command, f)
                self.call(c, 38, struct.pack("<IIQQ", d, m, 0, 4096), (3, 0, 0))  # pending map
                self.call(c, 30, struct.pack("<3IQ", d, f, 1, 5000000000))
                self.call(c, 38, struct.pack("<IIQQ", d, m, 0, 4096))
                self.call(c, 43, struct.pack("<IIQQ", d, m, 0, 4096))
                actual = self.call(c, 40, struct.pack("<IIQI", d, m, 0, 4096), (0, 0, 1))
                self.assertEqual(actual, struct.pack("<I", 0x12345678) * 1024)
                self.fields(c, 39, d, m); self.fields(c, 20, d, p, command)
                self.fields(c, 33, d, b); self.fields(c, 36, d, m)
                self.fields(c, 36, d, m, expected=(3, 0, 0))
                self.fields(c, 16, d)
            self.assertIn("buffers=1/1", self.cleanup_output)
            self.assertIn("flush=1 invalidate=1", self.cleanup_output)

    def test_memory_types_binding_ranges_and_wrong_parent(self):
        with self.broker() as path:
            c, d = self.session(path); b, m = self.storage(c, d)
            self.fields(c, 36, d, m, expected=(3, 0, 0))
            self.call(c, 35, struct.pack("<IQI", d, 4096, 2), (3, 0, 0))
            wrong_type = struct.unpack("<I", self.call(c, 35, struct.pack("<IQI", d, 4096, 0), (0, 0, 1)))[0]
            unbound = struct.unpack("<I", self.call(c, 32, struct.pack("<IQI", d, 4096, 3), (0, 0, 1)))[0]
            self.call(c, 37, struct.pack("<IIIQ", d, unbound, wrong_type, 0), (3, 0, 0))
            for offset, size in ((4096, 1), (4095, 2), (0, 0), (0, 4097)):
                self.call(c, 38, struct.pack("<IIQQ", d, m, offset, size), (3, 0, 0))
            self.call(c, 38, struct.pack("<IIQQ", d, m, 0, 4096))
            self.call(c, 42, struct.pack("<IIQQ", d, m, 1, 256), (3, 0, 0))
            self.call(c, 43, struct.pack("<IIQQ", d, m, 0, 255), (3, 0, 0))
            self.call(c, 40, struct.pack("<IIQI", d, m, 4095, 2), (3, 0, 0))
            self.call(c, 41, struct.pack("<IIQI", d, m, 0, 4) + b"x", (3, 0, 0))
            self.fields(c, 39, d, m)
            d2 = struct.unpack("<I", self.call(c, 14, self.device_request() + bytes(4), (0, 0, 1)))[0]
            self.fields(c, 34, d2, b, expected=(3, 0, 0))
            self.call(c, 37, struct.pack("<IIIQ", d2, unbound, m, 0), (3, 0, 0))
            self.fields(c, 33, d, b); self.fields(c, 33, d, b, expected=(3, 0, 0))
            self.fields(c, 16, d2); self.fields(c, 16, d)

    def test_binding_alignment_and_allocation_bounds(self):
        with self.broker() as path:
            c, d = self.session(path)
            b = struct.unpack("<I", self.call(c, 32, struct.pack("<IQI", d, 4096, 3), (0, 0, 1)))[0]
            m = struct.unpack("<I", self.call(c, 35, struct.pack("<IQI", d, 4096, 1), (0, 0, 1)))[0]
            for offset in (1, 256, 0xffffffffffffffff): self.call(c, 37, struct.pack("<IIIQ", d, b, m, offset), (3, 0, 0))
            self.call(c, 37, struct.pack("<IIIQ", d, b, m, 0)); self.fields(c, 16, d)

    def test_image_layout_and_copy_extent_validation(self):
        with self.broker() as path:
            c, d = self.session(path); b, m = self.storage(c, d, 16384)
            im = self.new(c, 44, d, 64, 64, 3)
            req = struct.unpack("<QQI", self.fields(c, 46, d, im, expected=(0, 0, 1))); self.assertEqual(req, (16384, 256, 1))
            mem = struct.unpack("<I", self.call(c, 35, struct.pack("<IQI", d, req[0], 0), (0, 0, 1)))[0]
            self.call(c, 47, struct.pack("<IIIQ", d, im, mem, 0))
            q, p, command = self.command(c, d)
            self.barrier(c, d, command, im, 7, 6, 0x1000, 0x1000, 0x1000, 0x800, expected=(3, 0, 0))
            self.barrier(c, d, command, im, 0, 7, 1, 0x1000, 0, 0x1000)
            self.call(c, 50, struct.pack("<4I4f", d, command, im, 7, .25, .5, .75, 1))
            self.barrier(c, d, command, im, 7, 6, 0x1000, 0x1000, 0x1000, 0x800)
            self.fields(c, 51, d, command, im, b, 6, 63, 64, 0, expected=(3, 0, 0))
            self.fields(c, 51, d, command, im, b, 7, 64, 64, 0, expected=(3, 0, 0))
            self.fields(c, 51, d, command, im, b, 6, 64, 64, 0)
            self.fields(c, 45, d, im, expected=(3, 0, 0))  # live command references
            self.fields(c, 22, d, command); f = self.new(c, 27, d, 0); self.fields(c, 31, d, q, command, f)
            self.call(c, 30, struct.pack("<3IQ", d, f, 1, 5000000000))
            self.call(c, 38, struct.pack("<IIQQ", d, m, 0, 16384))
            self.assertEqual(self.call(c, 40, struct.pack("<IIQI", d, m, 0, 4096), (0, 0, 1)), bytes((64, 128, 191, 255)) * 1024)
            self.fields(c, 39, d, m); self.fields(c, 16, d)

    def test_ahb_capability_gating_and_native_errors(self):
        for mode, expected in ((33, -7), (34, -11), (37, -1000072003)):
            with self.subTest(mode=mode), self.broker(mode) as path:
                if mode == 33:
                    c = self.connect(path); self.fields(c, 2, 1 << 22); self.call(c, 4, expected=(0, 0, 1))
                    self.call(c, 14, self.device_request() + struct.pack("<I", 1), (2, expected & 0xffffffff, 0))
                else:
                    c, d = self.session(path, True); self.fields(c, 52, d, 64, 64, expected=(2, expected & 0xffffffff, 0)); self.fields(c, 16, d)
        with self.broker(42) as path:
            c, d = self.session(path)
            self.fields(c, 52, d, 64, 64, expected=(2, (-7) & 0xffffffff, 0)); self.fields(c, 16, d)

    def test_ahb_dedication_refcounts_sync_and_repeat_present(self):
        with self.broker(42) as path:
            for cycle in range(2):
                c, d = self.session(path, True)
                im, mem, token, b, m, pool, command, fence, sync = self.ahb_frame(c, d, True)
                self.fields(c, 53, d, token, expected=(3, 0, 0))
                self.fields(c, 57, d, sync, expected=(3, 0, 0))
                self.fields(c, 56, d, sync, 5000)
                self.fields(c, 55, d, fence, 8, expected=(3, 0, 0))
                self.fields(c, 54, d, 0x10, expected=(3, 0, 0))
                self.fields(c, 59, d, token + 999, sync, expected=(3, 0, 0))
                self.fields(c, 59, d, token, sync)
                self.fields(c, 59, d, token, sync)
                checked = self.fields(c, 58, d, token, sync, 1, expected=(0, 0, 1)); self.assertEqual(checked, struct.pack("<I", 1))
                self.fields(c, 20, d, pool, command); self.fields(c, 45, d, im); self.fields(c, 36, d, mem)
                self.fields(c, 53, d, token); self.fields(c, 53, d, token, expected=(3, 0, 0))
                self.fields(c, 57, d, sync); self.fields(c, 57, d, sync, expected=(3, 0, 0)); self.fields(c, 16, d)
                c.close()
        self.assertIn("AHB=2/2 consumers=4 CPU=2", self.cleanup_output)

    def test_cpu_lockability_and_same_image_pixels(self):
        with self.broker(35) as path:
            c, d = self.session(path, True); frame = self.ahb_frame(c, d)
            self.fields(c, 56, d, frame[-1], 5000)
            self.assertEqual(self.fields(c, 58, d, frame[2], frame[-1], 0, expected=(0, 0, 1)), bytes(4))
            self.fields(c, 16, d)
        self.assertIn("CPU=0", self.cleanup_output)

    def test_unsignaled_producer_never_reaches_consumer(self):
        with self.broker(40) as path:
            c, d = self.session(path, True); frame = self.ahb_frame(c, d, True)
            self.fields(c, 56, d, frame[-1], 0, expected=(0, 2, 0))
            self.fields(c, 59, d, frame[2], frame[-1], expected=(0, 2, 0))
            self.fields(c, 53, d, frame[2], expected=(3, 0, 0))
            self.fields(c, 16, d)
        self.assertIn("consumers=0", self.cleanup_output)
        self.assertIn("idle=1", self.cleanup_output)

    def test_unrelated_sync_and_wrong_device_ahb_token_rejected(self):
        with self.broker(42) as path:
            c, d = self.session(path, True)
            first = self.ahb_frame(c, d)
            second = self.ahb_frame(c, d)
            self.fields(c, 59, d, first[2], second[-1], expected=(3, 0, 0))
            self.fields(c, 58, d, first[2], second[-1], 0, expected=(3, 0, 0))
            other = struct.unpack("<I", self.call(c, 14, self.device_request() + struct.pack("<I", 1), (0, 0, 1)))[0]
            self.fields(c, 53, other, first[2], expected=(3, 0, 0))
            self.fields(c, 53, d, first[0], expected=(3, 0, 0))
            self.fields(c, 56, d, first[-1], 5000); self.fields(c, 56, d, second[-1], 5000)
            self.fields(c, 16, other); self.fields(c, 16, d)
        self.assertIn("consumers=0", self.cleanup_output)

    def test_disconnect_unmaps_and_frees_owned_memory(self):
        with self.broker() as path:
            c, d = self.session(path); b, m = self.storage(c, d)
            self.call(c, 38, struct.pack("<IIQQ", d, m, 0, 4096)); c.close()
        self.assertIn("buffers=1/1 images=0/0 memories=1/1", self.cleanup_output)

    def test_export_failure_drains_producer_and_drops_ahb_refs(self):
        with self.broker(38) as path:
            c, d = self.session(path, True)
            # Export fails after submission. Verify device teardown drains that producer.
            old_new = self.new
            def failed_export(connection, op, *args):
                if op == 55:
                    self.fields(connection, op, *args, expected=(2, (-10) & 0xffffffff, 0))
                    return 0
                return old_new(connection, op, *args)
            self.new = failed_export
            try: self.ahb_frame(c, d)
            finally: self.new = old_new
            self.fields(c, 16, d)
        self.assertIn("AHB=1/1", self.cleanup_output)
        self.assertIn("idle=1", self.cleanup_output)

    def test_v6_bad_length_and_version_cannot_upgrade(self):
        with self.broker() as path:
            c, d = self.session(path)
            self.call(c, 32, struct.pack("<IQ", d, 4096), (3, 0, 0))
            self.fields(c, 16, d)
        with self.broker() as path:
            c = self.connect(path); self.fields(c, 2, 1 << 22)
            self.assertEqual(fixture.rpc(c, 4, version=5)[0], (3, 0, 0))


if __name__ == "__main__":
    unittest.main()
