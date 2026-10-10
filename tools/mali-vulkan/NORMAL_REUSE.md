# CP7P: completed timeline knowledge and persistent mapped staging

## Phone-validated input

The attribution build measured 47 in-frame RPCs/frame and four renderer waits:

| Reason | Native average | Steady observation |
| --- | --- | --- |
| SHM staging destruction | 1.4–1.9 ms | Almost always materially blocked |
| Descriptor reuse | 0.008–0.01 ms | Already satisfied |
| Output producer completion | 5.9–6.1 ms | Materially blocked |
| Output command retirement | ~0.01 ms | Already satisfied; same render value |

The subsequent A+B pass is now phone-validated: exactly 38 in-frame RPCs/frame,
descriptor and retirement waits locally completed, one forwarded output wait,
two staging slots with 100% warmed hits and no reuse waits, creation, resizing
or failures. Healthy windows were 25–27 FPS (peak 27.4), zero-copy, no pool drops.
The remaining output wait measured ~6.0–6.4 ms native. Mapped reads cost roughly
4.98–5.51 ms/frame RTT and writes 3.77–4.23 ms/frame in the supplied samples.
Persistent mappings are the current change and still need matching CI assets
and phone validation. No further FPS improvement is claimed.

## A. Trusted completed value

`mali_wait_profile.hpp` keeps render-thread-local completion knowledge keyed by
the exact VkDevice and VkSemaphore. Only a successful real wait or the successful
real counter query in `CVulkanDevice::completedSeqNo()` advances it. Values take
the maximum within that identity; switching identity discards older knowledge.
Submission, timing, FD export and Android ownership do not advance it. Session
initialization and shutdown reset it, including when numeric handles are reused.
Two-second statistics resets do not clear completion knowledge.

Descriptor reuse can return locally only for a covered value of the same
semaphore/device. Output command retirement additionally requires a recorded
successful `output-producer-completion` wait on that exact semaphore/value.
Entering a new producer-completion wait invalidates that proof; timeout/error
does not reinstate it. Counter knowledge alone cannot replace this producer proof.
Output producer completion itself always performs the real broker wait.
Unrecognized, multi-semaphore and non-normal waits follow the original path.

The local return skips only the redundant wait RPC. `CVulkanDevice::wait()`
still runs its upload-offset handling and `resetCmdBuffers()` when requested;
command resets and reference retirement remain real operations. Broker state
has already been completed by the proving wait/counter handler. No semaphore
value is modified and no Android buffer is returned to FREE by this cache.

## B. Staging pool and deferred retirement

`mali_staging_pool.hpp` implements the bounded policy, and `mali_staging.inc`
uses the existing Vulkan device/functions. It lazily warms two buffer/allocation
pairs, rotating through compatible completed slots. It can grow to three only
when no completed slot can serve the request. The slot count is bounded; range
and allocation limits remain the existing broker limits, with no new small cap.
Each slot tracks capacity, reservation and its real last upload sequence `U`.
The backend explicitly checks the scratch semaphore/device cache identity.

Before reusing or resizing a submitted slot, `U` must be covered by trusted
completion knowledge. At capacity, the oldest unreserved slot gets the original
real five-second timeline wait. Timeout/device loss returns failure, with no
reuse, destruction or successful release of that pending slot. A canceled
unsubmitted lease becomes available without inventing a new producer sequence.
Creation failures safely destroy only their unsubmitted partial resources.

The initial A+B importer retained the staging VkBuffer, VkDeviceMemory and
binding, but still mapped/unmapped every import. The current persistent-mapping
change below extends that lifetime to the mapping itself. Each import still
copies exactly stride × height bytes and performs the same channel conversion.
A retained larger-capacity slot can transfer more padding than a smaller new
allocation; logical pixel bytes and copy extent remain identical. Stride × height
is checked for overflow before acquisition and against the mapped allocation
before copying. There is no dirty-range or partial-image optimization.

Removing the host wait also requires preserving GPU visibility. The upload
command's **existing** final image barrier now includes shader-read destination
access in normal pooled imports, with its existing transfer-write source access,
GENERAL layout and ALL_COMMANDS scopes. It introduces no additional command RPC.
The render submission follows on the same actual queue. This is an actual GPU
memory dependency across submissions, not a claim that `U` has already completed.
See the [Vulkan barrier scopes](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPipelineBarrier.html).

The broker previously refused every overlapping pending reference. The narrow
normal-only exception in `renderer_upload_dependency.h` permits a sampled source
image whose sole pending predecessor is one copy-only, non-AHB upload with that
recorded visibility barrier. The consumer must use binding 3, the same queue ID,
the same timeline semaphore and a strictly later signal value. Storage writes,
other pending references, another queue/semaphore, multi-copy producers and AHBs
remain rejected. Transfer/clear commands in a consumer also disqualify it.
Descriptor updates likewise allow only these descriptor-free upload predecessors
to remain pending; updates to any still-recorded/in-flight descriptor set and
updates while arbitrary render work is pending remain rejected. Non-normal
updates retain their original global no-pending policy.
Recording sees the predecessor's planned final layout separately;
completed image layout and command state are updated only by real completion.
The host mapped-memory/destruction guards still see the pending upload. Existing
diagnostic/legacy submit paths retain their no-overlap checks.

Upload and render command buffers stay alive until completion and are reset by
the existing output retirement. Steady state reuses two command buffers instead
of reusing one between the old host waits. Before buffer destruction, the backend
also retires completed command references to satisfy the existing broker guards.

Stopping sets the normal producer's stopping flag, drains/cleans staging before
disabling normal wait tracking, then runs the existing output/Android teardown.
Shutdown cleans every safe slot; a failed wait retains the pending slot and
reports failure. Existing device failure cleanup handles remaining resources
after its safe drain. AHB release acknowledgment is independent and unchanged.

## Persistent coherent staging mappings (current change)

The confirmed old flow was `vulkan_create_texture_from_wlr_buffer()` →
`vkMapMemory(..., VK_WHOLE_SIZE)` → `proxy_MapMemory()` → acknowledged native
map and full coherent mirror download (`MB_MEMORY_READ`, two messages for the
230400-byte SHM image) → full CPU copy/channel conversion → `proxy_UnmapMemory()`
→ coherent bulk upload → real native unmap. This happened once per import,
despite reuse of the buffer/allocation pair.

The backend now maps offset zero and the whole allocation once after binding.
The slot resource stores `mapped` and `mappedBytes` (the memory requirements
size). After the existing completion/reservation checks, the importer copies
the full SHM image through that pointer; there is no per-frame map or unmap.
Normal-only bounds checks retain both the logical capacity and allocation range.
The private `vkDroidDeckMapStagingMALI` entrypoint maps this known upload-only
allocation without an initial coherent download. It zeroes the local mirror,
registers the whole mapping for scoped uploads, and returns its persistent pointer.
The caller overwrites the complete SHM image before arming any upload. Ordinary
`vkMapMemory` still performs its original coherent download, including diagnostics.
Diagnostics and non-pooled SHM imports retain their original map/unmap path.

**Submit scoping is necessary:** the old proxy queue path uploaded every live
coherent mirror before every submission. Simply retaining mappings would make
render `R` re-upload staging still read by upload `U`; the unchanged broker
pending-memory guard correctly rejects this. It would also upload idle slots.

The private, local `vkDroidDeckStagingMALI` contract arms only these
whole-allocation coherent mappings for pool-managed uploads. Registration
requires normal mode, wire v7, a live owned mapping and the complete valid range.
After copying, the importer arms the real upload command. Its QueueSubmit uses
the existing bounded `interop_copy_mapping()` bulk uploads and must receive all
ACKs before sending `MB_RENDERER_SUBMIT`. Success consumes the arm; failure
retains it and propagates failure without pretending submission succeeded.
Unrelated submits skip those managed slots. Ordinary coherent mappings retain
the existing upload-before-every-submit behavior. This is neither a global
`vkMapMemory` change nor automatic dirty tracking, and introduces no wire opcode,
version bump, advertised Vulkan extension or extra RPC.

CPU writes still require the slot's real last upload completion. Resizing and
shutdown prove completion, retire recorded references, unmap exactly once,
destroy/free and clear the pointer/range. Replacements receive a fresh mapping.
Failed drains retain their in-flight mappings/resources; safe slots can still
be cleaned. Map/registration failures clean only unsubmitted partial resources.
Successful unmap clears the proxy's opt-in and armed-command metadata. Missing
matching staging API fails before creating a slot; it does not fall back to
unsafe all-mapping uploads. Session initialization resolves the API anew.

### First-frame acquisition failure and dispatch fix

The first persistent-mapping phone run failed before any staging creation RPC.
`MaliStagingBackend::create()` returned `VK_ERROR_FEATURE_NOT_PRESENT` because
device lookup of `vkDroidDeckStagingMALI` returned null. The ICD's instance lookup
listed it, but `proxy_GetDeviceProcAddr()` omitted it. Gamescope obtains private
device functions through `g_device.vk.GetDeviceProcAddr`, the same path as the
already working performance/Wayland entrypoints. Matching headers and direct
function tests did not exercise that table.

Both private staging functions now appear in both ICD lookup tables. Their
signatures are distinct; the existing arm/register signature is unchanged.
No wire-v7 ABI or broker opcode changed. Normal startup resolves both and probes
wire-v7/normal-mode availability before `MB_NORMAL_BEGIN`, reporting
`persistent staging API: available` on success. Missing names, wrong wire version,
disabled normal mode, incompatible allocation requirements and private mapping
errors have specific diagnostics. Missing map support in an older staged ICD
fails explicitly; it never falls back to per-frame mapping or unsafe uploads.
There is no evidence from the supplied phone log of a different staged artifact;
the source dispatch omission alone explains the observed local failure.

## Counters and expected steady RPCs

The existing two-second opcode/category/RTT/FPS reports remain. New local rows:

```text
MaliPerf local-wait: wait-reason=descriptor-set-reuse requests=... locally-completed=... forwarded=... requests/frame=1.00 forwarded/frame=0.00
MaliPerf local-wait: wait-reason=output-command-retirement requests=... locally-completed=... forwarded=... requests/frame=1.00 forwarded/frame=0.00
MaliPerf staging: hits=... misses=... reuse-waits=... resources-created=... resized=... failures=... live=2 limit=3
MaliPerf staging-map: phase=window persistent-maps-created=0 persistent-map-reuse-hits=53 unmaps=0 map-failures=0 mapped-slots-live=2 (window totals)
```

Map counters reset with the two-second window; live mapped slots are a gauge.
Creation/resize windows can contain maps, but no staging reads. A final `phase=teardown`
row reports unmaps and remaining mapped slots, including a failed drain's retained
slots. A clean shutdown has zero mapped slots. Existing opcode, RTT, ownership,
wait-reason and pool reports remain intact.

Reason ID 1 keeps its wire value and snapshot size but is now named
`shm-staging-slot-reuse`. It measures fallback waits, rather than immediate
destruction. A warmed compatible pool should have hits, no new resources and
no reuse waits. RPC wait rows count only real calls; local rows expose skipped
waits separately. Native wait timing retains its prior measurement definitions.

| Category | Attribution baseline | Phone A+B | Persistent mapping expected |
| --- | ---: | ---: | ---: |
| Mapped transfer | 7 | 7 | 3 |
| Recording | 14 | 14 | 14 |
| Resource management | 12 | 7 | 7 |
| Timeline/synchronization | 8 | 5 | 5 |
| Descriptor/resource updates | 3 | 2 | 2 |
| Queue submit | 2 | 2 | 2 |
| AHB presentation | 1 | 1 | 1 |
| Total in-frame RPCs | **47** | **38** | **34** |

A alone predicted 45/frame. B additionally removed the upload wait, five staging
management calls and one buffer bind; the phone confirmed 38/frame. Persistent
mapping targets two reads, one map and one unmap removed per steady frame:
**38 → 34/frame**, with the existing three bulk write messages/frame retained
for the measured sizes. Four source-image destruction calls outside the frame
snapshot still exist, plus
periodic STATS. Variable sizes, cold command allocation or slot pressure add work.
Expected renderer wait RPCs are one/frame (output producer completion), with
staging fallback waits only under pressure. The real producer SYNC_FD wait and
publication's completed-sync check remain. The GPU still performs the upload;
its cost may move into the output wait rather than disappear. Measure FPS.

## C. Async output producer fence: audit only

**Export can represent pending `R`:** `MB_RENDERER_SUBMIT` attaches an exportable
fence to render `R`; after successful QueueSubmit, `MB_SYNC_EXPORT` requires
submitted/exportable/not-yet-exported, but does not require completed. It calls
real `vkGetFenceFdKHR(SYNC_FD)` without a preceding wait in that handler.
Vulkan permits exporting copy-transfer fence payloads with a pending signal
operation: [VkFenceGetFdInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkFenceGetFdInfoKHR.html).
Actual pending-export behavior/latency on this Mali driver needs a future test.

**Today's export is already satisfied:** the normal runner waits for `R` first.
Thus export represents an already-completed producer; the driver may return a
signaled FD or -1. SYNC_FD export has copy-transference/reset effects, so the
original fence's later status must not substitute for the exported payload's
completion. See [vkGetFenceFdKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetFenceFdKHR.html).
The runner then issues `DD_SYNC_WAIT`, and `MB_NORMAL_PUBLISH` calls
`interop_wait_sync()` again before publication and checking completed foreign
image state. Simply deleting the first wait would leave CPU blocking there.

**Android does not receive this producer FD in the current normal Mali path:**
`mb_normal_publish()` stores sync/fence IDs, not an acquire FD.
`ahb_create_broker_buffer()` creates an AHB-only buffer through
`droiddeck_create_ahb_buffer()` with no DMA-BUF planes/FD. Therefore
`ahb_swapchain_present()` cannot export an implicit DMA-BUF acquire fence for
these buffers and passes -1 to `sc_layer_present_ahb()`.

The shared `sc_layer_present_ahb()` implementation does pass a supplied valid
acquire FD to `ASurfaceTransaction_setBuffer()` and transfers ownership. Android
can wait asynchronously on it, but the normal broker-backed Mali path currently
supplies none and relies on the CPU completion gates. This distinction is
confirmed by the [SurfaceControl contract](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/master/include/android/surface_control.h).

Future pending publication would need an owned/duplicated producer FD handed
atomically to the exact Android buffer transaction, with cancellation/duplicate
commit handling and exactly-once FD closure. It would also need:

- Command buffers, source textures, uniform ranges and descriptor sets retained
  until real `R` completion; no reset, overwrite, update or destruction while
  their GPU uses remain pending. The current single upload offset and renderer
  guards would need to support more than one outstanding render safely.
- Output AHB/image/allocation retained through GPU completion **and** Android's
  real release acknowledgment. Rendering into an Android-owned slot remains
  prohibited, regardless of completed timeline values.
- Producer fence and sync identity retained through publication and the broker's
  real completion/retirement checks. Existing fence destruction rejects pending
  submissions, and SYNC_CLOSE requires completed; no early fence recycling.
  A transferred duplicate FD has its own lifetime owned by Android.
- A separate queued foreign-ownership/layout model for pending AHB producers;
  the new staging exception deliberately excludes AHBs. Never label pending
  render work completed merely to pass publication validation.

No output wait, SYNC_FD, publication gate, Android acquire-FD handoff or AHB
ownership code was changed for this audit.

## Cheap validation

```sh
cd tools/mali-vulkan
python3 -B -m unittest -v test_staging test_wait test_perf test_bulk test_normal.NormalOwnershipTests.test_android_owned_release_timeout_and_restart
```

Only standalone mocked helper programs compile. Cases cover cache identity,
monotonicity, timeout/device loss, strict producer proof, legacy fallback,
restart/reset, pool warm-up/rotation/bounds/resize, byte preservation, allocation
failures, lease cancellation, no unsafe reuse/destruction after failed waits,
partial safe shutdown, real backend cleanup, queued layout vs completion,
required barrier scopes, same-queue/semaphore/read-only guards, retained host
memory/AHB guards, profiler/header parity and existing bulk/ownership behavior.
Persistent cases also run the actual proxy map/unmap, upload and submit helpers:
40 U/R frames retain one mapping with zero initial/steady staging reads and no steady map,
unmap or read calls, exact full-frame bytes and no pending staging re-upload.
Other cases cover map/registration failure, pointer/range cleanup on resize,
partial failed drains retaining live mappings, missing private API, failed ACKs
preventing submit and unchanged automatic uploads for ordinary coherent memory.
The bulk harness resolves both private functions through the actual ICD device
lookup and invokes the capability probe/mapping. It checks v6 and disabled-mode
rejection without broker work, invalid memory/mapping guards, and ordinary maps
still downloading two initial chunks with exact bytes. Backend tests cover both
missing device names, rejected availability probes and specific error messages.
These are small extracted-source harnesses, not an Android Vulkan loader run.
CI must still compile actual Gamescope/proxy/broker, followed by normal Launch,
variable-size, stop/restart and zero-copy/release checks on the phone.
