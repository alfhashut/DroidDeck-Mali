"""Exercise the actual glibc client against fragmented and malformed wire responses."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import unittest

MAGIC = 0x564D4444
HEADER = struct.pack("<4I", MAGIC, 1, 1, 0)
RECORD = b"Mali-G52\0".ljust(256, b"\0") + struct.pack("<5I", 0x13B, 0x7093, 0x401000, 0xDEADBEEF, 1)


def response(payload, **changes):
    fields = dict(magic=MAGIC, version=1, opcode=1, length=len(payload))
    fields.update(changes)
    return struct.pack("<4I", *fields.values()) + payload


class ProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mali-probe-test-")
        cls.binary = str(Path(cls.directory.name) / "probe")
        subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra",
                        "-Werror", "-static", str(Path(__file__).with_name("broker_probe.c")),
                        "-o", cls.binary], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def exchange(self, wire, fragment=False):
        with tempfile.TemporaryDirectory(prefix="mb-sock-") as directory:
            path = str(Path(directory) / "broker.sock")
            errors = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(path)
                server.listen(1)
                server.settimeout(3)

                def serve():
                    try:
                        with server.accept()[0] as client:
                            client.settimeout(3)
                            request = b""
                            while len(request) < 16:
                                part = client.recv(16 - len(request))
                                if not part:
                                    raise AssertionError("truncated client request")
                                request += part
                            self.assertEqual(request, HEADER)
                            if fragment:
                                for byte in wire:
                                    client.sendall(bytes([byte]))
                            else:
                                client.sendall(wire)
                    except (BrokenPipeError, ConnectionResetError):
                        pass  # The client may reject the header before reading its payload.
                    except Exception as error:
                        errors.append(error)

                thread = threading.Thread(target=serve, daemon=True)
                thread.start()
                runner = [os.environ["PROBE_RUNNER"]] if os.environ.get("PROBE_RUNNER") else []
                result = subprocess.run(runner + [self.binary, path], text=True, capture_output=True, timeout=4)
                thread.join(timeout=4)
                self.assertFalse(thread.is_alive(), "server thread did not stop")
                if errors:
                    raise errors[0]
                return result

    def test_fragmented_success(self):
        result = self.exchange(response(struct.pack("<3I", 0, 0, 1) + RECORD), fragment=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        for value in ["Mali-G52", "vendorID: 0x0000013b", "deviceID: 0x00007093",
                      "apiVersion: 4198400 (1.1.0", "driverVersion: 3735928559", "deviceType: 1"]:
            self.assertIn(value, result.stdout)

    def test_multiple_devices(self):
        result = self.exchange(response(struct.pack("<3I", 0, 0, 2) + RECORD * 2))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("count: 2", result.stdout)
        self.assertIn("Device 1", result.stdout)

    def test_bad_headers(self):
        for change in [dict(magic=0), dict(version=2), dict(opcode=2), dict(length=5000), dict(length=0)]:
            with self.subTest(change=change):
                result = self.exchange(response(b"", **change))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Invalid broker response header", result.stderr)

    def test_truncated_payload(self):
        result = self.exchange(response(b"\0" * 4, length=12))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("response payload", result.stderr)

    def test_invalid_count(self):
        for count in [1, 17, 0xFFFFFFFF]:
            with self.subTest(count=count):
                result = self.exchange(response(struct.pack("<3I", 0, 0, count)))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("length/count", result.stderr)

    def test_unterminated_name(self):
        result = self.exchange(response(struct.pack("<3I", 0, 0, 1) + b"x" * 256 + RECORD[256:]))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unterminated", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_vulkan_error(self):
        result = self.exchange(response(struct.pack("<3I", 2, 0xFFFFFFFD, 0)))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("status=2 VkResult=-3", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_zero_devices_is_failure(self):
        result = self.exchange(response(struct.pack("<3I", 0, 0, 0)))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
