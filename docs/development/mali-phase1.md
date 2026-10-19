# DroidDeck-Mali 0.5 port: Phase 1

Base: `mali-0.5-port` / `0.5.0` at `3c5e95af`.
Implementation source: `mali-gpu-experiment` at `3e1d9255`.

The three isolated source trees and the manual PanVK workflow contribute 181
reference files. The native implementation, proxy, protocol versions, header
schemas, PanVK pin and build options retain their reference bytes. PanVK remains
an isolated client-driver artifact and is not included in APK runtime inputs.

**Modified 0.5 files and reasons**

| File | Reason |
| --- | --- |
| `app/src/main/cpp/CMakeLists.txt` | Build the dormant `maliahb` registry/trace library and `malivulkan` broker library; retain all existing targets. |
| `.github/workflows/build.yml` | Trigger pushes to `mali-0.5-port`; generate Mali glibc assets after cache restoration, run isolated contracts, and invalidate native caches for their external headers. Preserve the 0.5 asset preparation, attestation and APK validation. |
| `.gitignore` | Ignore generated `assets/mali-vulkan` binaries and manifest, including the pre-existing untracked outputs. |
| `tools/build_local.sh` | Prepare the five Mali assets using the existing cross-compiler image and a Vulkan-header-only mount, before runtime-input recording. This script was not executed. |
| `tools/release/runtime_inputs.py` | Attest Mali sources, require and hash all five generated assets, and cover Gamescope source inputs for later overlays. Retain every existing required runtime asset. |
| `tools/tests/test_runtime_inputs.py` | Test missing Mali assets, changed protocol/native sources, and missing/stale Mali APK payloads. |

**Adaptations to added reference files**

- `.github/workflows/build-panvk-glibc.yml` uses the Phase 1 contract selector;
  its manual trigger, ARM64 Debian container, source pin, artifact modes and build
  recipe are preserved.
- `tools/mali-vulkan/tests/wait_attribution.c` supplies the final tracing fields
  and diagnostic callback expected by the reference implementation.
- `tools/mali-vulkan/test_frame_cost.py` includes the real disabled trace helpers
  required by the reference lookup function.
- `tools/mali-vulkan/test_perf.py` separates the isolated snapshot contract from
  its Gamescope-consumer assertion, preserving both tests.
- `tools/mali-vulkan/run_phase1_tests.py` is new. It selects independent host
  harnesses and parsers, reports individual deferred integration contracts, and
  keeps colliding Mali/PanVK test module names isolated.

**Build and artifact ordering**

Mali assets are built independently of the preload cache, then included through
0.5's existing `stageRuntimeAssets` path. Runtime-input recording and existing
APK checks run afterward. The required assets are `broker_probe`,
`libdroiddeck_mali_proxy.so`, `mali_proxy_icd.json`, `vulkan_loader_test`, and
`capability_inventory`.

`GAMESCOPE_FROM_SOURCE` remains `false`. Gamescope continues to use 0.5's pinned
release. A later phase must add the source-producing job/dependency and enable
that flag together. The prepared consumer downloads `gamescope-patched` from
that same run, verifies its checksum, and overlays it after cache restoration
and before runtime attestation. The cache key separates source/release modes
and includes Gamescope patch/build inputs. No Gamescope patch was added here.

The copied historical documentation describes later checkpoints as well as
Phase 1; it does not mean those Android/Steam paths are integrated in this branch.

**Lightweight validation**

- `python3 -B tools/mali-vulkan/run_phase1_tests.py`: 101 Mali host contracts and
  124 PanVK/tooling contracts passed. Unix socket tests required execution outside
  the filesystem/network sandbox. Only small temporary host harnesses were compiled.
- `python3 -B tools/mali-vulkan/run_phase1_tests.py --panvk-only`: 21 ANGLE probe
  contracts and 124 PanVK/tooling contracts passed; this is the manual PanVK CI selector.
- `python3 -B -m unittest discover -s tools/tests -p test_runtime_inputs.py -v`:
  14 tests passed, including the new attestation regressions.
- `python3 -B -m unittest discover -s tools/tests -p test_session_assets.py -v`:
  all 4 existing tests passed; `check_session_assets.py` is unchanged.
- Both workflow YAML files and every imported Python file parse; shell syntax
  checks pass for the changed local build script and imported build scripts.
- All-file `git diff --check` passes, including additions, using a temporary Git
  index without staging changes in the real index.

CI additionally selects `--with-loader` for small host Vulkan-loader/broker mocks.
That subset was not run locally because Vulkan development headers are absent.
No APK, Android native project, Gamescope or PanVK driver build was run locally.
The CI workflows were inspected, not executed remotely.

**Deferred to Phase 2 and subsequent phases**

- SystemVulkanBroker diagnostic activity/Kotlin wrapper and manifest registration.
- MaliNormalBroker, MaliSessionSelection, GPU detection and automatic selection.
- Android service/session lifecycle, filesystem/report-path adaptation and teardown wiring.
- Compositor linking, AHB protocol/presentation changes and real Android fence integration.
- Gamescope behavioral patches, their producer job, and patch-dependent host tests.
- Steam launch changes, PanVK receiver/client launcher, and real client proof.
- Shared staging activation and device validation; source and isolated transport tests
  are present but no session enables it.
- Android Mali Kotlin tests: they require the deferred lifecycle/selection classes.

`app/build.gradle`, compositor CMake and behavior, `.gitattributes`, all Java/Kotlin
sources, the Android manifest, session scripts, and `tools/wlroots` are untouched.
No proprietary Android Mali library was added. No commit was created.

**Every added or modified file**

The inventory excludes ignored generated assets that existed before this phase.
`A` means a new worktree file; `M` means an edited 0.5 file.

```text
A	.github/workflows/build-panvk-glibc.yml
M	.github/workflows/build.yml
M	.gitignore
M	app/src/main/cpp/CMakeLists.txt
A	app/src/main/cpp/malivulkan/capability_queries.h
A	app/src/main/cpp/malivulkan/device_commands.h
A	app/src/main/cpp/malivulkan/interop_commands.h
A	app/src/main/cpp/malivulkan/interop_consumer.c
A	app/src/main/cpp/malivulkan/interop_consumer.h
A	app/src/main/cpp/malivulkan/interop_objects.h
A	app/src/main/cpp/malivulkan/normal_commands.h
A	app/src/main/cpp/malivulkan/normal_ownership.c
A	app/src/main/cpp/malivulkan/normal_ownership.h
A	app/src/main/cpp/malivulkan/normal_wait_native.h
A	app/src/main/cpp/malivulkan/renderer_commands.h
A	app/src/main/cpp/malivulkan/renderer_objects.h
A	app/src/main/cpp/malivulkan/renderer_upload_dependency.h
A	app/src/main/cpp/malivulkan/session_commands.h
A	app/src/main/cpp/malivulkan/shared_staging_commands.h
A	app/src/main/cpp/malivulkan/submit_commands.h
A	app/src/main/cpp/malivulkan/submit_objects.h
A	app/src/main/cpp/malivulkan/system_broker.c
A	docs/development/mali-phase1.md
M	tools/build_local.sh
A	tools/mali-vulkan/CAPABILITIES.md
A	tools/mali-vulkan/DEVICE_TEST.md
A	tools/mali-vulkan/FRAME_COST.md
A	tools/mali-vulkan/GAMESCOPE_RENDERER.md
A	tools/mali-vulkan/INTEROP_TEST.md
A	tools/mali-vulkan/NORMAL_REUSE.md
A	tools/mali-vulkan/NORMAL_SESSION.md
A	tools/mali-vulkan/PERFORMANCE.md
A	tools/mali-vulkan/PERSISTENT_SESSION.md
A	tools/mali-vulkan/README.md
A	tools/mali-vulkan/RESOURCE_REGISTRY.md
A	tools/mali-vulkan/SHARED_STAGING.md
A	tools/mali-vulkan/STEAM_AFFINITY.md
A	tools/mali-vulkan/STEAM_CEF_BACKEND.md
A	tools/mali-vulkan/STEAM_CLIENT_ACCELERATION.md
A	tools/mali-vulkan/STEAM_FRAME_TRACE.md
A	tools/mali-vulkan/STEAM_PERFORMANCE.md
A	tools/mali-vulkan/STEAM_SWAP_TIMING.md
A	tools/mali-vulkan/STEAM_UI.md
A	tools/mali-vulkan/STEAM_X11_PRESENTATION.md
A	tools/mali-vulkan/SUBMIT_TEST.md
A	tools/mali-vulkan/UPLOAD_RING.md
A	tools/mali-vulkan/WAIT_ATTRIBUTION.md
A	tools/mali-vulkan/broker_probe.c
A	tools/mali-vulkan/build-icd.sh
A	tools/mali-vulkan/build-probe.sh
A	tools/mali-vulkan/capabilities.h
A	tools/mali-vulkan/capabilities_fields.def
A	tools/mali-vulkan/capability_icd.h
A	tools/mali-vulkan/capability_inventory.c
A	tools/mali-vulkan/capability_transport.h
A	tools/mali-vulkan/device_icd.h
A	tools/mali-vulkan/device_protocol.h
A	tools/mali-vulkan/diagnose_steam_cef.py
A	tools/mali-vulkan/diagnose_steam_perf.py
A	tools/mali-vulkan/diagnose_steam_x11.py
A	tools/mali-vulkan/features_fields.def
A	tools/mali-vulkan/frame_trace.c
A	tools/mali-vulkan/frame_trace.h
A	tools/mali-vulkan/frame_trace_proxy.h
A	tools/mali-vulkan/icd_proxy.c
A	tools/mali-vulkan/interop_entries.def
A	tools/mali-vulkan/interop_icd.h
A	tools/mali-vulkan/interop_protocol.h
A	tools/mali-vulkan/interop_test_api.h
A	tools/mali-vulkan/loader_test.c
A	tools/mali-vulkan/mali_proxy_icd.json
A	tools/mali-vulkan/mali_staging.inc
A	tools/mali-vulkan/mali_staging_pool.hpp
A	tools/mali-vulkan/mali_wait_profile.hpp
A	tools/mali-vulkan/normal_api.h
A	tools/mali-vulkan/normal_detail_perf.hpp
A	tools/mali-vulkan/normal_memory_perf.h
A	tools/mali-vulkan/normal_opcode_perf.h
A	tools/mali-vulkan/normal_perf.h
A	tools/mali-vulkan/normal_protocol.h
A	tools/mali-vulkan/normal_wait_perf.h
A	tools/mali-vulkan/probe_steam_angle.py
A	tools/mali-vulkan/properties.h
A	tools/mali-vulkan/properties_fields.def
A	tools/mali-vulkan/protocol.h
A	tools/mali-vulkan/renderer_entries.def
A	tools/mali-vulkan/renderer_icd.h
A	tools/mali-vulkan/renderer_icd_fwd.h
A	tools/mali-vulkan/renderer_protocol.h
A	tools/mali-vulkan/resource_registry.h
A	tools/mali-vulkan/run_panvk_phone_test.py
A	tools/mali-vulkan/run_phase1_tests.py
A	tools/mali-vulkan/shared_staging.h
A	tools/mali-vulkan/shared_staging_types.h
A	tools/mali-vulkan/staging_api.h
A	tools/mali-vulkan/steam_affinity.py
A	tools/mali-vulkan/steam_frame_details.py
A	tools/mali-vulkan/steam_frame_journal.py
A	tools/mali-vulkan/steam_swap_details.py
A	tools/mali-vulkan/submit_entries.def
A	tools/mali-vulkan/submit_icd.h
A	tools/mali-vulkan/submit_protocol.h
A	tools/mali-vulkan/test_bulk.py
A	tools/mali-vulkan/test_frame_cost.py
A	tools/mali-vulkan/test_frame_trace.py
A	tools/mali-vulkan/test_icd.py
A	tools/mali-vulkan/test_interop.py
A	tools/mali-vulkan/test_normal.py
A	tools/mali-vulkan/test_normal_launch.py
A	tools/mali-vulkan/test_normal_session.py
A	tools/mali-vulkan/test_perf.py
A	tools/mali-vulkan/test_probe.py
A	tools/mali-vulkan/test_registry.py
A	tools/mali-vulkan/test_renderer.py
A	tools/mali-vulkan/test_session.py
A	tools/mali-vulkan/test_shared_staging.py
A	tools/mali-vulkan/test_staging.py
A	tools/mali-vulkan/test_steam_angle_probe.py
A	tools/mali-vulkan/test_steam_cef_diagnostics.py
A	tools/mali-vulkan/test_steam_client_panvk.py
A	tools/mali-vulkan/test_steam_launch.py
A	tools/mali-vulkan/test_steam_x11_diagnostics.py
A	tools/mali-vulkan/test_upload_ring.py
A	tools/mali-vulkan/test_upload_trace.py
A	tools/mali-vulkan/test_wait.py
A	tools/mali-vulkan/tests/android/data_space.h
A	tools/mali-vulkan/tests/android/hardware_buffer.h
A	tools/mali-vulkan/tests/android/log.h
A	tools/mali-vulkan/tests/android/native_window.h
A	tools/mali-vulkan/tests/android/native_window_jni.h
A	tools/mali-vulkan/tests/android/rect.h
A	tools/mali-vulkan/tests/broker_mock.c
A	tools/mali-vulkan/tests/capability_contract.c
A	tools/mali-vulkan/tests/consumer_contract.c
A	tools/mali-vulkan/tests/device_contract.c
A	tools/mali-vulkan/tests/frame_trace_android.c
A	tools/mali-vulkan/tests/frame_trace_host.c
A	tools/mali-vulkan/tests/icd_contract.c
A	tools/mali-vulkan/tests/interop_contract.c
A	tools/mali-vulkan/tests/memory_bulk.c
A	tools/mali-vulkan/tests/mock_capabilities.h
A	tools/mali-vulkan/tests/mock_interop.h
A	tools/mali-vulkan/tests/mock_interop_types.h
A	tools/mali-vulkan/tests/mock_renderer.h
A	tools/mali-vulkan/tests/mock_submit.h
A	tools/mali-vulkan/tests/normal_android_release.c
A	tools/mali-vulkan/tests/normal_host.cpp
A	tools/mali-vulkan/tests/normal_outer.c
A	tools/mali-vulkan/tests/normal_ownership.c
A	tools/mali-vulkan/tests/renderer_contract.c
A	tools/mali-vulkan/tests/renderer_host.cpp
A	tools/mali-vulkan/tests/resource_registry.c
A	tools/mali-vulkan/tests/session_host.cpp
A	tools/mali-vulkan/tests/session_pool.cpp
A	tools/mali-vulkan/tests/shared_staging_transport.c
A	tools/mali-vulkan/tests/staging_dependency.c
A	tools/mali-vulkan/tests/staging_vulkan_stub.h
A	tools/mali-vulkan/tests/submit_contract.c
A	tools/mali-vulkan/tests/wait_attribution.c
A	tools/mali-vulkan/tests/watermark_staging.cpp
A	tools/mali-vulkan/upload_ring.h
A	tools/mali-vulkan/upload_ring_api.h
A	tools/panvk/analyze_frame_journal.py
A	tools/panvk/analyze_steam_frames.py
A	tools/panvk/build-glibc.sh
A	tools/panvk/frame_journal.py
A	tools/panvk/meson-options.txt
A	tools/panvk/package_glibc.py
A	tools/panvk/patches/0001-android-only-liblog.patch
A	tools/panvk/patches/0002-clc-supported-availability-warnings.patch
A	tools/panvk/perf_capture.py
A	tools/panvk/source.json
A	tools/panvk/stage_phone_test.py
A	tools/panvk/stage_raw_test.py
A	tools/panvk/summarize_affinity.py
A	tools/panvk/system_frame_capture.py
A	tools/panvk/test_affinity.py
A	tools/panvk/test_affinity_summary.py
A	tools/panvk/test_artifact.py
A	tools/panvk/test_frame_journal_control.py
A	tools/panvk/test_frames.py
A	tools/panvk/test_perf.py
A	tools/panvk/test_shm_decomposition.py
A	tools/panvk/test_swap_details.py
A	tools/panvk/test_upload_queue.py
A	tools/panvk/test_upload_sampling.py
A	tools/panvk/tests/fixtures/panfrost_icd.aarch64.json
M	tools/release/runtime_inputs.py
M	tools/tests/test_runtime_inputs.py
```
