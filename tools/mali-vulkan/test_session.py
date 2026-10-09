#!/usr/bin/env python3
"""Checkpoint 6 actual Gamescope/headless/wlroots execution; host GPU MODEL ONLY."""
import os
from pathlib import Path
import shlex
import signal
import re
import struct
import subprocess
import unittest
import test_renderer

ROOT = Path(__file__).resolve().parent

class SessionTests(test_renderer.GamescopeRendererFixture):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        build = cls.directory / "renderer-build"
        wlr_build = Path(os.environ["WLR_BUILD"])
        compiler = shlex.split(os.environ.get("HOST_CXX", "c++"))
        flags = ["-std=c++20", "-O0", "-fno-exceptions", "-ffunction-sections", "-fdata-sections", "-DWLR_USE_UNSTABLE", "-DHAVE_DRM=1", "-I" + str(build), "-I" + os.environ.get("VULKAN_HEADERS", "/usr/include"), "-I/usr/include/libdrm", "-I/usr/include/pixman-1", "-I" + os.environ["WLR_HEADERS"], "-I" + str(wlr_build / "protocol"), "-I" + str(cls.source / "src")]
        objects = [str(build / "rendervulkan.o")]
        for src in (cls.source / "src/backend.cpp", cls.source / "src/Backends/HeadlessBackend.cpp", cls.source / "src/mali_session_wayland.cpp", ROOT / "tests/session_host.cpp"):
            obj = build / (src.stem + ".o"); objects.append(str(obj))
            subprocess.run(compiler + flags + ["-c", str(src), "-o", str(obj)], check=True)
        subprocess.run(compiler + flags + [str(ROOT / "tests/session_pool.cpp"), "-o", str(build / "session-pool")], check=True)
        subprocess.run([str(build / "session-pool")], check=True)
        cls.binary = build / "session-test"
        subprocess.run(compiler + ["-Wl,--gc-sections", *objects, "-L" + str(wlr_build), "-Wl,-rpath," + str(wlr_build), "-lwlroots-0.20", "-lwayland-server", "-lwayland-client", "-ldl", "-pthread", "-ldrm", "-o", str(cls.binary)], check=True)
        cc = shlex.split(os.environ.get("HOST_CC", "cc"))
        java = Path(os.environ.get("JAVA_HOME", "/usr/lib/jvm/default"))
        cls.consumer = build / "consumer-release-contract"
        subprocess.run(cc + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/consumer_contract.c"), "-I" + str(ROOT / "tests"), "-I" + str(java / "include"), "-I" + str(java / "include/linux"), "-pthread", "-o", str(cls.consumer)], check=True)

    def run_session(self, path, wayland=False, frames=65):
        env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(self.manifest), VK_ICD_FILENAMES=str(self.manifest), VK_LOADER_LAYERS_DISABLE="*", XDG_RUNTIME_DIR=str(self.directory), MALI_SESSION_FRAMES=str(frames))
        if frames is None: env.pop("MALI_SESSION_FRAMES", None)
        return subprocess.run([str(self.binary)] + (["wayland"] if wayland else []), env=env, capture_output=True, text=True, timeout=40)

    def test_persistent_backend_repeated_start_stop(self):
        with self.broker(43) as path:
            for repeat in range(2):
                result = self.run_session(path)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("persistent Gamescope session PASS", result.stdout)
                self.assertIn("frames rendered=65 presented=65 released=65", result.stdout)
                self.assertIn("sync leaks=0", result.stdout)
                self.assertIn("max Android-owned=2", result.stdout)
                self.assertIn("Android release wait: outstanding at stop=1 released=1 timed out=0", result.stdout)
                self.assertIn("final session accounting: release timeouts=0", result.stdout)
                for frame in (1,30,60,65): self.assertIn(f"selected frame={frame} actual AHB GPU readback mismatches=0", result.stdout)
        self.assertIn("baseline: devices=0 queues=0", self.broker_errors)

    def test_real_wayland_commits_callbacks_releases(self):
        with self.broker(43) as path:
            for repeat in range(2):
                result = self.run_session(path, True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("persistent Gamescope Wayland client PASS", result.stdout)
                self.assertIn("commits=65 frame callbacks=65 wl_buffer releases=65 pending callback=0", result.stdout)
                self.assertIn("surface removed=1 client status=0", result.stdout)
                self.assertIn("final session accounting: release timeouts=0", result.stdout)

    def test_native_release_wait_normal_and_delayed_before_deadline(self):
        for scenario in ("normal", "delayed"):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.consumer), scenario], capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("outstanding at stop=1 released=1 timed out=0", result.stdout)

    def test_native_missing_release_and_callback_are_bounded_and_retained(self):
        for scenario in ("missing", "callback", "allocation"):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.consumer), scenario], capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("TIMEOUT: retained producer, no reuse, no fake release; sticky failure", result.stdout)

    def test_release_timeout_fails_both_sessions_without_recycling(self):
        for wayland in (False, True):
            with self.subTest(wayland=wayland), self.broker(68) as path:
                result = self.run_session(path, wayland, 6)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("frame=6 state=ANDROID_OWNED outstanding releases=1 presented=6 released=5", result.stderr)
                self.assertIn("outstanding at stop=1 released=0 timed out=1; release timeouts=1", result.stdout)
                self.assertIn("release timeouts=1", result.stderr)
                self.assertNotIn("PASS", result.stdout)
            self.assertIn("state=ANDROID_OWNED outstanding releases=1 presented=6 released=5 last sync token=", self.broker_errors)
            self.assertIn("sync live=0", self.broker_errors)
            self.assertIn("failed session quarantined", self.broker_errors)
            self.assertIn("release TIMEOUT safety contract", self.cleanup_output)

    def test_stop_after_inflight_swap_timeout_retains_both_buffers(self):
        result = subprocess.run([str(self.consumer), "swap"], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("swap timeout: both AHBs retained; no release, no reuse; bounded stop", result.stdout)
        with self.broker(70) as path:
            result = self.run_session(path, True, 12)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertNotIn("PASS", result.stdout)
        self.assertIn("frame=5 state=ANDROID_OWNED outstanding releases=2", self.broker_errors)
        self.assertIn("frame=6 state=ANDROID_OWNED outstanding releases=2", self.broker_errors)
        self.assertIn("outstanding at stop=2 released=0 timed out=2", self.broker_errors)
        self.assertIn("release TIMEOUT safety contract", self.cleanup_output)

    def test_default_frame_counts(self):
        for wayland, count in ((False,300),(True,150)):
            with self.subTest(wayland=wayland), self.broker(43) as path:
                result = self.run_session(path, wayland, None)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"frames rendered={count} presented={count} released={count}", result.stdout)
                self.assertIn(f"selected frame={count} actual AHB GPU readback mismatches=0", result.stdout)

    def test_failed_device_destroy_acknowledgement_cannot_pass(self):
        with self.broker(67) as path:
            result = self.run_session(path, False, 6)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("native device destroy acknowledgement", result.stdout + result.stderr)
            self.assertNotIn("persistent Gamescope session PASS", result.stdout)
        self.assertIn("baseline: devices=0 queues=0", self.broker_errors)

    def test_late_consumer_timeout_preview_loss_and_device_loss(self):
        for mode, code in ((63,-3),(64,2),(65,-4),(66,-4)):
            with self.subTest(mode=mode), self.broker(mode) as path:
                result = self.run_session(path, True, 12)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("frame=3 timeline", result.stdout)
                self.assertIn(f"VkResult={code}", result.stdout + result.stderr)
                self.assertNotIn("client PASS", result.stdout)
        self.assertIn("baseline: devices=0 queues=0", self.broker_errors)

    def test_user_stop_client_and_broker_disconnect(self):
        for action in ("stop", "client", "broker"):
            with self.subTest(action=action), self.broker(43) as path:
                env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(self.manifest), VK_ICD_FILENAMES=str(self.manifest), VK_LOADER_LAYERS_DISABLE="*", XDG_RUNTIME_DIR=str(self.directory), MALI_SESSION_FRAMES="150")
                process = subprocess.Popen([str(self.binary), "wayland"], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                lines = []; child = None
                try:
                    while True:
                        line = process.stdout.readline(); lines.append(line)
                        self.assertTrue(line, ''.join(lines))
                        match = re.search(r"client pid=(\d+)", line)
                        if match: child = int(match[1])
                        if "frame=3 timeline" in line: break
                    if action == "broker": self.broker_process.stdin.write("\n"); self.broker_process.stdin.flush()
                    else: os.kill(child if action == "client" else process.pid, signal.SIGKILL if action == "client" else signal.SIGTERM)
                    output, errors = process.communicate(timeout=15)
                    output = ''.join(lines) + output
                    self.assertEqual(process.returncode, 1, output + errors)
                    self.assertNotIn("client PASS", output)
                    if action == "client": self.assertIn("Wayland commit failed", errors)
                    elif action == "broker": self.assertIn("socket/protocol failure", errors)
                    else: self.assertIn("STOPPED by user; clean teardown", output)
                finally:
                    if process.poll() is None: process.kill(); process.communicate()
            self.assertIn("baseline: devices=0 queues=0", self.broker_errors)

class SessionBrokerTests(test_renderer.RendererFixture):
    def open_session(self, path):
        c = self.connect(path)
        self.fields(c, 2, 1 << 22); self.call(c, 4, expected=(0,0,1))
        request = self.request()[:-16] + struct.pack("<4I", 1,1,1,0)
        return c, struct.unpack("<I", self.call(c, 14, request, (0,0,1)))[0]

    def test_counts_and_opt_in_lifecycle(self):
        with self.broker(43) as path:
            c,d = self.open_session(path)
            self.fields(c, 83, d)
            counts = struct.unpack("<25I", self.fields(c,86,d,expected=(0,0,1)))
            self.assertEqual(counts[:20], (1,) + (0,) * 19)
            queue = self.new(c,15,d,0,0)
            counts = struct.unpack("<25I", self.fields(c,86,d,expected=(0,0,1)))
            self.assertEqual(counts[:2], (1,1)); self.assertTrue(queue)
            self.assertEqual(self.fields(c,85,d,expected=(0,0,1)), bytes(4))
            self.fields(c,16,d)
        self.assertIn("baseline: devices=0 queues=0",self.broker_errors)

    def test_session_wrong_parent_stale_token_and_no_upgrade(self):
        for case in ("not begun", "wrong device", "stale AHB", "legacy"):
            with self.subTest(case=case), self.broker(43) as path:
                c,d = self.open_session(path)
                if case == "not begun": self.fields(c,86,d,expected=(3,0,0))
                elif case == "legacy":
                    self.assertEqual(__import__('test_icd').rpc(c,83,struct.pack('<I',d),version=6)[0],(3,0,0))
                else:
                    self.fields(c,83,d)
                    if case == "wrong device": self.fields(c,86,d+10000,expected=(3,0,0))
                    else: self.fields(c,84,d,99999,99998,expected=(3,0,0))
                self.assert_disconnected(c)
