#!/usr/bin/env python3
"""Checkpoint 5: production transport + actual Gamescope renderer, HOST MOCK ONLY.

Set GAMESCOPE_SOURCE to pristine 3.16.29 and WLR_HEADERS to its pinned wlroots
include directory (d783533489e1f75d6886c2ab5c5960090ef268f8). Uses system
C++20, glslangValidator, wayland-scanner, glm, libdrm/pixman/Wayland headers.
No mock result is evidence of phone Vulkan execution.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import unittest
import test_icd as fixture

ROOT = Path(__file__).resolve().parent
GAMESCOPE = ROOT.parent / "gamescope"


class RendererFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture.IcdTests.setUpClass()
        cls.addClassCleanup(fixture.IcdTests.doClassCleanups)
        cls.directory = fixture.IcdTests.directory
        cls.manifest = fixture.IcdTests.manifest
        cls.broker_errors = ""
        compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
        subprocess.run(compiler + ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + os.environ.get("VULKAN_HEADERS", "/usr/include"), str(ROOT / "tests/renderer_contract.c"), "-l:libvulkan.so.1", "-o", str(cls.directory / "renderer_contract")], check=True)

    broker = fixture.IcdTests.broker
    connect = fixture.IcdTests.connect

    def assert_disconnected(self, c):
        try: self.assertEqual(c.recv(1), b"")
        except ConnectionResetError: pass

    def call(self, c, op, payload=b"", expected=(0, 0, 0)):
        prefix, data = fixture.rpc(c, op, payload, version=7)
        self.assertEqual(prefix, expected, (op, prefix))
        return data

    def fields(self, c, op, *values, expected=(0, 0, 0), suffix=b""):
        return self.call(c, op, struct.pack("<" + "I" * len(values), *values) + suffix, expected)

    def new(self, c, op, *values, suffix=b""):
        return struct.unpack("<I", self.fields(c, op, *values, suffix=suffix, expected=(0, 0, 1)))[0]

    def request(self):
        extensions = ["VK_KHR_timeline_semaphore", "VK_EXT_scalar_block_layout", "VK_KHR_image_format_list"]
        return (struct.pack("<4I", 1, 0, 1, 3) + bytes(55 * 4) + struct.pack("<f", 1) +
                b"".join(s.encode().ljust(256, b"\0") for s in extensions) + struct.pack("<4I", 0, 1, 1, 0))

    def session(self, path):
        c = self.connect(path)
        self.fields(c, 2, 1 << 22)
        self.call(c, 4, expected=(0, 0, 1))
        return c, struct.unpack("<I", self.call(c, 14, self.request(), (0, 0, 1)))[0]


class RendererTransportTests(RendererFixture):
    def test_normal_loader_aliases_and_unknown_pnext_rejection(self):
        with self.broker(43) as path:
            env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(self.manifest), VK_ICD_FILENAMES=str(self.manifest), VK_LOADER_LAYERS_DISABLE="*")
            result = subprocess.run([str(self.directory / "renderer_contract")], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("renderer contracts:", result.stdout)

    def test_native_timeline_has_no_cpu_progress_and_real_submit_wait(self):
        with self.broker(43) as path:
            c, d = self.session(path)
            sem = self.new(c, 60, d, suffix=struct.pack("<Q", 0))
            self.assertEqual(struct.unpack("<Q", self.fields(c, 62, d, sem, expected=(0, 0, 1)))[0], 0)
            self.fields(c, 63, d, sem, suffix=struct.pack("<QQ", 1, 0), expected=(0, 2, 0))
            image = self.new(c, 78, d, 1, 1, 1, 1, 2, 0, 0)
            memory = self.new(c, 35, d, suffix=struct.pack("<QI", 256, 0))
            self.fields(c, 47, d, image, memory, suffix=struct.pack("<Q", 0))
            queue = self.new(c, 15, d, 0, 0)
            pool = self.new(c, 17, d, 0, 2)
            command = self.new(c, 19, d, pool, 0, 1)
            self.fields(c, 21, d, command, 1)
            self.fields(c, 79, d, command, 1, 0x1000, 1, image, 0, 0x1000, 0, 1, 0xffffffff, 0xffffffff)
            self.fields(c, 81, d, command, image, 1, suffix=bytes(16))
            self.fields(c, 22, d, command)
            self.fields(c, 76, d, queue, command, 0, sem, suffix=struct.pack("<Q", 2))
            self.assertEqual(struct.unpack("<Q", self.fields(c, 62, d, sem, expected=(0, 0, 1)))[0], 0)
            self.fields(c, 63, d, sem, suffix=struct.pack("<QQ", 2, 5000000000))
            self.assertEqual(struct.unpack("<Q", self.fields(c, 62, d, sem, expected=(0, 0, 1)))[0], 2)
            self.fields(c, 77, d, command)
            self.fields(c, 61, d, 1, sem)
            self.fields(c, 16, d)
        self.assertIn("submits=1 executions=1", self.cleanup_output)

    def test_wrong_parent_stale_and_wrong_type_timeline_ids(self):
        for wrong in ("device", "type", "stale", "unknown"):
            with self.subTest(wrong=wrong), self.broker(43) as path:
                c, d = self.session(path)
                sem = self.new(c, 60, d, suffix=struct.pack("<Q", 7))
                owner = d
                if wrong == "device": owner = struct.unpack("<I", self.call(c, 14, self.request(), (0, 0, 1)))[0]
                if wrong == "type": sem = self.new(c, 65, d, 1, 0)
                if wrong == "stale": self.fields(c, 61, d, 1, sem)
                if wrong == "unknown": sem = 99999
                self.fields(c, 62, owner, sem, expected=(3, 0, 0))
                self.assert_disconnected(c)

    def test_shader_bounds_alignment_and_malformed_spirv(self):
        for code in (bytes(19), struct.pack("<5I", 0, 0x10000, 0, 1, 0), struct.pack("<5I", 0x07230203, 0x10400, 0, 1, 0), bytes(524292)):
            with self.subTest(length=len(code)), self.broker(43) as path:
                c, d = self.session(path)
                self.call(c, 71, struct.pack("<II", d, len(code)) + code, (3, 0, 0))
                self.assert_disconnected(c)

    def test_native_capability_chain_is_checked_again_by_broker(self):
        for mode, result in ((44, -7), (45, -7), (46, -8), (47, -8)):
            with self.subTest(mode=mode), self.broker(mode) as path:
                c = self.connect(path)
                self.fields(c, 2, 1 << 22); self.call(c, 4, expected=(0, 0, 1))
                self.call(c, 14, self.request(), (2, result & 0xffffffff, 0))
        self.assertIn("DEVICES creates=0 destroys=0", self.cleanup_output)

        with self.broker(59) as path:
            c, d = self.session(path)
            sampler = self.new(c, 65, d, 1, 0)
            bindings = b"".join(struct.pack("<4I", i, 6 if i == 0 else 3 if i < 3 else 1, 1 if i < 3 else 16 if i < 5 else 2, sampler if i == 4 else 0) for i in range(7))
            layout = self.new(c, 66, d, 7, suffix=bindings)
            self.fields(c, 67, d, layout, expected=(2, (-8) & 0xffffffff, 0))
            self.fields(c, 16, d)

    def test_v7_cannot_be_upgraded_and_legacy_cannot_reach_renderer(self):
        with self.broker(43) as path:
            c, d = self.session(path)
            self.assertEqual(fixture.rpc(c, 60, struct.pack("<IQ", d, 0), version=6)[0], (3, 0, 0))
            self.assert_disconnected(c)
        with self.broker() as path:
            c = self.connect(path)
            self.assertEqual(fixture.rpc(c, 2, struct.pack("<I", 1 << 22), version=6)[0], (0, 0, 0))
            self.assertEqual(fixture.rpc(c, 60, bytes(12), version=6)[0], (3, 0, 0))

    def test_wrong_image_view_shape_and_immutable_sampler_lifetime(self):
        with self.broker(43) as path:
            c, d = self.session(path)
            image = self.new(c, 78, d, 0, 1, 1, 1, 6, 0, 0)
            mem = self.new(c, 35, d, suffix=struct.pack("<QI", 256, 0))
            self.fields(c, 47, d, image, mem, suffix=bytes(8))
            self.fields(c, 64, d, image, 1, 37, 4, expected=(3, 0, 0))
        with self.broker(43) as path:
            c, d = self.session(path)
            sampler = self.new(c, 65, d, 1, 0)
            bindings = b"".join(struct.pack("<4I", i, 6 if i == 0 else 3 if i < 3 else 1, 1 if i < 3 else 16 if i < 5 else 2, sampler if i == 4 else 0) for i in range(7))
            self.new(c, 66, d, 7, suffix=bindings)
            self.fields(c, 61, d, 3, sampler, expected=(3, 0, 0))


class GamescopeRendererTests(RendererFixture):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        source = os.environ.get("GAMESCOPE_SOURCE")
        if not source: raise RuntimeError("GAMESCOPE_SOURCE must name pristine 3.16.29")
        cls.source = cls.directory / "gamescope"
        shutil.copytree(source, cls.source, ignore=shutil.ignore_patterns(".git", "build", "builddir"))
        for patch in sorted((GAMESCOPE / "patches").glob("*.patch")):
            subprocess.run(["patch", "-p1", "--batch", "--fuzz=0", "-i", str(patch)], cwd=cls.source, check=True, capture_output=True)
        build = cls.directory / "renderer-build"; build.mkdir(); (build / "wlr").mkdir()
        (build / "config.h").write_text("#define HAVE_PIPEWIRE 0\n#define HAVE_OPENVR 0\n#define HAVE_SDL2 0\n")
        (build / "wlr/config.h").write_text("#define WLR_HAS_XWAYLAND 1\n#define WLR_HAS_SESSION 0\n")
        shader_names = re.findall(r"'shaders/(cs_[^']+)\.comp'", (cls.source / "src/meson.build").read_text())
        for name in shader_names:
            subprocess.run(["glslangValidator", "-V", str(cls.source / f"src/shaders/{name}.comp"), "--vn", name, "-o", str(build / f"{name}.h"), "--quiet"], check=True)
        for xml in (cls.source / "protocol").glob("*.xml"):
            subprocess.run(["wayland-scanner", "server-header", str(xml), str(build / f"{xml.stem}-protocol.h")], check=True, capture_output=True)
        cls.shader = b"".join(struct.pack("<I", int(word, 16)) for word in re.findall(r"0x[0-9a-fA-F]+", (build / "cs_composite_blit.h").read_text()))
        compiler = shlex.split(os.environ.get("HOST_CXX", "c++"))
        headers = os.environ.get("VULKAN_HEADERS", "/usr/include")
        wlr = os.environ.get("WLR_HEADERS", str(cls.source / "subprojects/wlroots/include"))
        flags = ["-std=c++20", "-O0", "-fno-exceptions", "-ffunction-sections", "-fdata-sections", "-DWLR_USE_UNSTABLE", "-DHAVE_DRM=1", "-I" + str(build), "-I" + headers, "-I/usr/include/libdrm", "-I/usr/include/pixman-1", "-I" + wlr, "-I" + str(cls.source / "src")]
        objects = []
        for src in (cls.source / "src/rendervulkan.cpp", ROOT / "tests/renderer_host.cpp"):
            obj = build / (src.stem + ".o"); objects.append(str(obj))
            subprocess.run(compiler + flags + ["-c", str(src), "-o", str(obj)], check=True)
        cls.binary = build / "renderer-test"
        subprocess.run(compiler + ["-Wl,--gc-sections", *objects, "-ldl", "-pthread", "-ldrm", "-o", str(cls.binary)], check=True)

    def run_renderer(self, path, frame=False):
        env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(self.manifest), VK_ICD_FILENAMES=str(self.manifest), VK_LOADER_LAYERS_DISABLE="*", DISPLAY="invalid-for-renderer", WAYLAND_DISPLAY="invalid-for-renderer")
        return subprocess.run([str(self.binary)] + (["frame"] if frame else []), env=env, capture_output=True, text=True, timeout=40)

    def test_actual_renderer_initialization_no_backend(self):
        with self.broker(43) as path:
            result = self.run_renderer(path)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for text in ("physical API: 1.1.131", "no Vulkan-version spoofing", "deliberately disabled", "VK_EXT_robustness2: absent", "initialized on GPU", "Gamescope renderer initialized: yes", "no backend/presentation started", "clean renderer teardown"):
                self.assertIn(text, result.stdout)
        self.assertIn("AHB=0/0 consumers=0", self.cleanup_output)
        self.assertIn("objects=38/38 dispatches=0", self.cleanup_output)

    def test_actual_compositor_frame_repeated_exact_ahb_and_all_pixels(self):
        with self.broker(43) as path:
            for repeat in range(2):
                result = self.run_renderer(path, True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for text in ("vulkan_composite -> BLIT -> CVulkanCmdBuffer::dispatch", "sequence=3 counter=3", "RGBA=(255,0,0,255)", "RGBA=(0,255,0,255)", "RGBA=(0,0,255,255)", "RGBA=(255,255,255,255)", "RGBA=(0,0,0,255)", "mismatches=0 source-vs-final differing pixels=49152", "Gamescope first frame PASS", "clean renderer teardown"):
                    self.assertIn(text, result.stdout)
        self.assertIn("AHB=2/2 consumers=2 CPU=2", self.cleanup_output)
        self.assertIn("objects=86/86 dispatches=2", self.cleanup_output)

    def test_extension_and_feature_failures_preserve_real_vkresult(self):
        for mode, code in ((59, -8), (60, -8), (44, -7), (45, -7), (46, -8), (47, -8), (48, -2), (49, -3), (50, 2), (51, -1), (54, -4), (58, -4)):
            with self.subTest(mode=mode), self.broker(mode) as path:
                result = self.run_renderer(path)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertRegex(result.stdout + result.stderr, rf"VkResult[:=] ?{code}\b")
                self.assertNotIn("first frame PASS", result.stdout)
                if mode == 58: self.assertNotIn("clean renderer teardown", result.stdout)

    def test_ahb_storage_corruption_sync_and_consumer_failure_no_pass(self):
        for mode, code in ((52, -11), (53, -3), (56, -3), (57, -10), (58, -4), (61, -2)):
            with self.subTest(mode=mode), self.broker(mode) as path:
                result = self.run_renderer(path, True)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertRegex(result.stdout + result.stderr, rf"VkResult[:=] ?{code}\b")
                self.assertNotIn("Gamescope first frame PASS", result.stdout)
                if mode == 53: self.assertIn("mismatches=1", result.stdout)

    def test_gpu_only_ahb_uses_actual_same_image_readback(self):
        with self.broker(55) as path:
            result = self.run_renderer(path, True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("AHB CPU inspection checked=0; actual same-image GPU readback checked=1", result.stdout)
            self.assertIn("mismatches=0", result.stdout)
        self.assertIn("AHB=1/1 consumers=1 CPU=0", self.cleanup_output)

    def test_shader_bytes_preserved_and_normal_renderer_requirements_retained(self):
        # Independently calculate expected hash from exact glslang-generated bytes.
        h = 14695981039346656037
        for byte in self.shader: h = ((h ^ byte) * 1099511628211) & ((1 << 64) - 1)
        with self.broker(43) as path:
            c, d = self.session(path)
            shader = self.new(c, 71, d, len(self.shader), suffix=self.shader)
            self.fields(c, 61, d, 8, shader); self.fields(c, 16, d)
        self.assertIn(f"shaderBytes={len(self.shader)}", self.cleanup_output)
        self.assertIn(f"exact bytes={len(self.shader)} FNV64={h}", self.broker_errors)
        text = (self.source / "src/rendervulkan.cpp").read_text()
        self.assertTrue(re.search(r"if \(m_maliDiagnostic\)\s*return CreateMaliDiagnosticDevice\(\);", text))
        self.assertTrue("deviceProperties.apiVersion < VK_API_VERSION_1_2" in text)
        self.assertTrue("VK_EXT_ROBUSTNESS_2_EXTENSION_NAME" in text)
        self.assertTrue(".nullDescriptor = VK_TRUE" in text)
        self.assertTrue(".dynamicRendering = VK_TRUE" in text)


if __name__ == "__main__": unittest.main(verbosity=2)
