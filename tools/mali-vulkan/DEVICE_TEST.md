# Checkpoint 4B: logical device and queue, no rendering

Adds **Run Gamescope Vulkan device test** and actual Gamescope
`--vk-create-device-test`. Existing checkpoint 1/2/3/4A buttons and normal sessions
retain their behavior. Only this action enables `MALI_VULKAN_DEVICE_TEST=1`.
This document covers 4B; the separate 4C action is described in [SUBMIT_TEST.md](SUBMIT_TEST.md).

Phone facts supplied after successful 4A: Mali-G52, vendor `0x13b5`, device
`0x74021000`, API **1.1.131**, family 0 flags `0x7` with two queues. Robustness2
is absent. Timeline, scalar layout, YCbCr conversion, FP16 and Int8 were reported
supported; core `shaderInt16=1` and `VK_KHR_image_format_list=PRESENT`.
`dynamicRendering`, `presentId` and `presentWait` were **NOT QUERYABLE** because
the native API/required extensions were absent; these were not successful false
feature queries. DMA-BUF import but not export, and AHB import/export with dedicated-only
semantics, remain inventory findings; this checkpoint implements no transport.
Checkpoint 4B subsequently passed **twice on the real phone**: device creation,
family-0 queue acquisition, clean device/instance destruction and exit 0.

## Source inspection before editing

Downloaded the exact upstream [Gamescope 3.16.29 source](https://github.com/ValveSoftware/gamescope/tree/3.16.29)
and applied every existing DroidDeck patch through 0114 with zero fuzz before
implementing 0115. Source archive SHA256:
`9fe99fb4c7a80323cb7617825887dd8ff43e80a1e8c60a42740320eaa91ea01e`.
The locations below refer to that DroidDeck-patched tree; 0115 does not modify
`rendervulkan.cpp`, `rendervulkan.hpp`, backends or shaders.

1. **Version enforcement:** `CVulkanDevice::selectPhysDev`, `src/rendervulkan.cpp:356`,
   skips each physical device with `apiVersion < VK_API_VERSION_1_2`, before checking
   its queues. It later fails if no physical device was selected. Independently,
   `vulkan_get_instance`, line 3635, requests an instance API of 1.3.

2. **Vulkan 1.2 usage:** `createDevice` queries a `VkPhysicalDeviceVulkan12Features`
   through `vkGetPhysicalDeviceFeatures2` at lines 522–531, using `shaderFloat16`
   together with core `shaderInt16` to select FP16 support. Its creation chain at
   lines 672–678 enables **scalarBlockLayout**, **timelineSemaphore**, and
   **shaderFloat16 only when FP16 is supported**. All other Vulkan12 feature fields,
   including shaderInt8, descriptor indexing and buffer device address, remain zero.
   Actual `vkCreateDevice`, `vkGetDeviceProcAddr`, and `vkGetDeviceQueue` are core 1.0
   commands. Immediately after creation, the device dispatch table resolves the
   core 1.2 names `vkWaitSemaphores` and `vkGetSemaphoreCounterValue`; no such command
   executes merely to create a device or acquire its queue. Later renderer paths
   create timeline semaphores, chain `VkTimelineSemaphoreSubmitInfo` into submits,
   wait for timeline points and query counters (`rendervulkan.cpp:1079`, 1361,
   1403, 1420, 1427, 1572). `vkSignalSemaphore` is not in that table or invoked.
   Shader scalar layouts appear in `cs_sgsr.comp`, `cs_rgb_to_nv12.comp`,
   `cs_nis.comp`, `cs_nis_fp16.comp`, `cs_composite_blit.comp`, and blur shaders.
   Gamescope also uses **VkImageFormatListCreateInfo** (promoted to Vulkan 1.2)
   in modifier/image queries (2064, 2824, 2869), paired linear/sRGB mutable-image
   creation (2178), and mutable swapchain setup (3269). Its Vulkan 1.1 equivalent
   is `VK_KHR_image_format_list` / `VkImageFormatListCreateInfoKHR`; the extension
   was **PRESENT** in the real phone's 4A inventory and has no feature bit.
   Timeline maps to `VK_KHR_timeline_semaphore`, scalar layout to
   `VK_EXT_scalar_block_layout`, and FP16 arithmetic to `VK_KHR_shader_float16_int8`.
   The phone supports their queried bits, including shaderInt16=1 for the FP16
   selection. These facts make the >=1.2 rejection a version gate for those
   individual capabilities, but do not establish a safe normal-renderer 1.1 path:
   extension enabling, individual feature chains, KHR command dispatch and the
   separate missing renderer requirements still need implementation.

3. **Robustness2 requirement:** `createDevice`, line 599, unconditionally appends
   `VK_EXT_robustness2`. Its required-extension loop (lines 607–625) logs every
   missing extension and returns false before calling Vulkan device creation.
   Lines 686–690 chain `VkPhysicalDeviceRobustness2FeaturesEXT` with
   `nullDescriptor = VK_TRUE`.

4. **Exact robustness2 bits:** only **nullDescriptor** is enabled.
   **robustBufferAccess2** and **robustImageAccess2** are zero from aggregate
   initialization. No additional robustness2 feature or command is used. The
   renderer's `CVulkanCmdBuffer::bindDescriptors` initializes descriptor arrays
   to zero around line 1720, leaves image views null for unused texture slots,
   explicitly uses `VK_NULL_HANDLE` for missing shaper/3D LUT views at 1830/1834,
   and passes these arrays to `vkUpdateDescriptorSets` at 1851. That is real
   later descriptor behavior; removing the feature alone would not fix it.

5. **Creation versus rendering:** none of those 1.2/robustness2 features is
   inherently necessary for Vulkan to create an otherwise valid 1.0/1.1 device
   with a supported queue. They are requirements of Gamescope's unchanged renderer
   and its own create-info. That create-info would fail when requested extensions
   or bits are unsupported. A separate no-work diagnostic can omit them completely.

6. **Unconditional normal create-info:** four baseline extensions are
   `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`,
   `VK_KHR_external_semaphore_fd`, and `VK_EXT_robustness2`. True feature requests
   are scalarBlockLayout, timelineSemaphore, samplerYcbcrConversion, nullDescriptor,
   dynamicRendering (Vulkan13), presentId and presentWait (KHR). FP16 and core
   shaderInt16 are conditional; other core/Vulkan12/Vulkan13 feature fields are zero.
   PresentId/Wait **feature structures are unconditional** even though their
   extension names are added only for a swapchain backend. Swapchain backends
   additionally enable swapchain, swapchain_mutable_format, present_id, present_wait
   and optional HDR metadata; modifier support adds image_drm_format_modifier and
   queue_family_foreign. Backend `GetDeviceExtensions` adds its own list (Wayland
   returns empty). Realtime queue priority is conditional in the existing DroidDeck
   patch. Maintenance5 is inside `#if 0`, so is not requested. Dynamic rendering is
   used later in ReShade (`reshade_effect_manager.cpp:1859/1863`); it is not a
   Vulkan 1.2 feature and is not necessary for this diagnostic. The real 4A
   inventory cannot query dynamicRendering/presentId/presentWait on this driver
   because the relevant API/extensions are absent. Normal create-info still
   requests them; no functionality was changed for these audit corrections.

## What the diagnostic bypasses

It exits from main before scripts, X11, backend/renderer or Wayland initialization.
It never enters normal `CVulkanDevice::selectPhysDev/createDevice`, so those policies
and renderer requirements stay intact. It logs the real physical API and explicitly
logs the unchanged version/robustness2 blockers when present.

The glibc instance still requests API 1.0 (the proxy's implemented ceiling). The
broker retains the 4A native query instance: API 1.1 if supported by the Android
loader, otherwise API 1.0 with available query extensions. Logical-device creation
uses only core 1.0 create-info fields, compatible with the real Mali 1.1 driver.
No version or unsupported feature/extension is invented.

The diagnostic selects the reported ARM vendor/device IDs, queries properties,
core features, device extensions and queue families, and chooses the first family
with at least one **graphics + compute** queue. Family 0 is never assumed. It asks
for one queue, ordinary priority 1.0, **zero device extensions and zero features**.
Then it creates the native device, acquires a native queue, returns local proxy
objects, destroys the device and instance, and exits 0. Enumeration, missing Mali,
missing queue flags, creation or queue errors exit 1 with cleanup.
No image, buffer, GPU memory allocation, command pool/buffer, submission, WSI,
AHB/FD transport, Steam or DXVK operation is introduced.

## Version 4 protocol and lifetimes

`device_protocol.h` defines the schema. Versions 1–3 preserve their wire layouts
and cannot create devices. A session cannot change protocol versions after CREATE.
Version 4 inherits v3 capability queries and adds:

| Opcode | Request payload | Success after the common response prefix |
| --- | --- | --- |
| 14 DEVICE_CREATE | u32 physical ID, queue family, queue count, extension count; 55 explicit LE-u32 core feature selections in features_fields.def order; count LE binary32 priorities; fixed 256-byte extension names | count=1, u32 logical-device ID |
| 15 DEVICE_QUEUE | u32 logical ID, family, queue index | count=1, u32 queue ID |
| 16 DEVICE_DESTROY | u32 logical ID | count=0, no extra data |

This bounded subset accepts one ordinary queue create-info, at most four queues,
32 extension names, and 16 simultaneously live logical devices per connection.
Queue/device flags, layers and extension feature selections are implicitly zero;
no arbitrary pNext chain is serialized. Unknown application pNext nodes are rejected;
only loader-owned device bookkeeping is skipped on the glibc side.
All 55 core bits are named explicitly, not copied as a native feature struct.
NaN/out-of-range priorities, invalid bools/counts/lengths, duplicate or unterminated
extension names, unsupported feature/extension requests, invalid/stale IDs and
invalid family/index/count are rejected. The broker checks native capabilities
again before calling the driver.

The diagnostic ICD accepts **no enabled device extensions**, even when its debug
inventory reports native ones, because their device commands are not implemented.
Broker protocol validation supports explicit native extension requests for testing,
without exposing any extra device command or extension-feature support in the ICD.
V4 core feature selections can be enabled only when actually supported; the
Gamescope test selects none. Advanced feature selections stay disabled/rejected.

The Android session owns VkInstance/VkPhysicalDevice/VkDevice/VkQueue handles.
Device/queue IDs increase monotonically within a connection and are never reused;
queues belong to a device and die with it. Physical IDs retain their stable v2/v3
meaning. The ICD owns local objects with loader dispatch words and broker IDs.
No native handle, pointer or callback crosses the socket. Repeated queue lookup
preserves proxy identity. Explicit instance teardown, disconnect and broker stop
all destroy remaining native devices **before** their native instance. No wait-idle
command is needed because this implementation cannot submit any work.

## Validation

```sh
python3 tools/mali-vulkan/test_probe.py
VULKAN_HEADERS=/path/to/Vulkan-Headers/include python3 tools/mali-vulkan/test_icd.py
GAMESCOPE_SOURCE=/path/to/pristine/gamescope-3.16.29 \
  VULKAN_HEADERS=/path/to/Vulkan-Headers/include python3 tools/gamescope/test_vk_enumerate_only.py
bash -n tools/mali-vulkan/build-probe.sh tools/mali-vulkan/build-icd.sh tools/gamescope/build-in-arch.sh
git diff --check
```

Host validation: 8 probe, 18 ICD/broker, 17 Gamescope tests passed (43 total).
Includes v1/v2/v3 regressions; request serialization/fragmentation; invalid IDs,
queues, features and extensions; stale IDs; stable queues; supported core-bit
round trip; native create errors, missing queue/entry point; repeated device
creation/destruction; disconnect/stop/instance cleanup; no-family-0 assumption;
real host glibc loader dispatch; full zero-fuzz patch applicability and reverse
application; missing-ICD and no-renderer Vulkan symbol checks. Host mock results
are labelled HOST TEST ONLY and are not real Mali acceptance.

AArch64 glibc cross-build passed for the ICD, static probe, loader test, inventory,
Gamescope device diagnostic translation unit, and actual broker source through
its mocked Android/JNI harness. All 8 probe tests also passed under qemu-aarch64.
The complete packaged Gamescope executable, Android NDK/APK build and real phone
execution were not performed locally; the existing component/APK CI checks now
require the new option in help and in the staged/APK binary and exercise its
missing-ICD failure. Cross artifacts were built in /tmp, not substituted into Git.

## Build and real-phone acceptance

Run the existing **Build APK** workflow on `mali-gpu-experiment`, whose same-run
Gamescope job applies 0113–0115 and overlays the component before packaging.
The pinned p8 release predates all these diagnostics. A fresh ICD/broker alone
with the old Gamescope binary cannot run 4B. No commit/push/release pin change is
part of this work. For manual builds use the existing Gamescope component build,
then stage gamescope.tzst before Gradle as described in README.md; build both
`build-probe.sh` and `build-icd.sh` with an AArch64 glibc compiler. Keep the installed
runtime and compatible APK signing key.

```sh
adb install -r path/to/new.apk
adb shell am start -n com.droiddeck.launcher/com.droiddeck.launcher.gpu.SystemVulkanBrokerActivity
adb logcat -s MaliVulkanBroker:I '*:S'
```

Use the correct package prefix for a variant. With the runtime installed, rerun
**Run probe via Linux/proot**, **Run Vulkan ICD test**, **Run Gamescope Vulkan
enumeration test**, and **Run Gamescope Vulkan capability test**. Then press
**Run Gamescope Vulkan device test**. Expected new output includes:

```text
gamescope: Vulkan logical-device test
physical device: Mali-G52
physical API: 1.1.131
normal Gamescope remains blocked: physical API below the unchanged Vulkan 1.2 floor
real queue[0]: flags=0x7 count=2
queue family: 0 (verified graphics+compute)
requested extensions: none
requested features: none (all core and extension bits disabled)
MaliProxyICD: broker vkCreateDevice = VK_SUCCESS; proxy device ID=1
queue obtained = yes
logical device destroyed
instance destroyed
normal Gamescope rendering remains disabled
exit 0
Gamescope Vulkan device test via Linux/proot exit code=0
```

Merged output also shows the native extension count, absent robustness2 blocker,
real core feature values, and queue/proxy cleanup logs. Broker logcat must show
selected physical ID/name/API, family/count, enabled extensions/features (zero),
`broker vkCreateDevice result=0`, queue acquisition with IDs, `vkDestroyDevice`,
instance destruction and loader closure. Retain the whole output on failure.

Repeat the new action twice, rerun an old query action, then **Stop broker and
close** and confirm socket removal/cleanup. Missing runtime/assets/old Gamescope
must report a failure rather than simulate success. Do not start a normal session.
Manual guest invocation under the app UID, after staging through the button:

```sh
test_dir=/data/user/0/com.droiddeck.launcher/files/mali-vulkan
env VK_DRIVER_FILES="$test_dir/mali_proxy_icd.json" \
    VK_ICD_FILENAMES="$test_dir/mali_proxy_icd.json" \
    MALI_VULKAN_BROKER_SOCKET="$test_dir/broker.sock" \
    MALI_VULKAN_DEVICE_TEST=1 VK_LOADER_LAYERS_DISABLE='*' \
    /usr/local/bin/gamescope --vk-create-device-test
```
