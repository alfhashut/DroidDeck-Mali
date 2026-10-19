"""Production session script and normal v7 transport boundaries (host models)."""
from pathlib import Path
import json
import os
import socket
import struct
import subprocess
import tempfile
import unittest
import test_icd
import test_renderer

ROOT = Path(__file__).resolve().parents[2]


class NormalBrokerTests(test_renderer.RendererFixture):
    def open_normal(self, path):
        connection = self.connect(path)
        self.fields(connection, 2, 1 << 22)
        self.call(connection, 4, expected=(0, 0, 1))
        request = self.request()[:-16] + struct.pack("<4I", 1, 1, 1, 0)
        device = struct.unpack("<I", self.call(connection, 14, request, (0, 0, 1)))[0]
        return connection, device

    def test_normal_begin_stats_end_and_separate_diagnostic(self):
        with self.broker(43) as path:
            connection, device = self.open_normal(path)
            self.fields(connection, 87, device, 0)
            counts = struct.unpack("<26I", self.fields(connection, 91, device, expected=(0, 0, 1)))
            self.assertEqual(counts, (1,) + (0,) * 25)
            self.fields(connection, 90, device)
            self.fields(connection, 16, device)
        self.assertIn("release timeouts=0", self.broker_errors)
        self.assertIn("baseline: devices=0 queues=0", self.broker_errors)

    def test_normal_wrong_parent_stale_key_and_explicit_begin(self):
        for case in ("not begun", "wrong parent", "stale token", "double begin", "diagnostic"):
            with self.subTest(case=case), self.broker(43) as path:
                connection, device = self.open_normal(path)
                if case == "not begun":
                    self.fields(connection, 91, device, expected=(3, 0, 0))
                elif case == "diagnostic":
                    self.fields(connection, 83, device)
                    self.fields(connection, 87, device, 0, expected=(3, 0, 0))
                else:
                    self.fields(connection, 87, device, 0)
                    if case == "wrong parent":
                        self.fields(connection, 91, device + 10000, expected=(3, 0, 0))
                    elif case == "stale token":
                        self.fields(connection, 88, device, 99999, expected=(3, 0, 0))
                    else:
                        self.fields(connection, 87, device, 0, expected=(3, 0, 0))
                self.assert_disconnected(connection)

    def test_normal_rejects_legacy_wire_and_invalid_payload(self):
        for version, payload in ((6, struct.pack("<II", 1, 0)), (7, bytes(4))):
            with self.subTest(version=version), self.broker(43) as path:
                connection, device = self.open_normal(path)
                self.assertEqual(test_icd.rpc(connection, 87, payload, version=version)[0], (3, 0, 0))
                self.assert_disconnected(connection)


class NormalLaunchTests(unittest.TestCase):
    def run_script(self, change=None):
        with tempfile.TemporaryDirectory(prefix="normal-launch-") as directory:
            root = Path(directory)
            executable = root / "gamescope"
            executable.write_text("#!/usr/bin/env python3\nimport json,os,sys\nprint('ARGV='+json.dumps(sys.argv[1:]))\nprint('ICD='+os.environ['VK_ICD_FILENAMES'])\n")
            executable.chmod(0o755)
            manifest = root / "mali.json"
            manifest.write_text('{"ICD":{"library_path":"proxy.so","api_version":"1.0.0"}}')
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                       BL_LOG="", BL_INSIDE="", BL_WIDTH="1280", BL_HEIGHT="720",
                       XDG_RUNTIME_DIR=directory, WAYLAND_DISPLAY="wayland-0",
                       MALI_VULKAN_NORMAL_SESSION="1", MALI_VULKAN_BROKER_SOCKET=str(root / "broker"),
                       VK_ICD_FILENAMES=str(manifest), VK_DRIVER_FILES=str(manifest))
            with socket.socket(socket.AF_UNIX) as display, socket.socket(socket.AF_UNIX) as broker:
                display.bind(str(root / "wayland-0")); broker.bind(str(root / "broker"))
                if change:
                    env.update(change)
                return subprocess.run(["bash", str(ROOT / "tools/linuxfs/overlay/usr/local/bin/droiddeck-session"), "mali-wayland"],
                                      env=env, capture_output=True, text=True, timeout=3)

    def test_normal_script_selects_actual_wayland_backend_and_ordinary_child(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        args = json.loads(next(line[5:] for line in result.stdout.splitlines() if line.startswith("ARGV=")))
        self.assertEqual(args, ["--backend", "wayland", "--mali-wayland-session", "--expose-wayland", "-f",
                                "-W", "1280", "-H", "720", "--", "/usr/local/bin/gamescope", "--mali-interactive-client"])

    def test_normal_script_missing_broker_fails_without_starting_gamescope(self):
        result = self.run_script({"MALI_VULKAN_BROKER_SOCKET": "/nonexistent/droiddeck-broker"})
        self.assertEqual(result.returncode, 65)
        self.assertIn("broker socket disappeared", result.stdout)
        self.assertNotIn("ARGV=", result.stdout)

    def test_normal_script_missing_icd_fails_without_fallback(self):
        result = self.run_script({"VK_ICD_FILENAMES": "/nonexistent/mali.json"})
        self.assertEqual(result.returncode, 65)
        self.assertIn("proxy manifest missing", result.stdout)
        self.assertNotIn("ARGV=", result.stdout)

    def test_normal_script_requires_opt_in_and_outer_wayland_0(self):
        for change in ({"MALI_VULKAN_NORMAL_SESSION": "0"}, {"WAYLAND_DISPLAY": "wayland-1"}):
            with self.subTest(change=change):
                result = self.run_script(change)
                self.assertEqual(result.returncode, 65)
                self.assertNotIn("ARGV=", result.stdout)

    def test_broker_connects_before_timeout_and_inventory_handshake(self):
        # Scope to the broker connection: the earlier Wayland socket probe
        # must not accidentally satisfy this regression check.
        broker = (ROOT / "app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt").read_text()
        handshake = broker.split("val socket = SystemVulkanBroker.startNormal(context)", 1)[1].split("val directory = File", 1)[0]
        created = handshake.index("LocalSocket().use { connection ->")
        connected = handshake.index("connection.connect(LocalSocketAddress(socket, LocalSocketAddress.Namespace.FILESYSTEM))")
        timeout = handshake.index("connection.soTimeout = 3000")
        inventory = handshake.index("connection.outputStream.write(request)")
        self.assertLess(created, connected)
        self.assertLess(connected, timeout, "LocalSocket must connect/create its FD before SO_TIMEOUT")
        self.assertLess(timeout, inventory)

    def test_normal_manifest_uses_diagnostic_asset_and_resolved_library_validation(self):
        # Parse the actual build input, without compiling Android/Gamescope fixtures.
        manifest = json.loads((ROOT / "tools/mali-vulkan/mali_proxy_icd.json").read_text())
        self.assertEqual(manifest["file_format_version"], "1.0.0")
        self.assertEqual(manifest["ICD"]["api_version"], "1.0.0")
        self.assertEqual(manifest["ICD"]["library_path"], "./libdroiddeck_mali_proxy.so")
        with tempfile.TemporaryDirectory(prefix="mali-manifest-") as directory:
            library = Path(directory) / "libdroiddeck_mali_proxy.so"
            self.assertEqual((Path(directory) / manifest["ICD"]["library_path"]).resolve(), library.resolve())
        build = (ROOT / "tools/mali-vulkan/build-icd.sh").read_text()
        self.assertIn('cp -- "$repo_root/tools/mali-vulkan/mali_proxy_icd.json" "$output/mali_proxy_icd.json"', build)
        broker = (ROOT / "app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt").read_text()
        self.assertIn("val library = validateProxyManifest(manifest)", broker)
        self.assertIn("resolved == library.canonicalFile", broker)
        self.assertIn("resolved.parentFile == directory", broker)
        self.assertNotIn('icd.getString("library_path") == "libdroiddeck_mali_proxy.so"', broker)

    def test_service_readiness_order_and_stop_start_race_guards(self):
        # Source contracts supplement the executable native/shell tests. They
        # do not claim to execute an Android Activity/Service lifecycle on host.
        service = (ROOT / "app/src/main/java/com/droiddeck/launcher/session/SessionService.kt").read_text()
        normal = service.split("private fun runMaliSession", 1)[1].split("private fun stopMaliGuest", 1)[0]
        self.assertLess(normal.index("MaliNormalBroker.start"), normal.index("HostProcess.start"))
        self.assertIn("synchronized(maliStartupLock)", normal)
        self.assertIn("synchronized(stopLock)", normal)
        self.assertGreaterEqual(normal.count("gen != sessionGen || !SessionState.running"), 3)
        self.assertIn('SessionEvents.fail("MALI_SESSION_START"', normal)
        broker = (ROOT / "app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt").read_text()
        self.assertLess(broker.index("connection.outputStream.write"), broker.index("context.assets.open"))
        self.assertLess(broker.index("context.assets.open"), broker.index("return MaliSessionSelection.environment"))
        self.assertLess(broker.index("OsConstants.S_ISSOCK"), broker.index("SystemVulkanBroker.startNormal"))
        self.assertIn("SystemClock.elapsedRealtime() < deadline", broker)
        self.assertIn('check(found)', broker)

    def test_duplicate_launch_stopping_guard_and_adreno_isolation(self):
        activity = (ROOT / "app/src/main/java/com/droiddeck/launcher/MainActivity.kt").read_text()
        service = (ROOT / "app/src/main/java/com/droiddeck/launcher/session/SessionService.kt").read_text()
        self.assertIn("selectingSessionGpu", activity)
        self.assertIn("SessionPhase.STOPPING", activity)
        self.assertIn("SessionPhase.STOPPING", service)
        self.assertIn("MaliSessionSelection.supported(gpu)", activity)
        self.assertIn("SessionState.phase !in setOf(SessionPhase.IDLE, SessionPhase.FAILED)", activity)
        self.assertIn("runMaliSession(gen); return", service)

    def test_reattached_activity_uses_live_mode_and_broker_lease_survives_stop_error(self):
        activity = (ROOT / "app/src/main/java/com/droiddeck/launcher/SessionActivity.kt").read_text()
        framegen = activity.split("private fun applyFrameGen()", 1)[1].split("val hz = refreshHz()", 1)[0]
        self.assertIn("loadingMali()", framegen)
        selection = activity.split("private fun loadingMali()", 1)[1].split("private fun pausedTitle", 1)[0]
        self.assertIn("SessionState.running", selection)
        self.assertIn("SessionState.maliBackend", selection)
        self.assertIn("nativeSetFrameGenArmed(false, 0, 0)", framegen)
        self.assertIn("MaliSessionSelection.MODE -> getString(R.string.session_starting_mali)", activity)
        broker = (ROOT / "app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanBroker.kt").read_text()
        stop = broker.split("fun stopNormal()", 1)[1].split("/**", 1)[0]
        self.assertLess(stop.index("nativeStop()"), stop.index("normalOwned = false"))
