# DroidDeck-Mali 0.5 port: Phase 2 diagnostics

Base: committed Phase 1 (`794e1b1b`) on `mali-0.5-port`.
Reference implementation: `mali-gpu-experiment` (`3e1d9255`).

This phase adds explicit shell diagnostics. Ordinary launch routing, GPU detection,
Android sessions, Steam, the Wayland compositor and PanVK integration are unchanged.
No normal Mali session entry point or shared-staging activation is added.

**Android and 0.5 API adaptations**

`SystemVulkanBroker.kt` and `SystemVulkanBrokerActivity.kt` are adapted from the
reference. Normal broker lease methods and persistent-session diagnostic controls
are omitted. The activity has no intent filter and requires `android.permission.DUMP`.
Its worker serializes tests and teardown; Surface callbacks retain their separate
native entry point without acquiring the wrapper monitor.

The wrapper uses current `GuestCommand.run(..., logName = ...)`, including current
0.5 runtime setup, clean child environments, persistent proot temporary storage and
`LinuxRuntime.logDir(context)` logging through GuestCommand. No obsolete
`debugLogDir()` API is restored, and runtime APIs are not changed.

Diagnostic probes, proxy ICD and manifest are staged from the existing five Phase 1
assets under app-private `files/mali-vulkan`. Executables receive mode 0700, data and
library files 0600, and the broker directory 0700. The relative manifest is validated
against the staged proxy library using the reference validator, now scoped to the
diagnostic wrapper. Proxy Vulkan environment variables apply only to each GuestCommand
child. The Android native broker still loads `/system/lib64/libvulkan.so`.

All Phase 1 native implementation files, proxy source and protocol/schema headers
remain byte-identical. Existing JNI symbols and CMake targets already match the
new wrapper, so no native build or compositor CMake changes are needed. Native
same-UID socket checks, ownership, disconnect cleanup, timeout retention and quarantine
are preserved; the wrapper refuses restart while diagnostic allocations remain
quarantined.

**Gamescope patch reconciliation**

All 11 existing 0.5 patches retain their exact bytes. The seven Mali patches are
renamed to 0200-0206 to avoid the 0114/0115 collisions. Applied after 0.5's patches,
all seven diagnostic patches apply at their expected lines with zero fuzz; no
conflicting hunks require changes. The complete stack passes on pristine Gamescope
3.16.29. Reversing only 0200-0206 reproduces the complete 0.5-patched source tree.

The sole patch-content adaptation is `0204`'s copied `interop_test_api.h`: it matches
the Phase 1 canonical header exactly, including comments and unused later ABI
declarations. Wire IDs, layouts and versions are unchanged. No later session
implementation is imported.

Final application order:

```text
0002-steamcompmgr-fallback-appid-focus.patch
0009-fix-arm64-steam-night-mode.patch
0019-steamcompmgr-arm64-virtual-white.patch
0020-color-p3-red-is-wide-gamut.patch
0100-realtime-queue-and-gamepad-cursor.patch
0101-nested-wayland-cursor-routing.patch
0110-wayland-backend-touch.patch
0111-wayland-pointer-warps-in-passthrough.patch
0112-restore-iconified-game-on-resume.patch
0114-take-override-redirect-from-mapnotify.patch
0115-limiter-paces-per-window.patch
0200-vulkan-enumerate-only.patch
0201-vulkan-capabilities.patch
0202-vulkan-create-device-test.patch
0203-vulkan-submit-test.patch
0204-vulkan-memory-ahb-tests.patch
0205-vulkan-gamescope-renderer-tests.patch
0206-vulkan-blit-input-proof.patch
```

**Build and CI**

- `build.yml` adds an ARM64 Gamescope producer for the port branch and consumes its
  same-run artifact. Other branches retain the pinned component path.
- The same-run archive checksum and source/build-input fingerprint are checked
  before overlaying the binary after cache restoration. Its diagnostic markers
  are then checked. Existing 0.5 runtimeAssets preparation, input attestation,
  session-asset checks and APK payload checks run afterward.
- The current Gamescope build script is extended, not restored from the fork.
  It retains the cursor-routing test and SONAME audit and adds independent
  missing-ICD diagnostic smoke checks. Both component workflows upload the
  fingerprint alongside the archive and checksum.
- `build-gamescope.yml` retains its existing triggers and manual release behavior.
- `.gitattributes` handles unified-patch context whitespace. Added diagnostic source
  is separately checked with `git apply --whitespace=error` in the host patch test.

**Lightweight validation**

The Phase 2 runner passed 63 broker/proxy/security/diagnostic-boundary contracts and
25 Gamescope diagnostic tests. It compiled only small host mocks and diagnostic
translation units in temporary directories, with public Vulkan headers. The system
host Vulkan loader loaded the actual proxy; Android Vulkan operations were mocked.
These results are not real Mali GPU evidence.

Tests include native socket permissions/ownership, rejected foreign or unavailable
peer credentials before Vulkan initialization, version and malformed-message
rejection, capability queries, device lifecycle, submit/fence errors, disconnect
cleanup, memory/AHB diagnostics, missing-ICD failures, early exits before normal
startup, and full preservation of 0.5's patch stack.

Runtime-input tests: 14 passed. Existing session-asset tests: 4 passed. Workflow
YAML/embedded shell, Python and shell syntax checks pass. All-file `git diff --check`
passes without staging changes in the real Git index.

Example local host-only test invocation, using separately downloaded public headers
and pristine source (no build products are placed in assets):

```sh
python3 -B tools/mali-vulkan/run_phase2_tests.py \
  --gamescope-source /tmp/droiddeck-mali-phase2-source/gamescope-3.16.29 \
  --vulkan-headers /tmp/droiddeck-mali-phase2-headers/Vulkan-Headers-1.3.296/include
```

**Still requires GitHub Actions and a device**

GitHub Actions must build/link the full ARM64 Gamescope renderer and Android APK,
run Android/Robolectric tests including `MaliProxyManifestTest`, and verify packaged
native libraries and the same-run component overlay. Those workflows were edited
and inspected; they were not triggered here. No APK or heavy project build was run
locally. The local/pinned Gamescope release was not changed.

On a phone, install the CI APK and explicitly open the shell-protected activity:

```sh
adb shell am start -n com.droiddeck.launcher/com.droiddeck.launcher.gpu.SystemVulkanBrokerActivity
adb logcat -s MaliVulkanBroker:I '*:S'
```

Run probe, normal-loader ICD test, Gamescope enumeration, capabilities, device
creation and submit/fence checks in order. Confirm the actual Android device
identity and real VkResults, then stop/reopen and verify cleanup. Memory/AHB/renderer
buttons are separate diagnostic checkpoints using the debug Surface; real GPU
readback, SYNC_FD and Android release behavior still require device evidence.

Normal Mali launch, persistent sessions, compositor AHB handoff, automatic routing,
shared staging and PanVK client integration remain deferred. No proprietary Mali
binary was added. No commit was created.

**Every added or modified file**

```text
M	.gitattributes
M	.github/workflows/build-gamescope.yml
M	.github/workflows/build.yml
M	app/src/main/AndroidManifest.xml
A	app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanBroker.kt
A	app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanBrokerActivity.kt
A	app/src/test/java/com/droiddeck/launcher/gpu/MaliProxyManifestTest.kt
A	docs/development/mali-phase2.md
M	tools/gamescope/PATCHES.md
M	tools/gamescope/build-in-arch.sh
A	tools/gamescope/check-diagnostics.sh
A	tools/gamescope/patches/0200-vulkan-enumerate-only.patch
A	tools/gamescope/patches/0201-vulkan-capabilities.patch
A	tools/gamescope/patches/0202-vulkan-create-device-test.patch
A	tools/gamescope/patches/0203-vulkan-submit-test.patch
A	tools/gamescope/patches/0204-vulkan-memory-ahb-tests.patch
A	tools/gamescope/patches/0205-vulkan-gamescope-renderer-tests.patch
A	tools/gamescope/patches/0206-vulkan-blit-input-proof.patch
A	tools/gamescope/test_vk_enumerate_only.py
A	tools/gamescope/tests/mock_enumeration.cpp
A	tools/mali-vulkan/run_phase2_tests.py
A	tools/mali-vulkan/test_diagnostics.py
M	tools/mali-vulkan/test_icd.py
M	tools/mali-vulkan/tests/broker_mock.c
```
