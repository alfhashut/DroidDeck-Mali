#!/usr/bin/env python3
"""Apply the complete DroidDeck patch stack to pristine Gamescope 3.16.29 and test its diagnostic.

GAMESCOPE_SOURCE must name that source tree. This builds the diagnostic translation unit;
the real packaged executable is smoke-tested by build-in-arch.sh, and on the phone.
"""
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
MALI = ROOT.parent / "mali-vulkan"


class EnumerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = os.environ.get("GAMESCOPE_SOURCE")
        if not source:
            raise RuntimeError("Set GAMESCOPE_SOURCE to a pristine Gamescope 3.16.29 source tree")
        cls.temporary = tempfile.TemporaryDirectory(prefix="gamescope-enum-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.source = cls.directory / "source"
        shutil.copytree(source, cls.source, ignore=shutil.ignore_patterns(".git", "build", "builddir"))
        cls.patch_output = ""
        for patch in sorted((ROOT / "patches").glob("*.patch")):
            if patch.name in ("0119-vulkan-blit-input-proof.patch", "0113-vulkan-enumerate-only.patch", "0114-vulkan-capabilities.patch", "0115-vulkan-create-device-test.patch", "0116-vulkan-submit-test.patch", "0117-vulkan-memory-ahb-tests.patch", "0118-vulkan-gamescope-renderer-tests.patch"):
                # Validate added source whitespace, excluding the patch's context prefixes.
                subprocess.run(["git", "apply", "--check", "--whitespace=error", str(patch)],
                               cwd=cls.source, check=True, capture_output=True)
            result = subprocess.run(["patch", "-p1", "--batch", "--fuzz=0", "--no-backup-if-mismatch",
                                     "-i", str(patch)], cwd=cls.source, capture_output=True, text=True, check=True)
            cls.patch_output += result.stdout
        cls.flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                     "-fno-exceptions", "-ffast-math", "-I" + str(cls.source / "src"),
                     "-I" + os.environ.get("VULKAN_HEADERS", "/usr/include")]
        cls.compiler = shlex.split(os.environ.get("CXX", "c++"))
        cls.unit = cls.source / "src/vulkan_enumerate_only.cpp"
        cls.main = cls.directory / "diagnostic_main.cpp"
        cls.main.write_text('#include "vulkan_enumerate_only.hpp"\nint main() { return vulkan_enumerate_only(); }\n')
        cls.object = cls.directory / "diagnostic.o"
        subprocess.run(cls.compiler + cls.flags + ["-c", str(cls.unit), "-o", str(cls.object)], check=True)
        cls.mock = cls.directory / "mock-diagnostic"
        subprocess.run(cls.compiler + cls.flags + [str(cls.object), str(cls.main),
                       str(ROOT / "tests/mock_enumeration.cpp"), "-o", str(cls.mock)], check=True)
        cls.normal = cls.directory / "normal-loader-diagnostic"
        subprocess.run(cls.compiler + cls.flags + [str(cls.object), str(cls.main), "-l:libvulkan.so.1",
                       "-o", str(cls.normal)], check=True)
        cls.cap_object = cls.directory / "capabilities.o"
        cap_main = cls.directory / "capabilities_main.cpp"
        cap_main.write_text('#include "vulkan_capabilities.hpp"\nint main() { return vulkan_capabilities(); }\n')
        subprocess.run(cls.compiler + cls.flags + ["-c", str(cls.source / "src/vulkan_capabilities.cpp"),
                       "-o", str(cls.cap_object)], check=True)
        cls.cap_normal = cls.directory / "capabilities-loader-diagnostic"
        subprocess.run(cls.compiler + cls.flags + [str(cls.cap_object), str(cap_main), "-l:libvulkan.so.1",
                       "-o", str(cls.cap_normal)], check=True)
        cls.device_object = cls.directory / "device.o"
        device_main = cls.directory / "device_main.cpp"
        device_main.write_text('#include "vulkan_device_test.hpp"\nint main() { return vulkan_create_device_test(); }\n')
        subprocess.run(cls.compiler + cls.flags + ["-c", str(cls.source / "src/vulkan_device_test.cpp"),
                       "-o", str(cls.device_object)], check=True)
        cls.device_normal = cls.directory / "device-loader-diagnostic"
        subprocess.run(cls.compiler + cls.flags + [str(cls.device_object), str(device_main), "-l:libvulkan.so.1",
                       "-o", str(cls.device_normal)], check=True)
        cls.submit_object = cls.directory / "submit.o"
        submit_main = cls.directory / "submit_main.cpp"
        submit_main.write_text('#include "vulkan_submit_test.hpp"\nint main() { return vulkan_submit_test(); }\n')
        subprocess.run(cls.compiler + cls.flags + ["-c", str(cls.source / "src/vulkan_submit_test.cpp"),
                       "-o", str(cls.submit_object)], check=True)
        cls.submit_normal = cls.directory / "submit-loader-diagnostic"
        subprocess.run(cls.compiler + cls.flags + [str(cls.submit_object), str(submit_main), "-l:libvulkan.so.1",
                       "-o", str(cls.submit_normal)], check=True)
        interop_main = cls.directory / "interop_main.cpp"
        interop_main.write_text('#include "vulkan_interop_test.hpp"\n#include <cstdlib>\nint main(int argc,char **argv) { if(argc!=2)return 2; switch(std::atoi(argv[1])) { case 1:return vulkan_buffer_memory_test(); case 2:return vulkan_image_memory_test(); case 3:return vulkan_ahb_test(); case 4:return vulkan_ahb_present_test(); } return 2; }\n')
        cls.interop_normal = cls.directory / "interop-loader-diagnostic"
        subprocess.run(cls.compiler + cls.flags + [str(cls.source / "src/vulkan_interop_test.cpp"),
                       str(interop_main), "-l:libvulkan.so.1", "-o", str(cls.interop_normal)], check=True)
        # Reuse checkpoint 2's actual proxy/broker host fixture without changing its tests.
        spec = importlib.util.spec_from_file_location("checkpoint2_fixture", MALI / "test_icd.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.IcdTests.setUpClass()
        cls.addClassCleanup(module.IcdTests.doClassCleanups)
        cls.checkpoint2 = module.IcdTests("test_normal_loader")

    def run_interop(self, path, stage):
        env = dict(os.environ, VK_DRIVER_FILES=str(self.checkpoint2.manifest),
                   VK_ICD_FILENAMES=str(self.checkpoint2.manifest), MALI_VULKAN_BROKER_SOCKET=path,
                   MALI_VULKAN_INTEROP_TEST="1", MALI_VULKAN_AHB_TEST="1" if stage >= 3 else "0",
                   VK_LOADER_LAYERS_DISABLE="*", DISPLAY="invalid-for-interop", WAYLAND_DISPLAY="invalid-for-interop")
        return subprocess.run([str(self.interop_normal), str(stage)], env=env, capture_output=True, text=True, timeout=20)

    def test_all_four_independent_memory_interop_paths(self):
        for stage in range(1, 5):
            with self.subTest(stage=stage), self.checkpoint2.broker(42) as path:
                test = self.run_interop(path, stage)
                self.assertEqual(test.returncode, 0, test.stdout + test.stderr)
                self.assertIn(f"4D{stage} PASS", test.stdout)
                self.assertIn("full device/instance cleanup", test.stdout)
                if stage >= 3: self.assertIn("actual AHB-backed image GPU readback: mismatched pixels=0", test.stdout)
                if stage == 4: self.assertIn("Android acquired/presented/released exact Vulkan AHB", test.stdout)

    def test_memory_noncoherent_and_channel_corruption(self):
        with self.checkpoint2.broker(29) as path:
            for stage in (1, 2):
                test = self.run_interop(path, stage)
                self.assertEqual(test.returncode, 0, test.stdout + test.stderr)
                self.assertIn("vkInvalidateMappedMemoryRanges", test.stdout)
                if stage == 1: self.assertIn("vkFlushMappedMemoryRanges", test.stdout)
        with self.checkpoint2.broker(36) as path:
            test = self.run_interop(path, 2)
            self.assertEqual(test.returncode, 1, test.stdout + test.stderr)
            self.assertIn("mismatched pixels=1", test.stdout)
            self.assertNotIn("4D2 PASS", test.stdout)

    def test_ahb_unsupported_configuration_and_consumer_failure_are_not_pass(self):
        for mode, stage, code in ((30, 1, -2), (31, 2, -11), (32, 1, -2), (33, 3, -7), (34, 3, -11), (37, 3, -1000072003), (38, 3, -10), (41, 4, -3)):
            with self.subTest(mode=mode), self.checkpoint2.broker(mode) as path:
                test = self.run_interop(path, stage)
                self.assertEqual(test.returncode, 1, test.stdout + test.stderr)
                self.assertIn(f"VkResult={code}", test.stdout)
                self.assertNotIn(f"4D{stage} PASS", test.stdout)
        with self.checkpoint2.broker(35) as path:
            test = self.run_interop(path, 3)
            self.assertEqual(test.returncode, 0, test.stdout + test.stderr)
            self.assertIn("AHB CPU lock unavailable", test.stdout)

    def test_interop_options_exit_before_startup_and_keep_renderer_gate(self):
        main = (self.source / "src/main.cpp").read_text()
        entry = main.index("int main(")
        for option, function in (("vk-buffer-memory-test", "vulkan_buffer_memory_test"),
                                 ("vk-image-memory-test", "vulkan_image_memory_test"),
                                 ("vk-ahb-test", "vulkan_ahb_test"), ("vk-ahb-present-test", "vulkan_ahb_present_test")):
            self.assertIn('{ "' + option + '", no_argument, nullptr, 0 }', main)
            self.assertIn("return " + function + "();", main[entry:entry + 2300])
        self.assertNotIn("src/rendervulkan.cpp", (ROOT / "patches/0117-vulkan-memory-ahb-tests.patch").read_text())
        self.assertEqual((self.source / "src/interop_test_api.h").read_bytes(), (MALI / "interop_test_api.h").read_bytes())

    def run_submit(self, path):
        env = dict(os.environ, VK_DRIVER_FILES=str(self.checkpoint2.manifest),
                   VK_ICD_FILENAMES=str(self.checkpoint2.manifest), MALI_VULKAN_BROKER_SOCKET=path,
                   MALI_VULKAN_SUBMIT_TEST="1", VK_LOADER_LAYERS_DISABLE="*",
                   DISPLAY="invalid-for-submit-test", WAYLAND_DISPLAY="invalid-for-submit-test")
        return subprocess.run([str(self.submit_normal)], env=env, capture_output=True, text=True, timeout=20)

    def test_submit_success_through_normal_loader(self):
        for repeat in range(2):
            with self.checkpoint2.broker() as path:
                result = self.run_submit(path)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                values = ("gamescope: Vulkan queue-submit test", "physical API: 1.1.131", "queue family: 0",
                          "queue obtained: yes", "command pool created: yes", "primary command buffer allocated: yes",
                          "event initial status: VK_EVENT_RESET", "vkBeginCommandBuffer: VK_SUCCESS",
                          "vkCmdSetEvent recorded", "vkEndCommandBuffer: VK_SUCCESS", "fence created: unsignaled",
                          "vkQueueSubmit: VK_SUCCESS", "vkWaitForFences: VK_SUCCESS", "vkGetFenceStatus: VK_SUCCESS",
                          "event final status: VK_EVENT_SET", "observable device command executed: yes",
                          "fence destroyed", "event destroyed", "command buffer freed", "command pool destroyed",
                          "logical device destroyed", "instance destroyed", "exit 0")
                last = -1
                for value in values:
                    found = result.stdout.index(value)
                    self.assertGreater(found, last); last = found
            self.assertIn("pools=1/1 commands=1/1 events=1/1 fences=1/1 submits=1 executions=1 idle=0",
                          self.checkpoint2.cleanup_output)

    def test_submit_failure_at_each_stage_cleanup(self):
        for mode in (9, 10, 11, 12, 13, 14, *range(15, 29)):
            with self.subTest(mode=mode), self.checkpoint2.broker(mode) as path:
                result = self.run_submit(path)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertNotIn("observable device command executed: yes", result.stdout)
                self.assertNotIn("exit 0", result.stdout)
                self.assertIn("instance destroyed", result.stdout)
                if mode in (22, 27):
                    self.assertIn("vkWaitForFences: VK_TIMEOUT (2)", result.stdout)
                    self.assertIn("delegating safe child cleanup", result.stdout)
                    self.assertNotIn("vkGetFenceStatus:", result.stdout)
                if mode in (23, 24, 26):
                    self.assertIn("VK_ERROR_DEVICE_LOST (-4)", result.stdout)
                if mode == 25:
                    self.assertIn("event final status: VK_EVENT_RESET", result.stdout)
            # broker fixture asserts balanced native resources on every failure

    def test_submit_dispatch_and_no_renderer_dependencies(self):
        main = (self.source / "src/main.cpp").read_text()
        body = main[main.index("int main(int argc, char **argv)"):]
        early = body.index("return vulkan_submit_test();")
        parser = body.index("return vulkan_submit_test();", early + 1)
        self.assertLess(early, body.index("XInitThreads()"))
        self.assertLess(parser, body.index("auto_select_backend()"))
        self.assertIn('{ "vk-submit-test", no_argument, nullptr, 0 }', main)
        symbols = subprocess.check_output(["nm", "-u", str(self.submit_object)], text=True)
        required = {"vkCreateInstance", "vkDestroyInstance", "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceProperties",
                    "vkGetPhysicalDeviceFeatures", "vkEnumerateDeviceExtensionProperties", "vkGetPhysicalDeviceQueueFamilyProperties",
                    "vkCreateDevice", "vkDestroyDevice", "vkGetDeviceQueue"}
        required |= {"vk" + line[len("MB_SUBMIT_ENTRY("):-1] for line in (MALI / "submit_entries.def").read_text().splitlines()
                     if line.startswith("MB_SUBMIT_ENTRY(")}
        self.assertEqual({line.split()[-1] for line in symbols.splitlines() if line.split()[-1].startswith("vk")}, required)
        patch = (ROOT / "patches/0116-vulkan-submit-test.patch").read_text()
        self.assertNotIn("+++ b/src/rendervulkan", patch)
        self.assertNotIn("+++ b/src/Backends", patch)

    def test_submit_missing_icd(self):
        missing = str(self.directory / "absent-icd.json")
        result = subprocess.run([str(self.submit_normal)], env=dict(os.environ, VK_DRIVER_FILES=missing,
                                VK_ICD_FILENAMES=missing, VK_LOADER_LAYERS_DISABLE="*"), capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1)
        self.assertIn("gamescope: submit test vkCreateInstance failed:", result.stderr)
        self.assertNotIn("physical device:", result.stdout)

    def test_device_path_exits_before_normal_startup(self):
        main = (self.source / "src/main.cpp").read_text()
        body = main[main.index("int main(int argc, char **argv)"):]
        early = body.index("return vulkan_create_device_test();")
        self.assertLess(early, body.index("g_argc = argc;"))
        parser = body.index("return vulkan_create_device_test();", early + 1)
        for initialization in ("HasCapSysNice()", "RaiseFdLimit()", "gpuvis_trace_init()", "RunDefaultScripts()",
                               "XInitThreads()", "auto_select_backend()", "vulkan_init_formats()", "vulkan_make_output()"):
            self.assertLess(parser, body.index(initialization))
        self.assertIn('{ "vk-create-device-test", no_argument, nullptr, 0 }', main)
        symbols = subprocess.check_output(["nm", "-u", str(self.device_object)], text=True)
        actual = {line.split()[-1] for line in symbols.splitlines() if line.split()[-1].startswith("vk")}
        self.assertEqual(actual, {"vkCreateInstance", "vkDestroyInstance", "vkEnumeratePhysicalDevices",
            "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceFeatures", "vkEnumerateDeviceExtensionProperties",
            "vkGetPhysicalDeviceQueueFamilyProperties", "vkCreateDevice", "vkGetDeviceQueue", "vkDestroyDevice"})
        # This patch never touches the normal renderer or shaders.
        patch = (ROOT / "patches/0115-vulkan-create-device-test.patch").read_text()
        self.assertNotIn("b/src/rendervulkan", patch)
        self.assertNotIn("b/src/shaders", patch)

    def run_device(self, path):
        fixture = self.checkpoint2
        env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(fixture.manifest),
                   VK_ICD_FILENAMES=str(fixture.manifest), VK_LOADER_LAYERS_DISABLE="*", MALI_VULKAN_DEVICE_TEST="1",
                   DISPLAY="invalid-for-device-test", WAYLAND_DISPLAY="invalid-for-device-test")
        return subprocess.run([str(self.device_normal)], env=env, capture_output=True, text=True, timeout=20)

    def test_device_normal_loader_and_mock_android(self):
        fixture = self.checkpoint2
        with fixture.broker() as path:
            for _ in range(2):
                result = self.run_device(path)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for value in ("gamescope: Vulkan logical-device test", "physical device: HOST TEST ONLY",
                              "physical API: 1.1.131", "queue family: 0 (verified graphics+compute)",
                              "requested extensions: none", "requested features: none", "queue obtained = yes",
                              "logical device destroyed", "instance destroyed", "normal Gamescope rendering remains disabled", "exit 0"):
                    self.assertIn(value, result.stdout)
                self.assertIn("broker vkCreateDevice = VK_SUCCESS", result.stderr)
        self.assertIn("DEVICES creates=2 destroys=2 queues=2", fixture.cleanup_output)

    def test_device_failures_cleanup_without_success(self):
        fixture = self.checkpoint2
        for mode in (3, 4, 9, 10, 11, 12, 13):
            with self.subTest(mode=mode), fixture.broker(mode) as path:
                result = self.run_device(path)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertNotIn("exit 0", result.stdout)
                self.assertIn("instance destroyed", result.stdout)
            self.assertIn("creates=1 destroys=1", fixture.cleanup_output)
            self.assertIn("DEVICES creates=1 destroys=1" if mode in (10, 11) else "DEVICES creates=0 destroys=0", fixture.cleanup_output)

    def test_device_queue_family_zero_must_have_required_flags(self):
        fixture = self.checkpoint2
        with fixture.broker(14) as path:
            result = self.run_device(path)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("queue family: 1 (verified graphics+compute)", result.stdout)
        self.assertIn("DEVICES creates=1 destroys=1 queues=1", fixture.cleanup_output)

    def test_device_missing_icd(self):
        missing = str(self.directory / "missing.json")
        result = subprocess.run([str(self.device_normal)], env=dict(os.environ, VK_DRIVER_FILES=missing,
            VK_ICD_FILENAMES=missing, VK_LOADER_LAYERS_DISABLE="*"), capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("gamescope: device test vkCreateInstance failed:", result.stderr)
        self.assertNotIn("physical device:", result.stdout)
        self.assertNotIn("exit 0", result.stdout)

    def test_capability_path_has_no_device_or_backend_calls(self):
        main = (self.source / "src/main.cpp").read_text()
        body = main[main.index("int main(int argc, char **argv)"):]
        early = body.index("return vulkan_capabilities();")
        self.assertLess(early, body.index("g_argc = argc;"))
        parser = body.index("return vulkan_capabilities();", early + 1)
        self.assertLess(parser, body.index("HasCapSysNice()"))
        self.assertIn('{ "vk-capabilities", no_argument, nullptr, 0 }', main)
        symbols = subprocess.check_output(["nm", "-u", str(self.cap_object)], text=True)
        for forbidden in ("vkCreateDevice", "vkAllocateMemory", "vkCreateImage", "vkCreateBuffer", "wl_display", "vkQueueSubmit", "vkCreateWaylandSurface"):
            self.assertNotIn(forbidden, symbols)

    def test_capabilities_real_loader_and_mock_android(self):
        fixture = self.checkpoint2
        with fixture.broker() as path:
            env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(fixture.manifest),
                       VK_ICD_FILENAMES=str(fixture.manifest), VK_LOADER_LAYERS_DISABLE="*", MALI_VULKAN_QUERY_CAPABILITIES="1")
            result = subprocess.run([str(self.cap_normal)], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for value in ("HOST TEST ONLY", "physical-device API: 1.1.131", "textureCompressionBC=0", "samplerAnisotropy=1",
                          "features2 scalarBlockLayout=0", "features2 timelineSemaphore=1", "features2 dynamicRendering: NOT QUERYABLE",
                          "queue families: 2", "memory heaps: 1 types: 2", "modifier=0x123456789abcdef0",
                          "modifier DMA_BUF image result=0", "D: device below version floor", "no device/backend initialized"):
                self.assertIn(value, result.stdout)
        self.assertIn("creates=1 destroys=1", fixture.cleanup_output)

    def test_capabilities_missing_icd_smoke(self):
        missing = str(self.directory / "missing.json")
        result = subprocess.run([str(self.cap_normal)], env=dict(os.environ, VK_DRIVER_FILES=missing,
            VK_ICD_FILENAMES=missing, VK_LOADER_LAYERS_DISABLE="*"), capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("gamescope: capabilities vkCreateInstance failed:", result.stderr)
        self.assertNotIn("deviceName:", result.stdout)
        smoke = (ROOT / "build-in-arch.sh").read_text()
        self.assertIn("grep -F 'gamescope: capabilities vkCreateInstance failed:'", smoke)

    def test_capabilities_one_zero_fallback(self):
        fixture = self.checkpoint2
        with fixture.broker(6) as path:
            env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=path, VK_DRIVER_FILES=str(fixture.manifest),
                       VK_ICD_FILENAMES=str(fixture.manifest), VK_LOADER_LAYERS_DISABLE="*", MALI_VULKAN_QUERY_CAPABILITIES="1")
            result = subprocess.run([str(self.cap_normal)], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("device extensions (real Android inventory; device creation remains unsupported): 0", result.stdout)
            self.assertIn("external semaphore FD: NOT QUERIED", result.stdout)
            self.assertIn("features2: NOT QUERYABLE", result.stdout)

    def run_mock(self, mode):
        return subprocess.run([str(self.mock)], env=dict(os.environ, ENUM_TEST_MODE=mode),
                              capture_output=True, text=True, timeout=10)

    def test_patch_stack_and_early_entry(self):
        self.assertNotIn("fuzz", self.patch_output)
        main = (self.source / "src/main.cpp").read_text()
        body = main[main.index("int main(int argc, char **argv)"):]
        first_return = body.index("return vulkan_enumerate_only();")
        self.assertLess(first_return, body.index("g_argc = argc;"))
        parser_return = body.index("return vulkan_enumerate_only();", first_return + 1)
        for initialization in ("HasCapSysNice()", "RaiseFdLimit()", "gpuvis_trace_init()", "RunDefaultScripts()",
                               "XInitThreads()", "auto_select_backend()", "CheckWaylandPresentationTime()",
                               "gamescope::IBackend::Set<", "vulkan_init_formats()", "vulkan_make_output()"):
            self.assertLess(parser_return, body.index(initialization))
        self.assertIn('{ "vk-enumerate-only", no_argument, nullptr, 0 }', main)
        self.assertIn("'vulkan_enumerate_only.cpp'", (self.source / "src/meson.build").read_text())
        reverse = self.directory / "reverse-check"
        shutil.copytree(self.source, reverse)
        for name in ("0119-vulkan-blit-input-proof.patch", "0118-vulkan-gamescope-renderer-tests.patch", "0117-vulkan-memory-ahb-tests.patch", "0116-vulkan-submit-test.patch", "0115-vulkan-create-device-test.patch", "0114-vulkan-capabilities.patch", "0113-vulkan-enumerate-only.patch"):
            subprocess.run(["patch", "-p1", "--batch", "--fuzz=0", "--reverse", "-i", str(ROOT / "patches" / name)],
                           cwd=reverse, check=True, capture_output=True)

    def test_only_four_instance_query_calls(self):
        symbols = subprocess.check_output(["nm", "-u", str(self.object)], text=True)
        self.assertEqual({line.split()[-1] for line in symbols.splitlines() if line.split()[-1].startswith("vk")},
                         {"vkCreateInstance", "vkDestroyInstance", "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceProperties"})
        for forbidden in ("wl_display", "wlserver", "vulkan_init", "GetBackend", "CreateDevice", "QueueSubmit"):
            self.assertNotIn(forbidden, symbols)

    def test_multiple_devices_and_honest_version(self):
        result = self.run_mock("success")
        self.assertEqual(result.returncode, 0, result.stderr)
        for value in ("instance API 1.0, no extensions", "physical-device count = 2", "HOST MOCK GPU 0",
                      "HOST MOCK GPU 1", "vendorID: 0x000013b5", "deviceID: 0x74021000", "apiVersion: 1.1.131",
                      "driverVersion: 109051904", "deviceType: 1", "destroys=1 lists=1 properties=2"):
            self.assertIn(value, result.stdout)

    def test_create_error_has_no_destroy_or_device_output(self):
        result = self.run_mock("create-error")
        self.assertEqual(result.returncode, 1)
        self.assertIn("vkCreateInstance failed: -9", result.stderr)
        self.assertIn("destroys=0 lists=0 properties=0", result.stdout)
        self.assertNotIn("deviceName:", result.stdout)

    def test_enumeration_errors_destroy_instance(self):
        for mode in ("count-error", "list-error", "always-incomplete"):
            with self.subTest(mode=mode):
                result = self.run_mock(mode)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("physical-device query failed:", result.stderr)
                self.assertIn("destroys=1", result.stdout)
                self.assertNotIn("deviceName:", result.stdout)
                if mode == "always-incomplete":
                    self.assertIn("lists=4", result.stdout)

    def test_incomplete_list_retries_without_partial_output(self):
        result = self.run_mock("retry")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("destroys=1 lists=2 properties=2", result.stdout)
        self.assertEqual(result.stdout.count("deviceName:"), 2)

    def test_zero_devices_is_an_honest_successful_diagnostic(self):
        result = self.run_mock("zero")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("physical-device count = 0", result.stdout)
        self.assertIn("destroys=1 lists=0 properties=0", result.stdout)

    def test_normal_loader_proxy_broker_path(self):
        fixture = self.checkpoint2
        with fixture.broker() as socket:
            manifest = str(fixture.manifest)
            env = dict(os.environ, MALI_VULKAN_BROKER_SOCKET=socket, VK_DRIVER_FILES=manifest,
                       VK_ICD_FILENAMES=manifest, VK_LOADER_LAYERS_DISABLE="*",
                       DISPLAY="invalid-for-enumeration", WAYLAND_DISPLAY="invalid-for-enumeration")
            result = subprocess.run([str(self.normal)], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for value in ("vkCreateInstance = VK_SUCCESS", "physical-device count = 1", "HOST TEST ONLY",
                          "vendorID: 0x000013b5", "deviceID: 0x74021000", "apiVersion: 1.1.131",
                          "destroyed enumeration-only Vulkan instance"):
                self.assertIn(value, result.stdout)
        self.assertIn("creates=1 destroys=1 closes=1", fixture.cleanup_output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
