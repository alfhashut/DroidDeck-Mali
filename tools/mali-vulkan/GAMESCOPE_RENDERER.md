# Checkpoint 5: exact-source renderer audit

Audit completed before implementation, against Gamescope **3.16.29**, applying every
repository patch in lexical order through `0117-vulkan-memory-ahb-tests.patch`,
with zero fuzz. Repository baseline: `798ae0f1c9645f036678095c3d5b776a825cf03d`.
The reconstructed source matches the checkpoint-4D tree byte for byte.
`src/rendervulkan.cpp` SHA256:
`7c12a26918aada3a1bd1ab72fc1f03502aa82bc5ded011e4d7912c150fe150f4`.
All line references below refer to that pre-checkpoint-5 patched source.

## Version policy versus functionality

| Actual source requirement | Vulkan 1.1 equivalent | Captured Mali evidence / diagnostic decision |
| --- | --- | --- |
| `selectPhysDev`, line 359: reject API <1.2 | No operation; a policy gate | Physical API stays **1.1.131**. Bypass only in the two Mali renderer diagnostics after capability validation. |
| `createDevice`, lines 672–679: `timelineSemaphore=1` | `VK_KHR_timeline_semaphore`, individual `VkPhysicalDeviceTimelineSemaphoreFeaturesKHR` | Extension present, queried bit 1. Required; use real driver semaphore operations. |
| Same block: `scalarBlockLayout=1` | `VK_EXT_scalar_block_layout`, individual `VkPhysicalDeviceScalarBlockLayoutFeaturesEXT` | Queried bit 1. Required by the shader's scalar UBO at `shaders/blit_push_data.h`. Check extension presence separately. |
| Same block: conditional `shaderFloat16` | `VK_KHR_shader_float16_int8`, individual `VkPhysicalDeviceShaderFloat16Int8FeaturesKHR` | Queried bit 1; optional. Prefer existing FP32 BLIT path. Neither float16 nor int8 nor core shaderInt16 needed for this diagnostic. |
| `WaitSemaphores`, lines 1403,1572 | `vkWaitSemaphoresKHR` | Real KHR operation, never CPU timeline emulation. |
| `GetSemaphoreCounterValue`, lines 1420,1427 | `vkGetSemaphoreCounterValueKHR` | Real KHR operation. |
| `VkSemaphoreTypeCreateInfo`, lines 1079,1479,1510 | KHR alias | Real timeline creation. |
| `VkTimelineSemaphoreSubmitInfo`, line 1360 | KHR alias passed to core `vkQueueSubmit` | Real GPU signal of internal sequence number. |
| `VkImageFormatListCreateInfo`, lines 2178,2824,2869,3269 | `VK_KHR_image_format_list` | Extension PRESENT in supplied 4A inventory. Required for ordinary mutable UNORM/sRGB textures. Used by the diagnostic input and ordinary dummy textures; the single-format AHB target needs no format list. |

Those are **all** Vulkan 1.2 feature fields enabled and all 1.2 core commands
called in this renderer. Other `VkPhysicalDeviceVulkan12Features` members are
zero. No buffer-device-address, descriptor-indexing, host-query-reset,
draw-indirect-count, or synchronization2 requirement was found. `CmdResetQueryPool`
is a Vulkan 1.0 recording command, not 1.2 host `ResetQueryPool`.

The extension/core aliases follow the official
[timeline](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_timeline_semaphore.html),
[scalar layout](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_scalar_block_layout.html),
[float16/int8](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_float16_int8.html), and
[image format list](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_image_format_list.html)
definitions. Promotion does not make optional feature bits automatically true.

## Genuinely unavailable functionality and unreachable paths

`VK_EXT_robustness2` is unconditionally appended by normal `createDevice` at
line 600. Lines 686–690 enable **only nullDescriptor**. Aggregate zero
initialization leaves `robustBufferAccess2=0`, `robustImageAccess2=0`. The phone
has no extension: this is unavailable functionality, not a version gate.
Valid dummy bindings remove its need in the diagnostic; they do not implement
or advertise robustness2. Normal behavior must retain both requirements.

Normal device creation also chains Vulkan13 `dynamicRendering=1`, presentId,
and presentWait unconditionally (lines 635–658), even with no swapchain.
The phone reports these **NOT QUERYABLE (API/extension absent)**. They cannot
be enabled. `vulkan_composite`'s ordinary BLIT path uses **compute**, no render
pass and no dynamic rendering. The only actual `CmdBeginRendering` /
`CmdEndRendering` / graphics-pipeline calls are in
`reshade_effect_manager.cpp` (1859,1863,1658). Diagnostic ReShade must be disabled
explicitly, including its initialization; omit those feature requests only
in the isolated diagnostic. No render-pass translation is required.

Normal `createDevice` unconditionally enables external memory FD, DMA-BUF,
and external semaphore FD. They serve desktop import/export and client
explicit synchronization. None is needed for the chosen single RGB layer,
private internal timeline, and Android AHB output. DMA-BUF export is genuinely
unavailable in the captured phone path. Do not use it. AHB import and SYNC_FD
fence export are already proven on the phone; **storage-image AHB use is a new
capability requirement**, and must be queried before a compute dispatch.

## Every actual null-descriptor site

Repository-wide descriptor-write search finds writes only in
`rendervulkan.cpp` and `reshade_effect_manager.cpp`.

| Site | Binding/type and shader shape | Why null / valid diagnostic replacement |
| --- | --- | --- |
| `CVulkanCmdBuffer::dispatch`, 1799–1816 | 3: combined image sampler, 16 × sampler2D | Unbound slots and YCbCr inputs leave regular image views zero. Every sampler is already a real cached sampler. Use initialized transparent 2D sampled image for absent regular views. |
| Same loop | 4: combined image sampler, 16 × sampler2D | All non-YCbCr inputs leave views zero. Normal binding uses immutable YCbCr sampler. Diagnostic supports **RGB only**, ycbcrMask=0; use ordinary real immutable sampler and ordinary valid 2D image for inactive binding. Do not claim NV12 conversion support in this path. |
| `dispatch`, 1820–1834 | 5: combined image sampler, 2 × sampler1D | Missing shaper LUT. Use actual initialized 1D sampled image, with normalized real sampler and matching layout. |
| Same block | 6: combined image sampler, 2 × sampler3D | Missing 3D LUT. Use actual initialized 3D sampled image, not a 2D view. Color management is disabled for this diagnostic, so neither LUT is sampled. |
| `dispatch`, 1837–1848 | 2: storage image, image2D | RGB output fills only target[0]; chroma target[1] stays zero (including its ignored sampler). Use separate real zero-initialized 2D storage image in GENERAL. BLIT writes only binding 1. |
| `ReshadeEffectPipeline::execute`, 1735–1755 | Storage image of effect-defined type | Failed `findTexture` yields null view. **Not reached**: ReShade disabled. An arbitrary effect may read/write this resource; no universal safe zero-image substitute claimed. Reject enabling effects in the diagnostic. |

Binding 0 UBO is always the real upload buffer; no null uniform/storage buffer,
standalone sampler, standalone sampled-image descriptor, texel buffer, or
other null descriptor use exists in the audited source. Null handles used for
optional fences, pipeline cache, base pipeline, allocator, or swapchain
ownership are **not descriptors**. ReShade combined samplers fall back to a
valid input image rather than a null view.

Dummies need SAMPLED / STORAGE usage appropriate to each binding, TRANSFER_DST
for real zero initialization, native allocated/bound memory, correctly typed
views, transfer-write → compute-read/write barriers, and GENERAL (or matching
shader-read-only) layout. Only inactive LUT bindings may use zero LUTs: zero
LUTs would not preserve enabled color-management behavior. This diagnostic
must prohibit that path. No robustness feature may be advertised as a result.

## Actual renderer execution and reached API set

`CVulkanDevice::BInit` normally calls `selectPhysDev`, `createDevice`,
`createLayouts`, `createPools`, `createShaders`, `createScratchResources`,
then starts background pipeline precompilation and ReShade. The diagnostic
must use those actual classes and initialization methods with diagnostic-only
branches to avoid backend calls, YCbCr setup, background compilation of unused
pipelines, and ReShade. Build only the existing BLIT shader/pipeline for the
one-frame subset, not a custom diagnostic shader.

`createLayouts`: seven existing bindings; `CreateDescriptorSetLayout`,
`CreatePipelineLayout`. `createPools`: two RESET_COMMAND_BUFFER command pools,
three descriptor-pool types, 24 sets (8 concurrent submits ×3).
`createScratchResources`: descriptor allocation, upload buffer with
TRANSFER_SRC|UNIFORM_BUFFER, memory requirements, native memory selection,
allocation, binding, mapping; internal timeline semaphore. Normal upload
capacity is 8,294,400 bytes, larger than checkpoint 4D's 1 MiB bound. A smaller
diagnostic capacity or explicitly bounded renderer allocation is necessary.
UBO offsets must meet the actual `minUniformBufferOffsetAlignment`, not merely
the upstream upload ring's 16-byte alignment. The seven-binding layout requires
**36 per-stage combined samplers/sample images**, two storage images, one UBO,
39 per-stage resources, one bound set and corresponding descriptor-set limits,
even for a single active layer. The diagnostic checks those actual limits before
creating a device; the broker checks them before pipeline-layout creation.
The existing [pipeline-layout valid usage rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineLayoutCreateInfo.html)
apply to layout counts, not merely the active synthetic layer. An unmet limit
is genuinely insufficient functionality for this unchanged layout, not a version
policy. No captured descriptor-limit values are assumed to be sufficient.
Compute requires 8x8x1 local size/64 invocations, 32x32x1 group-count capacity;
image dimensions and the actual BLIT UBO range are also checked.

`createShaders`: normal renderer creates nine shader modules (FP32 EASU/NIS
selected if FP16 disabled). The chosen path needs BLIT only.
`compilePipeline`: `CreateComputePipelines`, one compute stage `main`, one
existing pipeline layout, seven scalar specialization entries; no pipeline
cache object, graphics pipeline, push constants, or custom SPIR-V.
Meson uses glslang `-V` (default Vulkan1.0/SPIR-V1.0), no forced Vulkan1.2 target.
Shader transport must preserve bounded word-aligned SPIR-V bytes exactly.
Disassembly of the actual generated BLIT module declares only `Shader`,
`Sampled1D` and `ImageQuery`; it does not require float16, storage writes without
format or descriptor-array dynamic-indexing feature bits.

`CVulkanTexture::BInit`: ordinary input texture creation uses optimal RGBA8,
MUTABLE_FORMAT and format-list UNORM/SRGB views, memory requirements,
DEVICE_LOCAL allocation, binding, image-view creation; sRGB view usage has
`VkImageViewUsageCreateInfo` (Vulkan1.1 maintenance2). Dummies additionally need
1D and 3D images/views. AHB output must wrap the already-imported native image
with a storage-compatible **UNORM** view, without taking swapchain semantics
or exporting a DMA-BUF. Query STORAGE support in the exact AHB format/usage.

Input upload uses actual Gamescope `vulkan_create_texture_from_bits` /
`CVulkanCmdBuffer::copyBufferToImage`. Frame path:
`FrameInfo_t` → `vulkan_composite` (output override, no FSR/NIS/SGSR/blur/effect)
→ `bind_all_layers` → existing BLIT constants/pipeline →
`CVulkanCmdBuffer::dispatch` → actual descriptor writes/binds/compute dispatch
→ `CVulkanDevice::submitInternal` / `submit` → native KHR timeline signal/wait.
Output override suppresses output rotation, so use the existing layer
scale/offset controls for a deterministic centered source with black border,
or explicitly select the existing rotation constant without a custom shader.
Selected verification must cover every colored quadrant and the border.

Recording calls: Allocate/Free/Begin/End/ResetCommandBuffer,
CmdCopyBufferToImage, CmdPipelineBarrier (multiple images, GENERAL layout,
ALL_COMMANDS scopes, shader accesses), CmdBindPipeline, UpdateDescriptorSets,
CmdBindDescriptorSets, CmdDispatch. Source upload and shader constants need
real coherent visibility through the proxy; a permanently mapped local mirror
alone does not make native data visible. Timeline submit has no fence upstream;
exported SYNC_FD handoff needs an additional safe submission or diagnostic
producer fence tied to the actual AHB-producing work. Optional composite timing
is gated by debug logging: explicitly disable it, so query pools/timestamps
are not reached. No `vkSignalSemaphore` host operation is reached.

Teardown must wait for real completion, free/reset commands before referenced
textures, destroy views before images, release descriptors/pipelines/shaders,
semaphore/pools/layouts/upload memory/device/instance, and retain exact AHB
through Android previous-buffer release. Upstream has no CVulkanDevice
teardown method; add a diagnostic-only explicit cleanup with failure-safe
partial initialization. Native device idle is necessary on errors/device loss.

## Implementation and verification status

Checkpoint 5 is implemented for **one source-built APK**, with two independent
options/buttons. Host tests execute the actual complete modified renderer
translation unit against the production proxy/broker and a **mock native driver**.
They do not prove Mali shader execution or Android display on the phone.
Checkpoints 1–4D remain phone verified; checkpoint 5 awaits the phone procedure below.

### 1–5. Requirements, mappings, null descriptors, fallback, dynamic rendering

The audit tables above record the exact source and all Vulkan 1.2 mappings.
The >=1.2 check is a **version policy**. Timeline/scalar/image-format-list
functionality has Vulkan 1.1 extension equivalents. Float16 is deliberately
unused, using Gamescope's existing FP32 BLIT shader. Robustness2 is **genuinely
absent**; valid resources remove its requirement only in this diagnostic.
Dynamic rendering, present ID/wait, effects, NV12, output color management,
external client timelines, timing queries and desktop export are not reached.
Normal Gamescope retains its original version, extension and feature requirements.

The four native dummy textures are 1x1 RGBA8: sampled 2D, sampled 1D, sampled 3D,
and a separate storage 2D. They use real `CVulkanTexture::BInit`, native memory,
matching views and TRANSFER_DST usage. The actual command buffer clears them to
RGBA=(0,0,0,0), inserts transfer/compute barriers and waits on the native timeline.
They remain alive through renderer teardown. Inactive regular/NV12 slots receive
the 2D view; inactive LUT slots receive correctly shaped 1D/3D views and normalized
real samplers; inactive chroma storage receives the separate storage view.
The RGB-only diagnostic replaces the unused immutable YCbCr sampler with an
ordinary real normalized sampler. All 39 descriptors are valid; native validation
checks dimensions, usage, layout, memory binding, device ownership and UBO range.
No robustness2 extension or feature is synthesized.

### 6–9. Protocol, RPCs, objects and entrypoints

**Protocol v7**, chosen only by `MALI_VULKAN_RENDERER_TEST=1`; versions 1–6 remain
available and cannot be upgraded within a connection. Logical-device creation
adds explicit timeline/scalar/FP16=0 scalars to the existing AHB-service flag.
The proxy and broker independently validate the three required extension names,
actual queried feature bits and native Vulkan >=1.1. The native device receives
individual `TimelineSemaphoreFeaturesKHR -> ScalarBlockLayoutFeaturesEXT`, never
`VkPhysicalDeviceVulkan12Features`. Only 5B enables the existing native AHB service,
which appends Android AHB, foreign-queue-family and external-fence-FD extensions.

| New opcode | Operation |
| --- | --- |
| 60 | Create native timeline semaphore with initial value |
| 61 | Destroy typed renderer object |
| 62 | Query native KHR semaphore counter |
| 63 | Native KHR wait, bounded to five seconds |
| 64 | Create correctly typed image view, optional explicit view usage |
| 65 | Create real nearest/linear, normalized/unnormalized sampler |
| 66 | Create actual seven-binding descriptor set layout |
| 67 | Create pipeline layout, one descriptor set layout |
| 68 | Create descriptor pool, three types, at most 24 sets |
| 69 | Allocate descriptor sets with owned layout/pool IDs |
| 70 | Update the seven actual bindings with explicit typed records |
| 71 | Create shader module from exact SPIR-V bytes |
| 72 | Create BLIT compute pipeline, `main`, seven specialization entries |
| 73 | Bind compute pipeline |
| 74 | Bind descriptor set/pipeline layout |
| 75 | Dispatch compositor workgroups |
| 76 | Native timeline queue submit, optional real producer fence |
| 77 | Reset completed command buffer |
| 78 | Create ordinary 1D/2D/3D RGBA8 texture, optional format list |
| 79 | Record up to 16 real image barriers with tracked layout/ownership |
| 80 | Record packed buffer/image transfer, explicit offset and extent |
| 81 | Clear initialized dummy image |
| 82 | Real device idle, including error/teardown audit |

New native/proxy types: **semaphore, image view, sampler, descriptor set layout,
pipeline layout, descriptor pool, descriptor set, shader module, compute pipeline**.
They use stable typed IDs and strict device ownership; native handles stay in
Android. Command state tracks image layout/foreign ownership, descriptor revision,
pipeline/set compatibility, references and native semaphore signal values.
Destroying a view cannot destroy its image implicitly; stale dependencies cannot
be submitted. Pool and immutable-sampler lifetimes are validated. Native timeline
progress comes only from `vkQueueSubmit` and real KHR counter/wait calls.

The 25 newly supported entrypoints are exactly:

```text
vkCreateSemaphore, vkDestroySemaphore
vkGetSemaphoreCounterValueKHR, vkWaitSemaphoresKHR
vkCreateImageView, vkDestroyImageView
vkCreateSampler, vkDestroySampler
vkCreateDescriptorSetLayout, vkDestroyDescriptorSetLayout
vkCreatePipelineLayout, vkDestroyPipelineLayout
vkCreateDescriptorPool, vkDestroyDescriptorPool
vkAllocateDescriptorSets, vkUpdateDescriptorSets
vkCreateShaderModule, vkDestroyShaderModule
vkCreateComputePipelines, vkDestroyPipeline
vkCmdBindPipeline, vkCmdBindDescriptorSets, vkCmdDispatch
vkResetCommandBuffer, vkDeviceWaitIdle
```

The two unsuffixed timeline aliases resolve to the same real KHR implementations.
Existing image/buffer/memory/barrier/copy/fence entrypoints gain only the narrowly
reached v7 forms. No generic pNext/pointer serialization, push constants, graphics
pipelines, query pools, pipeline-cache objects or unrelated API forwarding.
Unknown pNext is rejected. SPIR-V is word-aligned, size-bounded to **512 KiB**,
validated for header/version and copied as exact bytes, not text. Existing BLIT
compiled here is **62,940 bytes**, default Vulkan1.0/SPIR-V1.0. Total request limit
is 512 KiB +256 bytes; 128 typed renderer objects per device. Session/large request
storage is on the heap: Android NDK worker stack frame is **198,048 bytes**.

Void-operation transport errors retain the first exact VkResult and fail command
completion or teardown. Native errors, timeouts and device loss are logged;
no first-frame PASS is emitted before pixel verification, Android release and
successful renderer teardown. Failure cleanup drains/destroys the native device
and all native children synchronously, then exits nonzero without an unsafe C++
unwind through Gamescope's `-fno-exceptions` recording code.

### 10. Actual Gamescope code exercised and transformed frame

Patch `0118-vulkan-gamescope-renderer-tests.patch` adds early main entries and
includes the diagnostic in the existing renderer TU. It uses:

```text
CVulkanDevice::BInit -> selectPhysDev -> createDevice
  -> createLayouts -> createPools -> createShaders -> createScratchResources
  -> native initialized dummy CVulkanTexture objects -> existing pipeline/compilePipeline
vulkan_create_texture_from_bits -> CVulkanTexture::BInit
  -> CVulkanCmdBuffer::copyBufferToImage -> submitInternal -> KHR timeline wait
FrameInfo_t -> vulkan_composite -> bind_all_layers
  -> existing BlitPushData_t -> CVulkanCmdBuffer::dispatch
  -> real descriptor update/binding -> vkCmdDispatch -> submitInternal
  -> real KHR wait/counter -> SYNC_FD export/wait
  -> same-image GPU readback + optional exact AHB CPU lock
  -> Android exact-buffer acquisition, display and previous-buffer release
```

The source is full 256x256 RED/GREEN over BLUE/WHITE. Gamescope's existing layer
controls use scale=(2,2), offset=(-64,-64), NEAREST, opaque layer and black border.
The existing BLIT shader writes a **centered 128x128** quadrant square with a
**64-pixel opaque BLACK border**, via 32x32 workgroups of 8x8. The existing constants
apply their normal +0.25 texel-center offset; no custom shader or copied compositor.

```text
+---------------------+
|        BLACK        |
|    RED  | GREEN     |
|    -----+------     |
|    BLUE | WHITE     |
|        BLACK        |
+---------------------+
```

The output is the imported 256x256 native RGBA8 AHB itself, wrapped by a Gamescope
texture with a real UNORM storage view. Compute writes it directly. A same-image
GPU readback is appended before FOREIGN ownership release, on the producer
submission; it does not supply presentation pixels. A native exportable binary
fence on that submission hands real SYNC_FD completion to Android, while Gamescope's
internal sequence uses the real KHR timeline. Android consumes that exact AHB,
retains it visibly for three seconds and releases it before Vulkan teardown.

All **65,536 pixels** must match, with **49,152 source-vs-final differing pixels**.
Printed samples: (96,96) RED=(255,0,0,255), (160,96) GREEN=(0,255,0,255),
(96,160) BLUE=(0,0,255,255), (160,160) WHITE=(255,255,255,255),
(128,128) WHITE and (16,16) BLACK=(0,0,0,255). AHB CPU inspection additionally checks
actual bytes if lockable; GPU-only AHB still requires actual same-image readback.

### 11. Validation

- All Mali suites: **62 tests PASS** (including 13 new renderer tests).
- Existing Gamescope diagnostics/complete patch stack: **25 tests PASS**;
  added patch passes `git apply --check --whitespace=error`.
- AArch64 QEMU native broker/consumer with host proxy and actual Gamescope renderer:
  **54 tests PASS**. This executes AArch64 production broker code against mocks.
- Repository suite: **297 tests**, **one skip**, passes from a temporary path without
  spaces. In this workspace, two unchanged launcher tests fail because they invoke
  an unquoted path containing `Desktop/ Project stuff`; checkpoint 5 does not change
  either launcher. Those two failures are reported, not counted as PASS here.
- Android NDK r27d API26 broker+consumer shared library: AArch64, `-Wall -Wextra
  -Werror`, `-Wl,-z,defs`, 16 KiB ELF LOAD alignment, PASS.
- AArch64 glibc production ICD, loader diagnostic, inventory and static probe:
  built with GCC13.3, PASS. Entire modified Gamescope renderer TU cross-compiles
  to an AArch64 object; actual complete renderer also compiles/links in host fixture.
- GitHub workflow: actionlint, shell syntax and embedded Python syntax checks.
- `git diff --check`: PASS. Full patch replay against HEAD and status captured
  with the review artifacts after final validation.

Host tests cover feature/extension omissions, unchanged API, real KHR aliases,
unknown pNext/Vulkan12/robustness rejection, typed/wrong-parent/stale IDs,
shader bounds and exact-byte hash, immutable sampler lifetime, descriptor types,
actual compositor order, timeline submit/counter/wait/timeout, native errors/device
loss, missing AHB storage support, pixel corruption, GPU-only AHB, SYNC_FD/consumer
failure, teardown failure and two frame runs on one live broker with balanced
objects. The host BLIT model is explicitly test-only; production has no CPU shader,
CPU timeline, output pixel fill or copied presentation buffer.

Reproduce locally (pristine source and pinned wlroots headers required):

```sh
VULKAN_HEADERS=/path/to/Vulkan-Headers/include JAVA_HOME=/path/to/jdk \
GAMESCOPE_SOURCE=/path/to/pristine/gamescope-3.16.29 \
WLR_HEADERS=/path/to/pinned/wlroots/include \
python3 -m unittest discover -s tools/mali-vulkan -p 'test_*.py'
GAMESCOPE_SOURCE=/path/to/pristine/gamescope-3.16.29 \
VULKAN_HEADERS=/path/to/Vulkan-Headers/include JAVA_HOME=/path/to/jdk \
python3 tools/gamescope/test_vk_enumerate_only.py
```

Host prerequisites: C/C++20 compilers, Vulkan loader/headers, JNI headers,
GLM, libdrm, pixman, X11, xkbcommon and Wayland headers, glslangValidator and
wayland-scanner. The renderer suite generates shaders and protocol headers,
applies all repository patches with zero fuzz and compiles the actual renderer.

### 12. Remaining hardware acceptance

**No checkpoint-5 real-phone PASS is claimed.** Required descriptor/compute limits (including 36 per-stage samplers/images)
are checked explicitly; an unmet limit stops 5A with `VK_ERROR_FEATURE_NOT_PRESENT (-8)`.
The other new requirement is RGBA8 AHB **STORAGE_IMAGE** support for this exact allocation/import usage.
The broker queries real image-format/AHB format features before creating the
storage target. If unavailable, 5B reports `VK_ERROR_FORMAT_NOT_SUPPORTED (-11)`
and stops; 5A remains independent. Other actual native shader/pipeline/descriptor
or synchronization failures remain visible with exact VkResult. Do not proceed
to a normal session on the strength of host mocks.

A full packaged Gamescope executable and signed APK were not built locally;
GitHub Actions below builds both from the same commit. Local Gradle/JUnit checks are not verified: this environment has JDK27, whereas
the workflow configures JDK17. Host link tests and the AArch64 renderer compile supplement, rather than replace, that full package build.

### 13–14. Full diff and git status

The working tree contains the implementation and audit on `mali-gpu-experiment`.
**No commit and no push were made.** Review artifacts outside the repository:
`/tmp/droiddeck-5/checkpoint-5.diff` (tracked and new files),
`/tmp/droiddeck-5/git-status.txt`, `/tmp/droiddeck-5/diff-stat.txt`, and validation
logs in `/tmp/droiddeck-5/`. The full diff can be replayed against baseline
`798ae0f1c9645f036678095c3d5b776a825cf03d`.

### 15. One-build GitHub Actions procedure

After reviewing, commit all tracked changes **and all new files**, including patch
0118, then push `mali-gpu-experiment` yourself. One push triggers **Build APK**.
Alternatively dispatch that workflow once on the same branch after publishing the
commit; avoid a simultaneous extra dispatch if the push build is already running.
The Gamescope job builds the full 3.16.29 executable with every patch, checks both
new options and all prior options, and exercises missing-ICD early failures.
The APK job downloads that exact run's Gamescope artifact, stages it, and checks
all ten option markers inside the APK. Download/install one APK from that run;
both buttons are present and no rebuild is needed between 5A and 5B.

Update/refresh the installed Linux runtime from the new APK so the guest's
`/usr/local/bin/gamescope` is the new binary. Merely replacing the APK while keeping
an old installed runtime can leave the old Gamescope in the guest. Existing app
runtime update controls perform that overlay refresh.

### 16. Exact phone procedure and acceptance

Keep the diagnostic activity's SurfaceView live and inspect its preview. In order:

1. Start broker; run **probe** (checkpoint 1).
2. Run **ICD** (checkpoint 2).
3. Run **Gamescope Vulkan enumeration test** (checkpoint 3).
4. Run **Gamescope Vulkan capability test** (4A).
5. Run **Gamescope Vulkan device test** (4B).
6. Run **Gamescope Vulkan submit test** (4C).
7. Run the existing **AHardwareBuffer presentation test** (4D4).
8. Run **Gamescope renderer init test** (5A).
9. Run **Gamescope first frame test** (5B).
10. Run **Gamescope first frame test** again, using the same APK and broker.

5A must print actual Mali-G52/API1.1.131, required extensions enabled, FP16 disabled,
robustness2 absent/not advertised, native initialized dummies, descriptor/shader/
compositor pipeline creation, renderer initialized, no backend started, clean
teardown and exit code 0. It does not allocate/present an AHB.

Each 5B run must show the **centered RED GREEN / BLUE WHITE square with black border**
for about three seconds. Confirm that arrangement visually. Logs must show the
actual `vulkan_composite -> BLIT -> CVulkanCmdBuffer::dispatch` path, native KHR
sequence/counter completion, selected exact RGBA values, **mismatches=0** and
**differing pixels=49152**, exact AHB Android acquisition/completion/release,
clean teardown, first-frame PASS and exit code 0. A -1 SYNC_FD can mean an already
signaled real fence, as in 4D; it is not a fabricated fd. GPU-only AHB may report
CPU inspection checked=0, but actual final-image GPU readback must pass.

Keep UI/proot and MaliVulkanBroker logs for both runs. Any nonzero exit, missing
release, incorrect pixels or wrong visible arrangement is a failed acceptance.
Checkpoint 5 ends with that verified frame; no normal session, client, Steam,
Proton, DXVK or game is launched by these diagnostics.
