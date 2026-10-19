# Checkpoint 4D: independent memory, image, AHB and Android-consumer diagnostics

All four diagnostics are built into Gamescope patch `0117-vulkan-memory-ahb-tests.patch`
and the same APK. Each creates its own instance/device, runs independently, and exits
before normal Gamescope startup. Checkpoints 1–4C are preserved. The user reports 4C
passed twice on the real Mali-G52, physical Vulkan 1.1.131.

**Status:** implementation and host/cross-build validation are complete; 4D phone
results and the full APK build are pending. Host mocks are not Mali results.
Normal Gamescope's Vulkan 1.2 floor and renderer requirements remain unchanged.

| Stage | Gamescope option | Debug button | PASS evidence |
|---|---|---|---|
| 4D1 | `--vk-buffer-memory-test` | Run Vulkan buffer memory test | 1024 actual readback words equal `0x12345678` |
| 4D2 | `--vk-image-memory-test` | Run Vulkan image memory test | 4096 actual RGBA pixels equal `(64,128,191,255)` |
| 4D3 | `--vk-ahb-test` | Run Vulkan AHardwareBuffer test | Same imported AHB image passes GPU readback, actual Android description, producer FD wait, and CPU verification when lockable |
| 4D4 | `--vk-ahb-present-test` | Run Vulkan AHB presentation test | Same 256×256 AHB passes quadrant readback and SurfaceControl completion/release |

## Buffer and image paths

4D1 creates a 4096-byte transfer source/destination buffer, queries real memory
requirements, and selects a compatible HOST_VISIBLE memory type, preferring
HOST_COHERENT. No memory type index is hardcoded. It allocates/binds, maps and
zeroes the memory, flushes if noncoherent, and unmaps. A primary command buffer
records a real `vkCmdFillBuffer` and a transfer-write to host-read buffer barrier.
A real queue submission/fence wait precedes mapping/invalidation and word verification.

4D2 creates a 64×64 optimal RGBA8_UNORM image with one mip/layer/sample, transfer
source/destination usage, real requirements and compatible memory, preferring
DEVICE_LOCAL. A HOST_VISIBLE staging buffer receives a real GPU image-to-buffer
copy after UNDEFINED→TRANSFER_DST, clear `(0.25,0.5,0.75,1)`, and TRANSFER_DST→TRANSFER_SRC
barriers. A transfer-write→host-read barrier and fence wait precede byte verification.
All synchronization commands are legacy/core Vulkan commands.

Mapping uses a bounded glibc mirror of the **real native mapped allocation**, not
an invented GPU result. RPC uploads/downloads use 4096-byte chunks. Coherent
unmap uploads the mirror; noncoherent flush uploads then calls native flush.
Invalidate calls native invalidate before downloading. Noncoherent map does not
read native bytes until invalidate. Maps currently require offsets aligned to the
real `minMemoryMapAlignment`; diagnostics map whole allocations at offset zero.
The broker enforces `nonCoherentAtomSize`, allocation bounds and mapped ranges.
The proxy checks atom alignment before uploading an invalid flush range.

## AHB import and ownership

AHB is the primary external-image path. The user's real 4A result is importable=1,
exportable=1, dedicated-only=1 for RGBA8. DMA_BUF importable=1/exportable=0 is not
used for export. Those observations are not hardcoded as successful runtime queries.

The broker enables a private diagnostic service only when the selected physical
API and native instance are at least 1.1 and the actual extension inventory includes:

- `VK_ANDROID_external_memory_android_hardware_buffer`
- `VK_EXT_queue_family_foreign`
- `VK_KHR_external_fence_fd`

The guest enables no device extensions/features. The private helper
`vkDroidDeckInteropTEST` is not advertised as a Vulkan extension. The public ICD
still exposes a bounded diagnostic subset, not a normal Vulkan renderer.

For each AHB, the broker re-queries the exact RGBA8 optimal image configuration
with transfer, sampled and color-attachment usage, checks native importability,
compatible handle type, extent and sample support, and preserves query failure
results including `VK_ERROR_FORMAT_NOT_SUPPORTED`.

It allocates an actual Android RGBA8 AHardwareBuffer with sampled/color-output/
composer usage. It first requests CPU read usage; if Android rejects that usage,
it tries an actual GPU-only allocation and explicitly reports lack of CPU lockability.
No allocation is fabricated. `vkGetAndroidHardwareBufferPropertiesANDROID`, including
its format-properties chain, supplies the real allocation size, format/features
and compatible memory-type bits. The broker selects a compatible type and creates
an external-memory image. `VkImportAndroidHardwareBufferInfoANDROID` and
`VkMemoryDedicatedAllocateInfo` are passed to native allocation with exactly that
AHB allocation size, then the image is bound at zero. Image requirements are queried
**after** binding, as required for AHB images.

This is an **import** path. `vkGetMemoryAndroidHardwareBufferANDROID` is intentionally
not called: this allocation has no `VkExportMemoryAllocateInfo`, required by its
01882 valid-usage rule. Android already owns the original handle. Adding an invalid
export call merely to exercise the function would violate the API contract.

The broker owns the original AHB reference, Vulkan owns an additional import
reference, and presentation temporarily owns a consumer reference plus the platform's
transaction reference. Destruction frees commands before referenced images/buffers,
then frees bound memory after destroying its resource, then releases the original
AHB token. AHB release rejects live image/memory bindings. References remain alive
through producer execution and consumer release. Disconnect/device teardown drains
pending GPU work and unmaps remaining mappings before destroying native resources.

AHB pointers never cross the socket. The proxy receives typed image/memory handles
and connection-local AHB tokens. Presentation occurs in the same Android process;
no cross-process pointer/FD transport is needed. Existing `banner_ahb_v1` native-handle
transport and desktop DMA-BUF ingestion are not involved in this isolated test.

## Binary external synchronization

The header-defined types are:

- external semaphore `0x10`: `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT`
- external fence `0x8`: `VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT`

Only the simpler **binary fence SYNC_FD** path is implemented. The broker checks
native fence exportability/compatible type, creates a fence with `VkExportFenceCreateInfo`,
submits the producer, then calls real `vkGetFenceFdKHR` **before waiting**.
SYNC_FD export copies/resets the fence payload; the diagnostic waits the exported
FD, not that fence. Export happens once per fence. A returned FD of -1 means an
already-signaled payload. Other FDs remain broker-owned behind typed sync tokens.

Imported images acquire FOREIGN ownership while discarding undefined initial
contents, receive the Vulkan write and readback, then release to
`VK_QUEUE_FAMILY_FOREIGN_EXT` in GENERAL layout. The broker commits planned
layout/ownership only when producer completion is observed. AHB inspection and
presentation require a sync token belonging to the producer of **that image**;
a signaled unrelated fence does not authorize access. They wait/reject an
unsignaled producer before acquiring/locking the buffer. Pending resources and
sync tokens cannot be freed. No timeline semaphore sharing is implemented.

## Android handoff and release

Before implementation, the existing `waylandcomp/src/sc_layer.c`, `ahb_swapchain`
AHB protocol and scanout pipeline were inspected. The isolated consumer uses that
same Android SurfaceControl mechanism and C ABI/dlsym pattern: setBuffer with a real
AHB/acquire FD, completion callback, previous-buffer release fence, blank-buffer
retirement, hide/reparent and final release. It does not start the normal compositor
or its Turnip Vulkan pipeline. API 29 functions are resolved dynamically so the
broker library still loads at the app's API 26 floor; absence is a reported failure.

4D4 initializes CPU staging with red/green/blue/white quadrants, then uses real
`vkCmdCopyBufferToImage` to produce the **actual imported AHB image on Mali**.
GPU readback verifies all four channels and quadrant positions before handoff.
The consumer displays this exact AHB; it never creates a CPU duplicate of that frame.
The tiny blank retirement AHB is only used to detach the original buffer safely.

A dedicated SurfaceView remains visible above the debug controls. The consumer
retains its native window during a run, holds the AHB, duplicates the borrowed
producer FD for SurfaceControl (which owns that duplicate), and waits the transaction's
OnComplete plus present fence when available. Android's present-fence -1 means the
device lacks hardware present-fence support; this is logged separately, and the
OnComplete callback still proves consumer completion. The frame stays visible for
one second. Retirement's OnComplete and previous-release fence must complete before
consumer refs are dropped and success is returned. Repeated tests create fresh buffers.
Surface callbacks do not acquire the Kotlin monitor held by the running diagnostic.

For safety, a missing platform completion callback or permanently broken release
fence can hold teardown rather than free a buffer still in use. SurfaceControl
completion/release waits are intentionally not treated as permission to free after
a timeout. The diagnostic screen must remain visible during the phone test.

## RPC v6 and bounded subset

`interop_protocol.h` describes every wire field. Version 6 appends one service flag
to device CREATE (0 memory-only, 1 native AHB service); v1–v5 payloads/behavior are
preserved and a connection cannot upgrade its version. Native results/statuses are
preserved in v5/v6. The schema never transports pNext, pointers or native descriptors.

| Opcodes | New operations |
|---|---|
| 32–34 | buffer create/destroy/requirements |
| 35–37 | memory allocate/free, buffer bind |
| 38–43 | map/unmap, bounded read/write, flush/invalidate |
| 44–47 | image create/destroy/requirements/bind |
| 48–51 | fill, barrier, clear, tightly packed image↔buffer copy |
| 52–53 | dedicated AHB import/create, AHB release |
| 54–57 | exportable fence create, SYNC_FD export/wait/close |
| 58–59 | AHB description/pixel inspection, Android presentation |

New typed resources: buffer, memory, image, AHB token and sync token. IDs are
monotonic across resource classes and connection-local; lookup also requires the
owning device. Invalid/destroyed/cross-device IDs, double free, bound/mapped memory
free, pending resources, invalid types/typeBits, misaligned or overflowing binding,
unsupported image transitions/copy extents, repeated export and unsupported handle
types are rejected. Referenced resources cannot be destroyed until their command
buffer is freed, a deliberately stricter diagnostic rule. Only one image is recorded
per command buffer; no arbitrary command stream, alias bindings or concurrent
resource submissions are supported. Ordinary allocations are limited to 1 MiB,
images to 256×256 RGBA8. Unsupported public pNext/allocator/sharing/format variants
are rejected and failed void recording prevents successful End/Submit.

New public core entrypoints are exactly those in `interop_entries.def`: buffer,
memory, map/flush/invalidate, image, fill/barrier/clear, image-to-buffer and
buffer-to-image copy. Native-only external calls are AHB properties and fence-FD
export. No swapchain, Android surface Vulkan extension, dynamic rendering,
robustness2, Vulkan 1.2 or DMA_BUF export support is claimed.

## One APK and real-phone sequence

GitHub Actions builds remote source, not this uncommitted working tree. After a
revision containing these edits exists on `mali-gpu-experiment`, run **Build APK**
(`.github/workflows/build.yml`) using that branch, or use its automatic push run.
This task does not commit, push or dispatch a build against an older revision.
The same run must finish its **Build experimental Gamescope** job and **Build APK**
job. It applies the entire patch stack, checks all eight diagnostic options in the
packaged Gamescope binary, overlays that same-run component despite preload cache
hits, rebuilds the glibc ICD and Android broker, and checks the APK's embedded binary.
Download the `droiddeck-apk` artifact. Do not reuse an APK or Gamescope artifact from
4C. No rebuild is needed between the four 4D buttons.

Install that APK, keep the existing Linux runtime installed, and launch:

```sh
adb shell am start -n com.droiddeck.launcher/.gpu.SystemVulkanBrokerActivity
adb logcat -v threadtime -s MaliVulkanBroker
```

Press, in exactly this order:

1. Run probe via Linux/proot.
2. Run Vulkan ICD test.
3. Run Gamescope Vulkan enumeration test.
4. Run Gamescope Vulkan capability test.
5. Run Gamescope Vulkan device test.
6. Run Gamescope Vulkan submit test (4C).
7. Run Vulkan buffer memory test (4D1).
8. Run Vulkan image memory test (4D2).
9. Run Vulkan AHardwareBuffer test (4D3).
10. Run Vulkan AHB presentation test (4D4), watching red/green above blue/white.
11. Repeat the AHardwareBuffer test (4D3).
12. Repeat the AHB presentation test (4D4).
13. Stop broker and close; reopen and repeat if testing Activity teardown.

Require each diagnostic's exit code 0, its own PASS/readback evidence, and full
cleanup. For 4D4 additionally inspect the visible channel order and completion/
release logs. Preserve exact negative VkResult/Android failure logs if a stage fails;
do not continue interpreting subsequent stages as proven. No normal Gamescope
session, Steam, Proton, DXVK or checkpoint 5 renderer is started.

## Validation and references

Host tests exercise the actual broker/ICD with an explicitly mocked Android driver,
including coherent/noncoherent visibility, exact readback, error preservation,
layout/extent/binding/pNext rejection, AHB dedicated allocation/refcounts, unrelated
sync tokens, unsignaled producer, export failure, repeated consumption, disconnect
and teardown. A separate test compiles the actual consumer against asynchronous
SurfaceControl mocks, checking exact-buffer identity, delayed producer/present/release
fences, surface destruction and FD/reference balance. Gamescope tests apply every
patch with zero fuzz and compile/run the independent translation units through the
normal Vulkan loader. Validation completed on this working tree:

- 49 Mali tests: original 8 probe + 27 ICD/broker, and 14 new interop/consumer tests.
- 25 Gamescope tests: original 21 plus 4 independent-diagnostic/interop checks.
- 41 AArch64 QEMU tests: 27 ICD/broker plus 14 interop/consumer tests.
- Repository regressions: 297 run, 296 passed, 1 skipped in a no-space temporary
  checkout and isolated D-Bus session. This avoids two pre-existing launcher-test
  failures caused by the space-containing workspace path.
- Actual Android NDK r27d, Android API 26 AArch64 broker/consumer shared-library
  build passed with `-Wall -Wextra -Werror`, defined-symbol linking and 16 KiB LOAD
  alignment. The library imports libandroid/libdl/liblog/libc and dynamically loads
  system Vulkan; it does not link a replacement driver.
- AArch64 glibc proxy, loader test, inventory, probe and broker/consumer mock builds
  passed; the new Gamescope translation unit also cross-compiled.
- Full Gamescope patch stack applied with zero fuzz; diagnostic patch whitespace,
  actionlint, all 28 workflow shell blocks, embedded Python and diff checks passed.
- Gradle `testReleaseUnitTest --offline` could not reach compilation: this machine
  has Java 27 (class-file major 71), unsupported by Gradle 8.10.2. Full Android
  Gradle unit/APK builds remain pending in CI with its configured Java 17/SDK.
  Neither a new APK nor real 4D phone results are claimed.

- [AHB Vulkan rules, memory allocation and ownership](https://docs.vulkan.org/spec/latest/chapters/memory.html)
- [AHB image requirements after binding](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetImageMemoryRequirements.html)
- [GetMemoryAHB export-allocation requirement](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryGetAndroidHardwareBufferInfoANDROID.html)
- [Foreign queue ownership extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_queue_family_foreign.html)
- [Fence-FD copy/export semantics](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetFenceFdKHR.html)
- [Android SurfaceControl completion/acquire/release contracts](https://developer.android.com/ndk/reference/group/native-activity)
