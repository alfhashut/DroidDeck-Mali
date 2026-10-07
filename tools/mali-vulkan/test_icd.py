#!/usr/bin/env python3
"""Host checks with the actual broker/ICD code and an explicitly mocked Android Vulkan backend."""
import contextlib
import json
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parent
MAGIC = 0x564D4444


def read_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise EOFError("short response")
        data.extend(part)
    return bytes(data)


def rpc(connection, opcode, payload=b"", version=2):
    connection.sendall(struct.pack("<4I", MAGIC, version, opcode, len(payload)) + payload)
    magic, response_version, response_opcode, length = struct.unpack("<4I", read_exact(connection, 16))
    assert (magic, response_version, response_opcode) == (MAGIC, version, opcode)
    assert 12 <= length <= (131072 if version == 3 else 4428)
    reply = read_exact(connection, length)
    return struct.unpack("<3I", reply[:12]), reply[12:]


class IcdTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="mali-icd-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
        headers = Path(os.environ.get("VULKAN_HEADERS", "/usr/include"))
        java = Path(os.environ.get("JAVA_HOME", "/usr/lib/jvm/default"))
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(headers), "-I" + str(ROOT)]

        def build(source, output, extra=()):
            subprocess.run(compiler + flags + [str(ROOT / source), "-o", str(cls.directory / output)] + list(extra), check=True)

        build("icd_proxy.c", "libdroiddeck_mali_proxy.so", ["-shared", "-fPIC", "-fvisibility=hidden", "-Wl,-z,defs", "-pthread"])
        build("loader_test.c", "vulkan_loader_test", ["-ldl"])
        build("broker_probe.c", "broker_probe")
        build("capability_inventory.c", "capability_inventory")
        build("tests/capability_contract.c", "capability_contract", ["-l:libvulkan.so.1"])
        build("tests/icd_contract.c", "icd_contract", ["-ldl"])
        build("tests/broker_mock.c", "broker_mock", ["-pthread", "-I" + str(ROOT / "tests"),
              "-I" + str(java / "include"), "-I" + str(java / "include/linux")])
        cls.manifest = cls.directory / "mali_proxy_icd.json"
        cls.manifest.write_bytes((ROOT / "mali_proxy_icd.json").read_bytes())

    @contextlib.contextmanager
    def broker(self, mode=0):
        path = self.directory / "broker.sock"
        with tempfile.TemporaryFile(mode="w+") as errors:
            process = subprocess.Popen([str(self.directory / "broker_mock"), "--serve", str(path), str(mode)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, text=True)
            try:
                self.assertEqual(process.stdout.readline().strip(), "READY")
                yield str(path)
            finally:
                try:
                    output, _ = process.communicate("\n", timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    self.fail("broker shutdown timed out")
                errors.seek(0)
                self.assertEqual(process.returncode, 0, errors.read())
                self.cleanup_output = output
                self.assertFalse(path.exists(), "broker must remove its socket")

    def run_program(self, name, path, *arguments, capabilities=False):
        env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(self.manifest),
                   VK_ICD_FILENAMES=str(self.manifest), VK_LOADER_LAYERS_DISABLE="*")
        if capabilities:
            env["MALI_VULKAN_QUERY_CAPABILITIES"] = "1"
        else:
            env.pop("MALI_VULKAN_QUERY_CAPABILITIES", None)
        return subprocess.run([str(self.directory / name), *map(str, arguments)], env=env,
                              capture_output=True, text=True, timeout=20)

    def connect(self, path):
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(connection.close)
        connection.settimeout(5)
        connection.connect(path)
        return connection

    def test_capability_loader_contract(self):
        with self.broker() as path:
            result = self.run_program("capability_contract", path, capabilities=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("capability contracts:", result.stdout)
            result = self.run_program("capability_inventory", path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("VK_KHR_android_surface", result.stdout)
        self.assertIn("creates=1 destroys=1", self.cleanup_output)

    def test_corrupt_capability_snapshot_does_not_publish_devices(self):
        for mode in (7, 8): # Invalid VkBool32 / invalid memory heap index.
            with self.subTest(mode=mode), self.broker(mode) as path:
                result = self.run_program("vulkan_loader_test", path, capabilities=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("deviceName:", result.stdout)
                self.assertIn("invalid capability snapshot", result.stderr)
            self.assertIn("creates=1 destroys=1", self.cleanup_output)

    def test_inventory_fragmentation_and_bad_wire_names(self):
        for malformed in (False, True):
            path = str(self.directory / "inventory.sock")
            errors = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(path); server.listen(1); server.settimeout(10)
                def respond():
                    try:
                        with server.accept()[0] as connection:
                            self.assertEqual(struct.unpack("<4I", read_exact(connection, 16)), (MAGIC, 3, 6, 0))
                            name = b"x" * 256 if malformed else b"HOST_WIRE_ONLY".ljust(256, b"\0")
                            body = struct.pack("<4I", 0, 0, 1, 1 << 22) + name + struct.pack("<I", 9)
                            wire = struct.pack("<4I", MAGIC, 3, 6, len(body)) + body
                            for byte in wire:
                                connection.sendall(bytes([byte]))
                    except Exception as error:
                        errors.append(error)
                worker = threading.Thread(target=respond); worker.start()
                result = self.run_program("capability_inventory", path)
                worker.join(15)
                self.assertFalse(worker.is_alive()); self.assertFalse(errors, errors)
                self.assertEqual(result.returncode == 0, not malformed, result.stdout + result.stderr)
                if not malformed: self.assertIn("HOST_WIRE_ONLY specVersion=9", result.stdout)
            os.unlink(path)

    def test_version_three_queries_and_unsupported_are_distinct(self):
        with self.broker(6) as path:
            connection = self.connect(path)
            prefix, data = rpc(connection, 6, version=3)
            self.assertEqual(prefix, (0, 0, 0))
            self.assertEqual(struct.unpack("<I", data)[0], 1 << 22)
            connection.close()
            connection = self.connect(path)
            self.assertEqual(rpc(connection, 2, struct.pack("<I", 1 << 22), version=3)[0], (0, 0, 0))
            self.assertEqual(rpc(connection, 4, version=3)[0], (0, 0, 1))
            prefix, data = rpc(connection, 7, struct.pack("<I", 1), version=3)
            self.assertEqual(prefix, (0, 0, 1))
            schema = (ROOT / "properties_fields.def").read_text().splitlines()
            property_bytes = 272 + sum(8 if line.startswith(("MB_U64(", "MB_SIZE(")) else 4
                                       for line in schema if line.startswith("MB_"))
            self.assertEqual(struct.unpack("<I", data[property_bytes:property_bytes + 4])[0], 0) # get2 unavailable, not false feature claims
            self.assertEqual(rpc(connection, 11, struct.pack("<2I", 1, 1), version=3)[0][0], 6)
            self.assertEqual(rpc(connection, 7, struct.pack("<I", 0x42), version=3)[0][0], 3)
            self.assertEqual(connection.recv(1), b"") # invalid ID closes and destroys the session

    def test_normal_loader(self):
        with self.broker() as path:
            result = self.run_program("vulkan_loader_test", path)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for field in ("glibc libvulkan.so.1: vkCreateInstance=0", "deviceName: HOST TEST ONLY",
                          "vendorID: 0x000013b5", "deviceID: 0x74021000", "apiVersion: 1.1.131",
                          "driverVersion: 109051904", "deviceType: 1", "Destroyed Vulkan instance"):
                self.assertIn(field, result.stdout)
        self.assertIn("creates=1 destroys=1 closes=1", self.cleanup_output)

    def test_icd_contract(self):
        with self.broker() as path:
            result = self.run_program("icd_contract", path, self.directory / "libdroiddeck_mali_proxy.so")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("ICD contracts:", result.stdout)
        self.assertIn("creates=2 destroys=2 closes=2", self.cleanup_output)

    def test_checkpoint_one_native_mock_regressions(self):
        result = subprocess.run([str(self.directory / "broker_mock"), "--self-test",
                                 str(self.directory / "native.sock"), str(self.directory / "broker_probe")],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("native broker:", result.stdout)

    def test_live_instance_and_old_probe_can_coexist(self):
        with self.broker() as path:
            connection = self.connect(path)
            self.assertEqual(rpc(connection, 2, struct.pack("<I", 1 << 22))[0], (0, 0, 0))
            result = self.run_program("broker_probe", path, path)
            self.assertEqual(result.returncode, 0, result.stderr)
            prefix, ids = rpc(connection, 4)
            self.assertEqual(prefix, (0, 0, 1))
            self.assertEqual(ids, struct.pack("<I", 1))  # not the mock Android handle 0x42
            self.assertEqual(rpc(connection, 3)[0], (0, 0, 0))
        self.assertIn("creates=2 destroys=2 closes=2", self.cleanup_output)

    def test_disconnect_and_stop_destroy_live_instances(self):
        with self.broker() as path:
            one, two = self.connect(path), self.connect(path)
            for connection in (one, two):
                self.assertEqual(rpc(connection, 2, struct.pack("<I", 1 << 22))[0], (0, 0, 0))
            one.close()  # abrupt client disconnect
            # Broker stop must wake the second connection's blocking read and destroy it.
        self.assertIn("creates=2 destroys=2 closes=2", self.cleanup_output)

    def test_invalid_session_id_and_api_version(self):
        with self.broker() as path:
            connection = self.connect(path)
            self.assertEqual(rpc(connection, 2, struct.pack("<I", (1 << 22) | (1 << 12)))[0],
                             (2, (-9) & 0xFFFFFFFF, 0))
            connection.close()
            connection = self.connect(path)
            self.assertEqual(rpc(connection, 2, struct.pack("<I", 1 << 22))[0], (0, 0, 0))
            self.assertEqual(rpc(connection, 4)[0], (0, 0, 1))
            self.assertEqual(rpc(connection, 5, struct.pack("<I", 0x42))[0], (3, 0, 0))
        self.assertIn("creates=1 destroys=1 closes=1", self.cleanup_output)

    def test_native_errors_never_report_successful_devices(self):
        for mode in (1, 2, 3, 4, 5):
            with self.subTest(mode=mode), self.broker(mode) as path:
                result = self.run_program("vulkan_loader_test", path)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("deviceName:", result.stdout)

    def test_malformed_reply_and_fragmentation(self):
        # Exercise actual loader -> ICD against deliberately fragmented/bad RPC replies.
        for malformed in (False, True):
            with self.subTest(malformed=malformed):
                path = str(self.directory / "fake.sock")
                errors = []
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                    server.bind(path)
                    server.listen(1)
                    server.settimeout(10)

                    def respond():
                        try:
                            with server.accept()[0] as connection:
                                connection.settimeout(5)
                                header = struct.unpack("<4I", read_exact(connection, 16))
                                self.assertEqual(header[:3], (MAGIC, 2, 2))
                                read_exact(connection, header[3])
                                reply = struct.pack("<4I", MAGIC, 99 if malformed else 2, 2, 12) + bytes(12)
                                for byte in reply:
                                    try:
                                        connection.sendall(bytes([byte]))
                                    except BrokenPipeError:
                                        if not malformed:
                                            raise
                                        break
                                if not malformed:
                                    header = struct.unpack("<4I", read_exact(connection, 16))
                                    self.assertEqual(header[:3], (MAGIC, 2, 4))
                                    # A count beyond the bounded protocol limit must fail enumeration.
                                    connection.sendall(struct.pack("<7I", MAGIC, 2, 4, 12, 0, 0, 17))
                                    # Drain destroy/EOF; never fabricate properties.
                                    connection.recv(16)
                        except Exception as error:
                            errors.append(error)

                    worker = threading.Thread(target=respond)
                    worker.start()
                    result = self.run_program("vulkan_loader_test", path)
                    worker.join(15)
                    self.assertFalse(worker.is_alive())
                    self.assertFalse(errors, errors)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn("deviceName:", result.stdout)
                os.unlink(path)

    def test_loader_facing_exports_and_manifest(self):
        symbols = subprocess.check_output(["nm", "-D", "--defined-only",
                                           str(self.directory / "libdroiddeck_mali_proxy.so")], text=True)
        self.assertEqual({line.split()[-1] for line in symbols.splitlines()}, {
            "vk_icdNegotiateLoaderICDInterfaceVersion", "vk_icdGetInstanceProcAddr", "vk_icdGetPhysicalDeviceProcAddr"})
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual(manifest["ICD"]["api_version"], "1.0.0")
        self.assertEqual(manifest["ICD"]["library_path"], "./libdroiddeck_mali_proxy.so")
        dependencies = subprocess.check_output(["readelf", "-d", str(self.directory / "libdroiddeck_mali_proxy.so")], text=True)
        self.assertNotIn("libvulkan", dependencies)
        dependencies = subprocess.check_output(["readelf", "-d", str(self.directory / "vulkan_loader_test")], text=True)
        self.assertNotIn("libdroiddeck_mali_proxy", dependencies)


if __name__ == "__main__":
    unittest.main(verbosity=2)
