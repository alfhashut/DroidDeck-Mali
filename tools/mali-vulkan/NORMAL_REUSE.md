# CP7P: completed timeline knowledge, retained staging, async-fence audit

## Phone-validated input

The attribution build measured 47 in-frame RPCs/frame and four renderer waits:

| Reason | Native average | Steady observation |
| --- | --- | --- |
| SHM staging destruction | 1.4–1.9 ms | Almost always materially blocked |
| Descriptor reuse | 0.008–0.01 ms | Already satisfied |
| Output producer completion | 5.9–6.1 ms | Materially blocked |
| Output command retirement | ~0.01 ms | Already satisfied; same render value |

FPS remained about 23–26, zero-copy, ~1 ms Android scene work, no pool drops.
These measurements validate the attribution baseline. The optimizations below
still need matching CI assets and a new phone run; no new FPS is claimed.

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

The SHM importer still maps coherent memory, copies exactly stride × height
bytes, performs the same channel conversion and unmaps before submission.
It retains the staging VkBuffer, VkDeviceMemory and binding instead of freeing
them immediately after `U`. There is no persistent mapped mirror or dirty-range
optimization: existing acknowledged bulk mapped transfers/order are unchanged.
A retained larger-capacity slot can map/transfer more padding than a smaller
new allocation; the logical pixel bytes and copy extent remain identical.
Stride × height is checked for overflow before pool acquisition.

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

## Counters and expected steady RPCs

The existing two-second opcode/category/RTT/FPS reports remain. New local rows:

```text
MaliPerf local-wait: wait-reason=descriptor-set-reuse requests=... locally-completed=... forwarded=... requests/frame=1.00 forwarded/frame=0.00
MaliPerf local-wait: wait-reason=output-command-retirement requests=... locally-completed=... forwarded=... requests/frame=1.00 forwarded/frame=0.00
MaliPerf staging: hits=... misses=... reuse-waits=... resources-created=... resized=... failures=... live=2 limit=3
```

Reason ID 1 keeps its wire value and snapshot size but is now named
`shm-staging-slot-reuse`. It measures fallback waits, rather than immediate
destruction. A warmed compatible pool should have hits, no new resources and
no reuse waits. RPC wait rows count only real calls; local rows expose skipped
waits separately. Native wait timing retains its prior measurement definitions.

| Category | Phone baseline | Expected after A+B |
| --- | ---: | ---: |
| Mapped transfer | 7 | 7 |
| Recording | 14 | 14 |
| Resource management | 12 | 7 |
| Timeline/synchronization | 8 | 5 |
| Descriptor/resource updates | 3 | 2 |
| Queue submit | 2 | 2 |
| AHB presentation | 1 | 1 |
| Total in-frame RPCs | **47** | **38** |

A alone predicts 45/frame. B additionally removes the upload wait, five staging
management calls and one buffer bind, predicting 38/frame after warm-up. Four
source-image destruction calls outside the frame snapshot still exist, plus
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
CI must still compile actual Gamescope/proxy/broker, followed by normal Launch,
variable-size, stop/restart and zero-copy/release checks on the phone.
