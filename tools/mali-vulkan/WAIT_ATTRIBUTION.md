# CP7P: normal Mali waits and transient resource lifetimes

This document records the attribution baseline, now phone-validated: 47
in-frame RPCs/frame, four renderer waits and 12 resource-management calls.
Native averages were 1.4–1.9 ms for SHM staging cleanup, 5.9–6.1 ms for output
producer completion, 0.008–0.01 ms for descriptor reuse and ~0.01 ms for output
retirement. The latter two were already satisfied in every steady sample;
the first two materially blocked. Current optimizations and the async-fence
audit are described in [NORMAL_REUSE.md](NORMAL_REUSE.md). Tables below describe
the pre-optimization lifetimes and call counts, rather than current targets.

The source audit uses Gamescope 3.16.29 `rendervulkan.cpp`/`rendervulkan.hpp`
with this repository's patches applied in order. The renderer hunks apply
without compiling Gamescope. The normal runner and presentation functions
are in `tools/gamescope/patches/0121-mali-normal-wayland-session.patch`.
Upstream sources:
[renderer](https://raw.githubusercontent.com/ValveSoftware/gamescope/3.16.29/src/rendervulkan.cpp),
[types and descriptor ring](https://raw.githubusercontent.com/ValveSoftware/gamescope/3.16.29/src/rendervulkan.hpp).

## Four steady-state wait sites

All four wait on the same real scratch KHR timeline, through opcode 63.
The original semaphore, requested value, flags, five-second timeout, result
handling and command retirement logic are unchanged. Let `U` be this frame's
SHM upload submission and `R` its compositor submission. Normally `R = U + 1`.
Absolute values depend on initialization; the profiler reports actual values.

| Stable reason | Exact call site and requested value | Protected resource / producer | Consumer or reuse requiring completion |
| --- | --- | --- | --- |
| `shm-upload-staging-destroy` | `vulkan_create_texture_from_wlr_buffer()`: `g_device.wait(sequence)`, immediately after `g_device.submit(copyCmd)`; target `U` | Temporary transfer-source VkBuffer and coherent VkDeviceMemory; upload command copies SHM pixels into the newly created source image | Immediately resets/retires upload commands, destroys staging buffer, frees staging memory. Those actions require completion with the present lifetime. The subsequent compositor submission is ordered on the same queue; freeing the upload storage is the immediate host-wait dependency. |
| `descriptor-set-reuse` | `CVulkanDevice::descriptorSet()`: existing `WaitSemaphores` inside the nonzero `m_descriptorSetSeqNos[uIndex]` branch; target `D` from that slot | Persistent descriptor set, last read by the compositor submission stamped into the rotating slot | `CVulkanCmdBuffer::dispatch()` updates that set after `descriptorSet()` returns. An in-flight set must not be overwritten. The ring has 24 sets; with one dispatch and two submissions/frame, steady `D` is the compositor value 24 frames earlier, normally `R - 48`. Initially unused slots do not wait. |
| `output-producer-completion` | `vulkan_mali_normal_present()`: `g_device.wait(*sequence, false)` after `vulkan_composite()`; target `R` | Compositor command, its sampled source texture and rendered output AHB image; producer fence attached to that submission | Existing completed-counter proof, producer SYNC_FD export/wait, Ready transition and publication. Completion is required by the current host validation here. A later real SYNC_FD wait also protects publication; changing that sequence would require a separate synchronization review. |
| `output-command-retirement` | Same function: `g_device.wait(*sequence, true)` after `mali_normal_commit(index)`; target **the same `R`** | The same completed compositor command and its retained texture/descriptor references | Resets command buffers and releases references, then closes the sync token and destroys the producer fence. The earlier timeline wait and real producer SYNC_FD wait have already succeeded. This is a completion/retirement serialization artifact by source ordering, rather than a new GPU dependency. Retirement itself remains necessary. |

The SHM staging and output producer sites can encounter unfinished GPU work.
The descriptor site should normally find its older producer already complete
in this serialized loop. The retirement site's target must already be complete
after the earlier successful wait. These are source-based expectations; native
timings and counter observations must confirm their phone costs. Even an
already-satisfied wait may take significant driver CPU or scheduling time.

The four named rows should each report `per-frame=1.00` after descriptor-ring
warm-up. Additional unexpected device waits are attributed to
`unattributed-device-wait`; aggregate coverage compares all opcode-63 calls
against reason counts. The existing profiling frame boundary is unchanged.

## What the new two-second report measures

Each active reason prints:

- Frame/outside counts and calls/frame; client complete synchronous RTT
  total/average/maximum, using the existing RPC timer.
- Native `WaitSemaphoresKHR` wall total/average/maximum and thread CPU total.
  Native wall excludes request transport, reply transport, and the counter query.
- `native-off-CPU-est = max(native-wall - native-thread-CPU, 0)` summed over
  samples. This includes scheduling/preemption as well as blocking. Neither
  it nor native wall is a GPU timestamp or an exact measurement of pure GPU
  blocked duration. No GPU-query commands or extra wait/poll loops are added.
- One native counter snapshot immediately before the real wait, with query
  duration reported separately. Query failure never bypasses or changes the wait.
- Counts: `already-satisfied` means counter-before >= target;
  `briefly-blocked` means counter-before < target and native wall < 1 ms;
  `materially-blocked` means counter-before < target and native wall >= 1 ms.
  These latter names are operational classifications, not proof the entire
  wall interval was blocked: GPU completion can race the snapshot. Failed
  counter queries or missing native samples are `unknown`. `failed` separately
  counts non-success wait results, including timeout.
- The latest observed target for that reason, its counter-before/result,
  semaphore broker ID, latest submitted value, reason detail, and historical
  producer command/image/buffer/memory/descriptor-set/fence broker IDs.
  Detail is the SHM staging buffer ID, descriptor-ring index, or output-pool
  index, as appropriate. IDs are typed broker IDs, not native handles/pointers.

Producer metadata is recorded only after a successful submission. A bounded
64-submission history survives command reset and covers the normal descriptor
ring's 48-submission reuse distance. `producer-found=0` explicitly reports
missing/evicted history; the semaphore and target are still reported. The buffer
is the first referenced buffer (staging for SHM, typically uniforms for render),
not a complete list of all submission resources. `image` is the output image or
first copied renderer image. History IDs can identify resources already retired
by the time of a later descriptor wait; they are attribution, never permissions
to reuse them. Each report retains one representative last tuple per reason,
rather than logging every frame/value separately.

No new RPC or opcode is added. A normal-only private wait entrypoint uses
opcode 63 with the original 24-byte request plus `reason:u32, detail:u32`.
The optional 72-byte reply sample contains five explicit little-endian u64
fields and eight u32 fields, defined by `DD_WAIT_FIELDS` in `normal_wait_perf.h`.
All request sizes, reason IDs, semaphore IDs and SHM buffer IDs are validated.
The proxy validates exact reply size/count before using metadata. Vulkan timeout
is returned unchanged with a sample; negative Vulkan errors retain the existing
prefix-only error reply and are counted as failures with missing native data.
The old 24-byte wait request and counter RPC are unchanged, including diagnostics.

The extension applies only to wire-v7 normal sessions. Use matching proxy,
broker and Gamescope assets: older brokers reject the optional request rather
than invent completion. Older proxies fall back to the original real wait and
show missing attribution. Local snapshots use `vkDroidDeckPerformance2MALI`;
the original snapshot entrypoint keeps its original buffer size for old callers.
Neither private entrypoint is advertised as a supported Vulkan extension.

Per-wait overhead is fixed-size metadata, clocks, one native counter query and
a bounded producer-history lookup, with no per-wait allocation or logging.
There are four extra native counter observations/frame, **zero extra RPCs**.
Counter queries can perturb timings; their time is explicit. Formatting occurs
only at the existing two-second aggregate boundary. Existing opcode/category,
bulk-transfer, broker service/CPU, FPS, AHB and release counters remain active.

## The 12 in-frame management RPCs and four outside-frame destructions

The counts below are for successful steady frames on the existing SHM path.
The new typed report counts management RPC attempts (and successful typed
`MB_RENDERER_DESTROY` calls), not live objects or allocation byte totals.
Memory rows combine staging and source-image allocations. Non-steady failures,
initialization or other resource types appear in residual management coverage.

| Resource | In-frame management RPCs | Outside-frame management RPCs | Actual lifetime |
| --- | --- | --- | --- |
| SHM staging VkBuffer + memory | Create buffer 32, requirements 34, allocate memory 35, destroy buffer 33, free memory 36: **5** | 0 | `vulkan_create_texture_from_wlr_buffer()`, from CPU pixel copy to completion of upload `U`; responsible for the staging-destroy wait. Logical bytes are stride × height (230400 for 320×180 RGBA); native allocation size comes from requirements. |
| Source VkImage + memory + linear/sRGB views | Create image 78, requirements 46, allocate memory 35, two create-view 64 calls: **5** | Two destroy-view 61/kind 2, destroy image 45, free memory 36: **4** | `CVulkanTexture::BInit()` through compositor use `R`, command retirement and final local references dropping. Extent/format/usage describe the SHM content, not inherently a one-frame allocation. |
| Exportable producer VkFence | Create 54 and destroy 28: **2** | 0 | Created before compositor submission `R`; exported to real SYNC_FD, waited, published, then destroyed after command retirement. |
| Total | **12/frame** | **4/frame** | Does not include map/read/write/unmap, binding, recording or synchronization categories. |

`source = nullptr` happens before `vulkan_mali_normal_frame_end()`, but the local
`FrameInfo_t` still retains `layers[].tex` (`gamescope::Rc<CVulkanTexture>`).
Its scope ends **after** frame-end accounting. This explains four destruction
RPCs outside frame snapshots, in addition to periodic STATS: 47 in-frame can
coexist with approximately 51 total/frame. The instrumentation preserves this
scope/lifetime and accounts for both phases rather than moving work between them.

The three output AHBs, descriptors/pool, shader/pipeline/samplers, command pool,
reused command buffers and persistent uniform upload buffer are not recreated
each steady frame. Producer sync-token/FD operations belong to synchronization
or presentation accounting, not the 12 management RPCs above.

## Reuse opportunities identified by the attribution pass

- **Staging buffer/memory:** pool by required size, usage and memory type;
  retain each entry until its real upload completion. Do not overwrite pending
  bytes. With deferred retirement and correct broker pending-state tracking,
  the immediate staging-destroy host wait could potentially disappear because
  the buffer would no longer be freed at that point. Reuse still requires real
  completion; a pool alone does not authorize skipping synchronization.
- **Source image/memory/views:** retain compatible extent/format/usage entries
  across frames; retire sampling commands and all descriptor references before
  rewriting/rebinding. This can remove image/view allocation churn. It does not
  automatically remove the staging or output-completion waits. Layout/discard
  state must be handled correctly if an image is reused.
- **Producer fences:** potentially reusable only with a separately validated
  fence-reset and SYNC_FD-export lifetime. Preserve real exported FD ownership,
  closure and Android release state. This is a higher-risk follow-up.
- **Descriptor wait / output command retirement:** completion is already known
  by source ordering in steady frames. Future explicit completion/retirement
  tracking might avoid redundant synchronous calls while retaining all resets,
  resource-reference retirement and ownership checks. Pooling source images
  alone is not a sufficient justification to remove them.

The attribution pass itself did not implement pooling or wait short-circuits.
The subsequent [reuse pass](NORMAL_REUSE.md) implements staging pooling and
proven-completion short-circuits. Timeline completion is never Android AHB
release. AHB ownership, SYNC_FD and release acknowledgments remain unchanged.

## Historical attribution validation

Cheap targeted checks compile only tiny fake-clock/Vulkan/transport helper
harnesses, not the ICD, broker, renderer or dependencies:

```sh
cd tools/mali-vulkan
python3 -B -m unittest -v test_wait test_perf test_bulk test_normal.NormalOwnershipTests.test_android_owned_release_timeout_and_restart
```

They cover unchanged wait arguments/real execution even when already satisfied,
counter-query failure, timeout/device-loss propagation, malformed replies,
normal-only metadata, legacy request/snapshot compatibility, historical metadata,
scope restoration, frame/outside accounting, header/patch parity, bulk-transfer
ordering and Android ownership. Renderer patch application is also checked as
text against the pinned upstream source. Full build/integration remains for CI.

The attribution phone-validation run collected steady two-second windows after
24-frame descriptor warm-up: four named waits/frame, 47 frame RPCs/frame, two
bulk reads/frame and intact zero-copy/releases. Its per-reason results are
recorded above. Reuse validation now follows the new expected counts and
targeted tests in [NORMAL_REUSE.md](NORMAL_REUSE.md).
