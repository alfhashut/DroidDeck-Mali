# Checkpoint 4A: real driver capability inspection

This is a query experiment. It creates an Android Vulkan **instance**, never a logical device.
The existing three debug actions retain their previous protocol and behavior. The fourth action,
**Run Gamescope Vulkan capability test**, enables `MALI_VULKAN_QUERY_CAPABILITIES=1` only in its
GuestCommand children. Normal sessions and Turnip/freedreno selection are unchanged.

## What the output means

The action first runs `capability_inventory`, then the actual runtime
`/usr/local/bin/gamescope --vk-capabilities`, both through Linux/proot while the broker stays in
Android. GuestCommand/SessionFiles stages the APK Gamescope as in checkpoint 3.

There are three different versions/lists:

- **Android system loader instance API / raw Android instance extensions**: actual system
  `vkEnumerateInstanceVersion` and `vkEnumerateInstanceExtensionProperties`, without an instance.
- **glibc loader instance API / loader+ICD extensions**: the normal guest loader's global queries.
  The loader can add loader-owned extensions; this list is not the Android inventory.
- **Proxy API ceiling 1.0.0 / physical-device API**: the ICD deliberately retains its implemented
  1.0 instance ceiling. The physical-device version/properties are copied from real Android
  queries, including Properties2 when available. A loader reporting 1.3 does not upgrade Mali.

The proxy exposes only these actually-reported instance extensions, for their implemented query
commands: `VK_KHR_get_physical_device_properties2`, `VK_KHR_external_memory_capabilities`,
`VK_KHR_external_semaphore_capabilities`, `VK_KHR_external_fence_capabilities`.
Device extensions are the complete native **inventory**, with their real specVersion values;
this debug ICD cannot enable them on a device because it cannot create a device. No surface,
swapchain, device commands, or FD import/export operations are added or claimed.

The broker internally requests native API 1.1 when the Android loader supports it, otherwise 1.0
with only reported KHR query extensions. The native instance remains connection-owned. No Vulkan
handles or pNext chains cross the boundary.

The diagnostic prints all 55 core feature bits, queue flags/counts/granularity, memory heaps/types,
core physical-device properties, and selected Features2 fields: timelineSemaphore, scalarBlockLayout,
samplerYcbcrConversion, shaderFloat16, shaderInt8, nullDescriptor, robustBufferAccess2,
robustImageAccess2, dynamicRendering, presentId, presentWait. ID and DRM Properties2 fields are
queried when supported. This is a selected pNext implementation, not a general pNext transport.
Unknown or unavailable structures are left untouched and logged; the diagnostic distinguishes
**NOT QUERYABLE/NOT QUERIED** from an actual successful zero/false result.

Format queries cover BGRA8, RGBA8, ABGR2101010 and NV12. Linear/optimal image queries include
sampled, storage, color-attachment and combined usages. External image queries cover OPAQUE_FD,
DMA_BUF and ANDROID_HARDWARE_BUFFER only when their native extension is present. Reported DRM
modifiers include 64-bit values, plane counts and tiling flags, with per-modifier sampled DMA-BUF
image queries. External buffer queries use storage-buffer usage. Binary-semaphore/fence queries cover
OPAQUE_FD and SYNC_FD separately; timeline semaphore FD queries are not included. Importable/exportable/dedicated-only, compatible handle types,
and export-from-imported masks are returned unchanged. These are capability queries, not resource
creation, descriptor transfer, Android gralloc/nativewindow setup or proof of cross-process use.
Other formats/usages/handle types and AHardwareBuffer usage/output-specific pNext structures are
outside this diagnostic's scope.

## Version 3 wire schema

Versions 1 (one-shot probe) and 2 (enumeration ICD sessions) remain supported. Version 3 adds:

| Opcode | Request after the 16-byte header | Successful response after status/VkResult/count |
| --- | --- | --- |
| 6 GLOBAL | empty; connection closes after reply | u32 native instance API; count extension records |
| 7 CAPS | u32 physical-device ID | existing core properties codec, then fixed capability snapshot |
| 8 FORMAT | u32 ID, format | three u32 format masks, u32 modifier count, modifier records |
| 9 IMAGE | u32 ID, format, type, tiling, usage, flags, handle type, modifier-present; u64 modifier | extent/mips/layers/sample counts, u64 maxResourceSize, three external-memory masks |
| 10 BUFFER | u32 ID, flags, usage, handle type | three external-memory masks |
| 11 SEMAPHORE / 12 FENCE | u32 ID, handle type | features, export-from-imported, compatible masks |
| 13 SPARSE | u32 ID, format, type, samples, usage, tiling | count sparse-format records |

CREATE/LIST/PROPERTIES/DESTROY keep their version-2 encodings, using a version-3 header in this
mode. GLOBAL has no instance; subsequent CREATE uses a new connection. Status 6 means a query
was unsupported/not attempted, distinct from successful zero capabilities. Errors carry no
fabricated capability records.

All integers use explicit little-endian u32/u64 encoding. Extension records are 256 bytes of
NUL-terminated name plus u32 specVersion. Modifier records are u64 modifier, u32 planes,
u32 tiling flags. Snapshot fields are defined by `capabilities_fields.def` and `features_fields.def`;
fixed queue/memory/extension slots use explicit loops, never sizeof(VkStruct) copying. A queried
bitmask distinguishes unavailable Features2/Properties2 nodes from queried false fields.
Bounds: 16 devices, 256 extensions, 64 queue families, 32 memory types/16 heaps, 128 modifiers,
64 sparse formats, 131072 response bytes. Invalid counts, VkBool32 values, heap indices,
unterminated names, reply lengths/versions/opcodes fail validation. Exceeding a bound is an
error rather than silent truncation or a support claim. No pointers, callbacks, size_t values,
Android Vulkan handles, native enums/struct layouts or FD numbers are serialized directly.

## Gamescope 3.16.29 comparison

Reviewed the current DroidDeck-patched 3.16.29 `src/rendervulkan.cpp`: `CVulkanDevice::selectPhysDev`,
`CVulkanDevice::createDevice`, and `vulkan_init`. The diagnostic exits in main before scripts,
normal Vulkan renderer, backend and Wayland initialization; normal code is not changed.

**Observed on the real phone so far:** Vulkan 1.1.131, Mali-G52, vendor 0x13b5/device 0x74021000.
**Not observed yet:** the new extension, feature, format, external-memory and sync results.
The host tests use HOST TEST ONLY data, not a Mali profile. The table therefore separates known
findings from classifications to apply after the phone test. Never treat an unqueried value as absent.

A = directly supported when the real query confirms it. B = optional/existing fallback or a
plausible Gamescope fallback change. C = candidate for an implemented translation/emulation.
D = blocker for the current renderer/presentation path; not necessarily impossible to redesign.

| Requirement / evidence in normal Gamescope | Phone evidence now | Classification and what to inspect next |
| --- | --- | --- |
| Physical device API >=1.2 in selectPhysDev; normal instance requests 1.3 | Physical API 1.1.131 | **D known** for unchanged normal device selection. Lowering a version request alone does not implement 1.2/1.3 functionality. Android loader version is still to be measured. |
| Compute queue; graphics+compute general queue / presentation support depending on backend | Pending queue list | **A** if the appropriate real flags exist. **D** if required queue capabilities do not. No surface support query is possible in 4A. |
| scalarBlockLayout, timelineSemaphore, dynamicRendering set true in device feature chain | Pending Features2 | **A** if true. If absent: scalar layout needs shader/layout redesign (**B/C**, unproven); timeline needs real sequencing/translation (**C**, unproven); dynamic rendering needs render-pass translation or renderer changes (**C/D**, no demonstrated implementation here). |
| samplerYcbcrConversion set true | Pending Features2/formats | **A** if true. **B/C** if missing: format/shader fallback requires a Gamescope audit. No current general fallback was established. |
| VK_EXT_robustness2 and nullDescriptor unconditionally enabled | Pending native extension/feature | **A** if extension and feature exist. **C** candidate for canonical-zero descriptor resources when absent; requires device/resource/descriptor implementation and correctness tests. Other robustness2 semantics are not implied. |
| VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf, VK_KHR_external_semaphore_fd unconditionally required | Pending inventory and per-handle flags | **A** only for native query support with usable flags. Missing/zero support is **D** for current Linux buffer-sharing/presentation; an Android AHB path would need a separate real bridge (**C**, substantial future work). |
| DRM modifiers plus VK_EXT_queue_family_foreign | Pending extensions/formats | **B**: existing code disables modifiers without foreign queues and supports a modifier-free path. That does not remove the unconditional DMA-BUF requirement. |
| VK_EXT_physical_device_drm / render node validation | Pending DRM properties; /dev/mali0 is known | **B** when extension is absent (existing warning); **D** if an enabled backend requires a DRM node unavailable on Android. /dev/mali0 is not a DRM render node. |
| shaderFloat16 plus core shaderInt16 | Pending feature bits | **B**: existing m_bSupportsFp16 check enables both only when available; non-FP16 shaders remain. |
| swapchain, mutable-format, present_id/present_wait for swapchain backends | Pending native inventory/features; proxy has no WSI | **D** for current presentation without a real WSI implementation. The normal feature chain also requests presentId/presentWait; backend-specific fallback needs code changes (**B/C**), not invented bits. |
| HDR metadata | Pending extension | **B**: existing conditional enablement; no HDR claim when absent. |
| External fence FD | Pending inventory and query | Not one of the four unconditional createDevice extensions, but relevant future synchronization evidence. **A** if native flags support the required operation; otherwise protocol/sync redesign (**C/D**). |

## Public wrapper / Sarek source review

GameNative's defaults select a Bionic container and Wrapper-gamenative for non-Adreno in
[ContainerUtils](https://raw.githubusercontent.com/utkarshdalal/GameNative/master/app/src/main/java/app/gamenative/utils/ContainerUtils.kt).
The inspected public [wrapper instance code](https://raw.githubusercontent.com/leegao/bionic-vulkan-wrapper/wrapper/src/vulkan/wrapper/wrapper_instance.c)
loads Android libvulkan directly in-process and supplies Mesa WSI dispatch. It is not this glibc/socket architecture.
These public branches are references, not verified exact source revisions of any downloaded GameNative binary.

| Missing capability to study if the phone reports it | Verified public behavior / consequence |
| --- | --- |
| API version / various feature bits | gn_wrapper2 allows WRAPPER_VK_VERSION property overrides and sets several features. This experiment does neither. An overridden bit is not evidence of implementation. |
| presentWait | gn_wrapper2 ties it to native timeline support and can disable it with WRAPPER_DISABLE_PRESENT_WAIT. That suggests investigating optional timing, not promising it on this phone. |
| nullDescriptor | ARM D3D-engine-specific robustness2 advertisement is paired with separate resource emulation. Non-D3D Gamescope does not automatically match those engine-specific paths. |
| timeline/scalar/dynamic rendering on a 1.1 device | No fallback implementing these was established in the inspected wrapper files. Device creation can remove unsupported aggregate Vulkan feature structures; removing a request is not implementing the missing operations. |
| FD/DMA-BUF/modifier extensions | The wrapper appends native-supported external/FD/modifier/AHB extensions to native device creation. The reviewed files do not establish a universal replacement for absent native FD capabilities. |

Sources for those rows: [gn_wrapper2 physical-device queries/overrides](https://raw.githubusercontent.com/leegao/mesa-wrapper-CI/gn_wrapper2/src/vulkan/wrapper/wrapper_physical_device.c)
and [gn_wrapper2 device filtering, pNext handling and null resources](https://raw.githubusercontent.com/leegao/mesa-wrapper-CI/gn_wrapper2/src/vulkan/wrapper/wrapper_device.c).
The separate [compat_layer null-descriptor implementation](https://raw.githubusercontent.com/leegao/compat_layer/main/src/null_descriptors.cpp)
also uses real zero-filled buffers/images and descriptor fixups. This is a concrete future C candidate,
not permission to mark nullDescriptor true in a query-only proxy.

[Sarek's adapter](https://raw.githubusercontent.com/pythonlover02/DXVK-Sarek/main/src/dxvk/dxvk_adapter.cpp)
checks requested features against native bits, chains extension features conditionally, and uses queue
fallbacks. Its [project description](https://raw.githubusercontent.com/pythonlover02/DXVK-Sarek/main/README.md)
targets older Vulkan 1.1/1.2 consumers. A [Mali compatibility release](https://github.com/zeyadadev/DXVK-Sarek/releases)
makes clip/cull optional and offers process-local shared-texture fallback. Those change the consumer's
requirements or semantics; they do not supply Gamescope's Linux WSI or cross-process DMA-BUF protocol.
No wrapper/Sarek code or binaries were copied. Future reuse needs revision-specific license review;
public availability alone is insufficient. Vendor libraries stay on the phone.

## Build and phone test

Run **Build APK** on `mali-gpu-experiment` using the existing same-run Gamescope job/artifact flow.
No separate component release is required. The current patch stack includes unchanged 0113 and new
0114. CI checks help and missing-ICD behavior of the actual executable, and verifies both diagnostic
strings inside the APK. The existing preload cache already hashes all Gamescope patches/build scripts;
the current same-run component is overlaid even on a cache hit. The ICD build now bundles
`capability_inventory` alongside the existing probe, normal-loader test, manifest and library.

Install the built APK with the same compatible signing key (`adb install -r path/to.apk`). Keep the
installed Linux runtime; GuestCommand stages APK session assets. Open the shell-only screen:

```sh
adb shell am start -n com.droiddeck.launcher/com.droiddeck.launcher.gpu.SystemVulkanBrokerActivity
adb logcat -s MaliVulkanBroker:I '*:S'
```

Use the package variant's package name before `/` if needed. Run the three existing actions first,
then **Run Gamescope Vulkan capability test**. Both `capability_inventory` and Gamescope should
exit 0, print separate inventories, and finish with the destroyed-instance/no-device-backend message.
A missing runtime is reported without launching a command. Copy the complete new output before
classifying pending table rows; extension presence alone is insufficient for external resource use.
Press **Stop broker and close** after testing. No normal session or root/su launch is involved.

Local reproducible checks (host glibc loader and Vulkan headers required):

```sh
python3 tools/mali-vulkan/test_probe.py
VULKAN_HEADERS=/path/to/headers/include python3 tools/mali-vulkan/test_icd.py
GAMESCOPE_SOURCE=/path/to/pristine/gamescope-3.16.29 VULKAN_HEADERS=/path/to/headers/include \
  python3 tools/gamescope/test_vk_enumerate_only.py
bash -n tools/mali-vulkan/build-icd.sh tools/gamescope/build-in-arch.sh
git diff --check
```

The tests cover v1/v2 regressions, v3 wire fragmentation/bounds/invalid fields, truthful false features,
64-bit memory/modifier values, count/truncation semantics, pNext preservation, external capability
masks, instance cleanup, 1.0 fallback, actual glibc loader dispatch/export symbols, and the complete
Gamescope patch stack plus isolated diagnostic builds. A full APK/packaged Gamescope build and
real Mali capability results still require CI and the phone; host mocks are not those validations.

## Follow-up after real checkpoint 4A results

Checkpoint 4A passed on the real phone. The 4B handoff and exact Gamescope source
review are recorded in [DEVICE_TEST.md](DEVICE_TEST.md); that document supersedes
the pending-phone entries above. Capability-only mode stays query-only. The new
separate device-test mode creates/destroys a device and obtains a queue, with no rendering.

## Captured phone facts used by the 4B/4C audit

The supplied real Mali-G52 4A output reports `VK_KHR_image_format_list=PRESENT`
and core `shaderInt16=1`. `dynamicRendering`, `presentId`, and `presentWait` are
**NOT QUERYABLE (API/extension absent)**, rather than measured false feature bits.
The older "Pending" audit rows above describe the initial pre-phone investigation;
these captured results supersede them. See [DEVICE_TEST.md](DEVICE_TEST.md) for
Gamescope's image-format-list use and [SUBMIT_TEST.md](SUBMIT_TEST.md) for 4C.
