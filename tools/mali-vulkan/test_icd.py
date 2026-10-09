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
    assert 12 <= length <= (131072 if version >= 3 else 4428)
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
        build("tests/device_contract.c", "device_contract", ["-l:libvulkan.so.1"])
        build("tests/submit_contract.c", "submit_contract", ["-l:libvulkan.so.1"])
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
                self.broker_process = process
                yield str(path)
            finally:
                try:
                    output, _ = process.communicate("\n", timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    self.fail("broker shutdown timed out")
                errors.seek(0)
                self.broker_errors = errors.read()
                self.assertEqual(process.returncode, 0, self.broker_errors)
                self.cleanup_output = output
                self.assertFalse(path.exists(), "broker must remove its socket")

    def run_program(self, name, path, *arguments, capabilities=False, device=False):
        env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(self.manifest),
                   VK_ICD_FILENAMES=str(self.manifest), VK_LOADER_LAYERS_DISABLE="*")
        if device:
            env["MALI_VULKAN_DEVICE_TEST"] = "1"
        else:
            env.pop("MALI_VULKAN_DEVICE_TEST", None)
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

    def submit_session(self, path, family=0):
        c = self.connect(path)
        self.assertEqual(rpc(c, 2, struct.pack("<I", 1 << 22), version=5)[0], (0, 0, 0))
        self.assertEqual(rpc(c, 4, version=5)[0], (0, 0, 1))
        prefix, data = rpc(c, 14, self.device_request(family=family), version=5)
        self.assertEqual(prefix, (0, 0, 1))
        return c, struct.unpack("<I", data)[0]

    def submit_rpc(self, c, op, *fields, expected=(0, 0, 0), suffix=b""):
        prefix, data = rpc(c, op, struct.pack("<" + "I" * len(fields), *fields) + suffix, version=5)
        self.assertEqual(prefix, expected, (op, fields, prefix))
        return struct.unpack("<I", data)[0] if data else None

    def submit_objects(self, c, d):
        q = self.submit_rpc(c, 15, d, 0, 0, expected=(0, 0, 1))
        p = self.submit_rpc(c, 17, d, 0, 0, expected=(0, 0, 1))
        b = self.submit_rpc(c, 19, d, p, 0, 1, expected=(0, 0, 1))
        e = self.submit_rpc(c, 23, d, 0, expected=(0, 0, 1))
        f = self.submit_rpc(c, 27, d, 0, expected=(0, 0, 1))
        return q, p, b, e, f

    def submit_record(self, c, d, b, e):
        self.submit_rpc(c, 21, d, b, 0)
        self.submit_rpc(c, 26, d, b, e, 0x10000)
        self.submit_rpc(c, 22, d, b)

    def test_submit_version_is_opt_in_and_cannot_upgrade(self):
        with self.broker() as path:
            c = self.device_session(path)
            self.assertEqual(rpc(c, 14, self.device_request(), version=4)[0], (0, 0, 1))
            self.assertEqual(rpc(c, 17, struct.pack("<3I", 1, 0, 0), version=5)[0], (3, 0, 0))
            try:
                self.assertEqual(c.recv(1), b"")
            except ConnectionResetError:
                pass
        self.assertIn("pools=0/0 commands=0/0", self.cleanup_output)
        with self.broker() as path:
            c = self.device_session(path)
            self.assertEqual(rpc(c, 17, struct.pack("<3I", 1, 0, 0), version=4)[0], (3, 0, 0))
        self.assertIn("pools=0/0 commands=0/0", self.cleanup_output)

    def test_submit_fragmentation_and_same_device_wrong_pool(self):
        with self.broker() as path:
            c, d = self.submit_session(path)
            q, p, b, e, f = self.submit_objects(c, d)
            p2 = self.submit_rpc(c, 17, d, 0, 0, expected=(0, 0, 1))
            self.submit_rpc(c, 20, d, p2, b, expected=(3, 0, 0))
            for op, fields in ((21, (d, b, 0)), (26, (d, b, e, 0x10000)), (22, (d, b)), (31, (d, q, b, f))):
                payload = struct.pack("<" + "I" * len(fields), *fields)
                wire = struct.pack("<4I", MAGIC, 5, op, len(payload)) + payload
                for byte in wire: c.sendall(bytes([byte]))
                self.assertEqual(struct.unpack("<4I", read_exact(c, 16)), (MAGIC, 5, op, 12))
                self.assertEqual(struct.unpack("<3I", read_exact(c, 12)), (0, 0, 0))
            self.submit_rpc(c, 30, d, f, 1, suffix=struct.pack("<Q", 5000000000))
            self.submit_rpc(c, 16, d)
        self.assertIn("pools=2/2 commands=1/1 events=1/1 fences=1/1 submits=1 executions=1 idle=0", self.cleanup_output)

    def test_submit_loader_contract(self):
        with self.broker() as path:
            env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(self.manifest),
                       VK_ICD_FILENAMES=str(self.manifest), MALI_VULKAN_SUBMIT_TEST="1", VK_LOADER_LAYERS_DISABLE="*")
            result = subprocess.run([str(self.directory / "submit_contract")], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("submit contracts:", result.stdout)
        self.assertIn("pools=1/1 commands=1/1 events=1/1 fences=1/1 submits=1 executions=1 idle=0", self.cleanup_output)

    def test_submit_complete_sequence_and_double_free(self):
        with self.broker() as path:
            c, d = self.submit_session(path)
            q, p, b, e, f = self.submit_objects(c, d)
            self.submit_rpc(c, 25, d, e, expected=(0, 4, 0))
            self.submit_rpc(c, 29, d, f, expected=(0, 1, 0))
            self.submit_record(c, d, b, e)
            self.submit_rpc(c, 25, d, e, expected=(0, 4, 0))
            self.submit_rpc(c, 31, d, q, b, f)
            self.submit_rpc(c, 30, d, f, 1, suffix=struct.pack("<Q", 5000000000))
            self.submit_rpc(c, 29, d, f)
            self.submit_rpc(c, 25, d, e, expected=(0, 3, 0))
            for op, fields in ((28, (d, f)), (24, (d, e)), (20, (d, p, b)), (18, (d, p))):
                self.submit_rpc(c, op, *fields)
                self.submit_rpc(c, op, *fields, expected=(3, 0, 0))
            self.submit_rpc(c, 16, d)
            self.assertEqual(rpc(c, 3, version=5)[0], (0, 0, 0))
        self.assertIn("submits=1 executions=1 idle=0", self.cleanup_output)

    def test_submit_invalid_ids_parents_and_states(self):
        with self.broker() as path:
            c, d = self.submit_session(path)
            q, p, b, e, f = self.submit_objects(c, d)
            prefix, data = rpc(c, 14, self.device_request(family=1), version=5)
            self.assertEqual(prefix, (0, 0, 1)); d2 = struct.unpack("<I", data)[0]
            q2 = self.submit_rpc(c, 15, d2, 1, 0, expected=(0, 0, 1))
            p2 = self.submit_rpc(c, 17, d2, 1, 0, expected=(0, 0, 1))
            b2 = self.submit_rpc(c, 19, d2, p2, 0, 1, expected=(0, 0, 1))
            cases = [(17, (999, 0, 0)), (17, (d, 1, 0)), (19, (d, 999, 0, 1)),
                     (19, (d2, p, 0, 1)), (19, (d, p, 1, 1)), (19, (d, p, 0, 2)),
                     (21, (d, 999, 0)), (21, (d2, b, 0)), (22, (d, b)),
                     (25, (d, 999)), (25, (d2, e)), (29, (d, 999)), (29, (d2, f)),
                     (31, (d, 999, b, f)), (31, (d, q2, b, f)), (31, (d, q, b2, f)),
                     (31, (d, q, b, 999)), (31, (d, q, b, f)), (20, (d, p2, b)),
                     (21, (d, e, 0)), (25, (d, p))]
            for op, fields in cases:
                self.submit_rpc(c, op, *fields, expected=(3, 0, 0))
            self.submit_rpc(c, 21, d, b, 0)
            self.submit_rpc(c, 21, d, b, 0, expected=(3, 0, 0))
            self.submit_rpc(c, 26, d, b, 999, 0x10000, expected=(3, 0, 0))
            self.submit_rpc(c, 26, d2, b, e, 0x10000, expected=(3, 0, 0))
            self.submit_rpc(c, 26, d, b, e, 0x4000, expected=(3, 0, 0))
            self.submit_rpc(c, 26, d, b, e, 0x10000)
            self.submit_rpc(c, 26, d, b, e, 0x10000, expected=(3, 0, 0))
            self.submit_rpc(c, 22, d, b)
            self.submit_rpc(c, 31, d, q, b, f)
            for op, fields in ((20, (d, p, b)), (18, (d, p)), (24, (d, e)), (28, (d, f)), (31, (d, q, b, f))):
                self.submit_rpc(c, op, *fields, expected=(3, 0, 0))
            self.submit_rpc(c, 16, d) # drains pending submission before child teardown
            self.submit_rpc(c, 16, d2)
        self.assertIn("submits=1 executions=1 idle=1", self.cleanup_output)

    def test_submit_malformed_lengths_flags_and_truncation(self):
        with self.broker() as path:
            c, d = self.submit_session(path)
            q, p, b, e, f = self.submit_objects(c, d)
            for op, fields in ((17, (d, 0, 1)), (21, (d, b, 1)), (23, (d, 1)), (27, (d, 1))):
                self.submit_rpc(c, op, *fields, expected=(3, 0, 0))
            for op, payload in ((17, struct.pack("<2I", d, 0)), (19, struct.pack("<5I", d, p, 0, 1, 99)),
                                (31, struct.pack("<3I", d, q, b)), (30, struct.pack("<3IQ", d, f, 2, 0)),
                                (30, struct.pack("<3IQ", d, f, 1, 2**64 - 1))):
                self.assertEqual(rpc(c, op, payload, version=5)[0], (3, 0, 0))
            c.sendall(struct.pack("<4I", MAGIC, 5, 31, 16) + b"\x01\x00")
            c.shutdown(socket.SHUT_WR)
            self.assertEqual(read_exact(c, 16), struct.pack("<4I", MAGIC, 5, 31, 12))
            self.assertEqual(struct.unpack("<3I", read_exact(c, 12)), (3, 0, 0))
            self.assertEqual(c.recv(1), b"")
        self.assertIn("pools=1/1 commands=1/1 events=1/1 fences=1/1", self.cleanup_output)

    def test_submit_timeout_and_device_loss_cleanup(self):
        for mode, expected in ((22, 2), (23, -4), (24, -4), (27, 2)):
            with self.subTest(mode=mode), self.broker(mode) as path:
                c, d = self.submit_session(path)
                q, p, b, e, f = self.submit_objects(c, d)
                self.submit_record(c, d, b, e)
                self.submit_rpc(c, 31, d, q, b, f, expected=(2, (-4) & 0xffffffff, 0) if mode == 23 else (0, 0, 0))
                if mode != 23:
                    self.submit_rpc(c, 30, d, f, 1, suffix=struct.pack("<Q", 5000000000),
                                    expected=(2 if expected < 0 else 0, expected & 0xffffffff, 0))
                self.submit_rpc(c, 16, d)
            self.assertIn("pools=1/1 commands=1/1 events=1/1 fences=1/1", self.cleanup_output)
            self.assertIn("idle=" + ("2" if mode == 27 else "1" if mode in (22, 24) else "0"), self.cleanup_output)

    def test_submit_disconnect_stop_and_optional_fence(self):
        with self.broker() as path:
            connections = []
            for index in range(3):
                c, d = self.submit_session(path); connections.append(c)
                q, p, b, e, f = self.submit_objects(c, d)
                self.submit_record(c, d, b, e)
                self.submit_rpc(c, 31, d, q, b, f if index else 0)
            connections[0].close()
            self.assertEqual(rpc(connections[1], 3, version=5)[0], (0, 0, 0))
            # third pending device is reclaimed by broker stop
        self.assertIn("pools=3/3 commands=3/3 events=3/3 fences=3/3 submits=3 executions=3 idle=3", self.cleanup_output)

    def test_submit_event_destroy_invalidates_command_and_pool_frees_children(self):
        with self.broker() as path:
            c, d = self.submit_session(path)
            q, p, b, e, f = self.submit_objects(c, d)
            self.submit_record(c, d, b, e)
            self.submit_rpc(c, 24, d, e)
            self.submit_rpc(c, 31, d, q, b, f, expected=(3, 0, 0))
            self.submit_rpc(c, 18, d, p)
            self.submit_rpc(c, 21, d, b, 0, expected=(3, 0, 0))
            new_pool = self.submit_rpc(c, 17, d, 0, 0, expected=(0, 0, 1))
            self.assertGreater(new_pool, f) # IDs never reused after destruction
            self.submit_rpc(c, 16, d)
        self.assertIn("pools=2/2 commands=1/1 events=1/1 fences=1/1 submits=0 executions=0", self.cleanup_output)

    def test_device_loader_contract(self):
        with self.broker() as path:
            result = self.run_program("device_contract", path, device=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("device contracts:", result.stdout)
        self.assertIn("DEVICES creates=3 destroys=3 queues=3", self.cleanup_output)

    def device_request(self, physical=1, family=0, feature=-1, extension=None):
        features = [int(i == feature) for i in range(55)]
        return (struct.pack("<4I", physical, family, 1, int(extension is not None)) +
                struct.pack("<55I", *features) + struct.pack("<f", 1.0) +
                (extension.encode().ljust(256, b"\0") if extension is not None else b""))

    def device_session(self, path):
        connection = self.connect(path)
        self.assertEqual(rpc(connection, 2, struct.pack("<I", 1 << 22), version=4)[0], (0, 0, 0))
        self.assertEqual(rpc(connection, 4, version=4)[0], (0, 0, 1))
        return connection

    def test_device_protocol_rejections_and_stable_ids(self):
        with self.broker() as path:
            connection = self.device_session(path)
            for request, status, result in (
                (self.device_request(physical=99), 3, 0),
                (self.device_request(family=99), 3, 0),
                (self.device_request(extension="HOST_UNSUPPORTED"), 2, -7),
                (self.device_request(feature=22), 2, -8), # textureCompressionBC
                (self.device_request()[:-1], 3, 0),
                (self.device_request() + b"x", 3, 0),
            ):
                self.assertEqual(rpc(connection, 14, request, version=4)[0], (status, result & 0xffffffff, 0))
            for op, payload in ((15, struct.pack("<3I", 99, 0, 0)), (16, struct.pack("<I", 99))):
                self.assertEqual(rpc(connection, op, payload, version=4)[0], (3, 0, 0))
            for expected in (1, 2):
                # Byte-at-a-time request fragmentation, supported extension + supported feature.
                request = self.device_request(feature=19, extension="VK_KHR_external_memory_fd")
                header = struct.pack("<4I", MAGIC, 4, 14, len(request))
                for byte in header + request:
                    connection.sendall(bytes([byte]))
                self.assertEqual(struct.unpack("<4I", read_exact(connection, 16)), (MAGIC, 4, 14, 16))
                self.assertEqual(struct.unpack("<4I", read_exact(connection, 16)), (0, 0, 1, expected))
                for family, index in ((99, 0), (0, 99)):
                    self.assertEqual(rpc(connection, 15, struct.pack("<3I", expected, family, index), version=4)[0], (3, 0, 0))
                for repeat in range(2):
                    prefix, queue = rpc(connection, 15, struct.pack("<3I", expected, 0, 0), version=4)
                    self.assertEqual(prefix, (0, 0, 1)); self.assertEqual(queue, struct.pack("<I", expected))
                self.assertEqual(rpc(connection, 16, struct.pack("<I", expected), version=4)[0], (0, 0, 0))
                self.assertEqual(rpc(connection, 15, struct.pack("<3I", expected, 0, 0), version=4)[0], (3, 0, 0))
            self.assertEqual(rpc(connection, 3, version=4)[0], (0, 0, 0))
        self.assertIn("DEVICES creates=2 destroys=2 queues=2", self.cleanup_output)

    def test_device_disconnect_stop_and_instance_cleanup(self):
        with self.broker() as path:
            connections = [self.device_session(path) for _ in range(3)]
            for c in connections:
                self.assertEqual(rpc(c, 14, self.device_request(), version=4)[0], (0, 0, 1))
            connections[0].close()
            self.assertEqual(rpc(connections[1], 3, version=4)[0], (0, 0, 0))
            # Third live device/instance is reclaimed by broker shutdown.
        self.assertIn("DEVICES creates=3 destroys=3 queues=0", self.cleanup_output)
        self.assertIn("creates=3 destroys=3 closes=3", self.cleanup_output)

    def test_device_backend_errors_and_cleanup(self):
        for mode in (9, 10, 11):
            with self.subTest(mode=mode), self.broker(mode) as path:
                c = self.device_session(path)
                prefix, _ = rpc(c, 14, self.device_request(), version=4)
                if mode == 10:
                    self.assertEqual(prefix, (0, 0, 1))
                    self.assertEqual(rpc(c, 15, struct.pack("<3I", 1, 0, 0), version=4)[0], (2, (-3) & 0xffffffff, 0))
                else:
                    self.assertNotEqual(prefix[0], 0)
                self.assertEqual(rpc(c, 3, version=4)[0], (0, 0, 0))
            self.assertIn("DEVICES creates=0 destroys=0" if mode == 9 else "DEVICES creates=1 destroys=1", self.cleanup_output)

    def test_session_version_cannot_upgrade(self):
        for version in (2, 3):
            with self.broker() as path:
                c = self.connect(path)
                self.assertEqual(rpc(c, 2, struct.pack("<I", 1 << 22), version=version)[0], (0, 0, 0))
                self.assertEqual(rpc(c, 4, version=version)[0], (0, 0, 1))
                self.assertEqual(rpc(c, 14, self.device_request(), version=4)[0], (3, 0, 0))
                try:
                    self.assertEqual(c.recv(1), b"")
                except ConnectionResetError:
                    pass # Closing with an unread rejected payload may reset the socket.
            self.assertIn("DEVICES creates=0 destroys=0", self.cleanup_output)

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
