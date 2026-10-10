# CP7P normal-frame cost attribution

This pass adds measurements only. The phone-validated baseline remains 60 Hz,
34 in-frame RPCs/frame, three mapped writes, one forwarded output completion
wait, persistent staging mappings, and zero-copy output. New timing results
require the next phone run; the initial ~47 FPS versus sustained ~25–30 FPS
does not by itself establish a cause.

## Exclusive local stages

`normal_detail_perf.hpp` is copied into Gamescope by patch 0122. Collection is
active only between the normal session's existing frame-begin/end hooks.
Nested scopes suspend their parent stage, then restore it: each interval is
charged once. Four aggregate rows distinguish frame preparation/cleanup, SHM
import, `vulkan_composite`, and output publication/retirement.

Every stage reports `wall/thread-CPU` milliseconds per attempt. Wall is measured
on every attempt; thread CPU is sampled on one attempt in sixteen, with the
sample count printed. It is an estimate of execution cost for those sampled
attempts, not a GPU timer. `off-CPU-est` uses wall minus thread CPU on the same
sampled attempts; it includes synchronous waiting and scheduler delays and
cannot distinguish them alone. Compare it with the existing client RTT and
native wait measurements. All values aggregate at the existing two-second
report boundary; there are no per-frame log messages.

| Stage | Start/end and included work |
| --- | --- |
| `scene` | Normal `FrameInfo_t` construction to the connector call; within `vulkan_composite`, entry checks/layer and pipeline selection, excluding the nested stages below. |
| `shm-access` | SHM importer entry through data-pointer access/metadata; after conversion through optional unmap and `wlr_buffer_end_data_ptr_access`. Persistent slots do not unmap here. |
| `staging-pool` | Staging acquisition through persistent pointer/range validation, ending immediately before the image copy. Warm slots and any safe reuse/resize work are measured. |
| `memcpy-convert` | Full `stride * height` memcpy through the existing ARGB/XRGB conversion loops. |
| `descriptors` | In `dispatch`, after image barriers through descriptor preparation and `UpdateDescriptorSets`; ends before binding the descriptor set. The nested reuse check is charged to `local-sync`. |
| `uniforms` | Entire `uploadConstants`: record construction, upload-ring space allocation, and memcpy of `sizeof(data)`. Nested real waits, if any, retain their own stage. |
| `commands` | Entire `commandBuffer` acquisition/preparation and `bindPipeline`; `dispatch` outside descriptor/image stages; SHM copy recording before upload submission. |
| `source-resource` | SHM source texture construction/`BInit`; entire `prepareSrcImage` state-map lookup/insertion and reference bookkeeping. |
| `output-resource` | Output image selection in `vulkan_composite`, and entire `prepareDestImage` state-map bookkeeping. The normal path supplies its existing AHB texture override. |
| `local-sync` | Entire descriptor reuse check, timeline counter query, and device wait; `submit` bookkeeping outside nested `submitInternal`. Includes synchronous RPC wall time when these functions issue one. |
| `submit` | Entire `submitInternal`, including command end, submit-vector preparation, coherent uploads and queue-submit RTT; SHM staging-arm/submission wrapper outside nested scopes. |
| `retirement` | Entire `resetCmdBuffers` and texture destructor; normal source-reference release after Present. Includes nested cleanup RPC wall time. |
| `other` | Remaining active-frame time; output wrapper work outside nested compositor/wait/retirement scopes, including fence export, publish, Wayland commit and fence cleanup. |

The `compositor-cpu` row covers the body of `vulkan_composite`, which is wrapped
by the old `compositor-CPU` wall timer. It prints that old `parent` value and
`parent-minus-stages`; call-boundary overhead is the expected small difference.
SHM copy/import happens **before** that parent timer and is reported separately
as `shm-cpu`. Output completion/publication happens **after** it, under
`output-local`. These new rows partition active-frame wall time; they overlap
the existing broad stage and RPC totals and must not be added to those totals.
The word CPU in the existing report names does not imply wall time excludes
RPCs, waits, or scheduling.

## The three mapped writes

Source chain: `CVulkanDevice::createDevice` creates the Mali upload ring with
512 KiB buffer size and `TRANSFER_SRC | UNIFORM_BUFFER` usage, allocates the
driver's memory-requirements size and maps the full allocation coherently.
`uploadConstants<BlitPushData_t>` writes one small record into that ring for R.
The SHM importer writes the full source image into a separate persistent
staging mapping and arms that mapping for U. `renderer_QueueSubmit` uploads
each ordinary live coherent mapping on **both** submissions; a staging-managed
mapping is uploaded only for its armed command.

| Write | Submission and purpose | Expected data bytes in the supplied workload | Known client stores |
| --- | --- | ---: | --- |
| `compositor-upload-ring` | U, the SHM buffer-to-image upload | 524,288 | SHM import makes no new ring-constant store. The existing ring is fully transmitted again. |
| `staging-SHM` | U, source pixels for buffer-to-image upload | 230,400 | Full `stride * height` copy (320 × 180 × 4), plus existing in-place format conversion. |
| `compositor-upload-ring` | R, BLIT constants | 524,288 | One `sizeof(BlitPushData_t)` store at the ring allocator's returned offset. The new counter prints actual compiled size/offset. |
| Total | Two submissions, three writes | **1,278,976** | Store extents and transmitted extents are different measurements. |

The two ring transmissions are confirmed by the source, not two distinct
512 KiB resources. Their actual lengths are the full mapped allocation: a
driver's allocation padding or a retained larger staging slot may change the
numbers. Do not infer allocation size from the logical image size alone.

`MaliPerf memory-write` aggregates by resource purpose and U/R/other, recording
call/acknowledgment counts, opaque protocol memory IDs (no pointer addresses),
last allocation and mapped offset/size, last write offset/length, min/max
length, whole-map count and requested/acknowledged data bytes. Failed transport
can send only part of a request; its requested bytes are never counted as
acknowledged. It also reports requested wire
bytes: data plus the existing 16-byte message header and 20-byte write
arguments per RPC, excluding replies. A range split by the existing bulk limit
is counted once per actual write RPC. IDs may alternate between the two pool
slots; first/last IDs and `IDs-changed` explicitly identify aggregation.

Classification uses the existing staging opt-in and successful buffer usage
binding; the normal renderer's armed upload command identifies U and its
producer fence identifies R. Other uses remain labelled `other`. No protocol,
upload selection, range, payload, acknowledgment or submission is changed.

`MaliPerf client-writes` separately reports full SHM-copy bytes and actual
uniform store extents. Source proves the ring store is much smaller than the
transmitted mapping, and that this SHM import does not change the ring before
U. It does **not** establish which bytes differ from the previous frame or a
safe general dirty-range policy. Padding, repeated stores, ring wrap/reuse and
other coherent mappings still need their existing semantics. Content
differences remain explicitly unknown: no scans, hashes or dirty tracking.

## Slowdown context and limits

The exclusive wall/thread-CPU rows distinguish growing local execution from
off-CPU time, then locate the affected stage. Existing RTT and native wait
reports remain intact to identify synchronous waits within those stages.

`MaliPerf proxy-lifetime` counts `submit_find` lookups and nodes visited,
average/max traversal length, and the last submit's active/live/mapped records.
Phone results confirmed increasing historical-list traversal as the sustained
slowdown. The subsequent [live registry fix](RESOURCE_REGISTRY.md) excludes
retired records from hot lookup/submit walks, while retaining their storage
until device cleanup. Owned/live-index/active/retired gauges distinguish those
lifetimes. Counters piggyback existing walks and the local snapshot lock;
there is no added resource scan or query RPC.

At each report boundary, `MaliPerf host-context` samples process CPU delta,
the current compositor core, and that core's `scaling_cur_freq` if readable.
The first process delta is invalid; missing core/frequency is `-1`. A boundary
frequency sample is not a residency history and may miss thread migration.
Broker thread CPU/service remain available in existing Android reports;
broker core frequency and thermal status are not newly sampled. No thermal
throttling claim follows from FPS or frequency alone. No scheduling, affinity,
priority, governor or clock settings change.

Proxy write/lifetime rows use an independent two-second window, retained
across the existing local snapshot resets. Compare warmed rates and ranges;
do not subtract totals from misaligned windows. Startup/shutdown or failures
can produce partial windows. Existing diagnostics and perf-disabled paths do
not activate these normal-session counters.

There are no added Vulkan calls or wire messages, and the existing local perf
snapshot ABI is unchanged. The 34-RPC/frame expectation remains unchanged for
the same workload. Collection uses fixed counters and scopes; formatting and
the single frequency-file read occur only at the aggregate boundary. Timer and
counter overhead remains real and should be checked in the next phone run.

## Lightweight checks

```sh
cd tools/mali-vulkan
python3 -B -m unittest -v test_frame_cost test_perf test_bulk test_staging
```

These compile only small extracted helpers with mocked clocks/Vulkan/transport,
not Gamescope or the APK. They check exclusive accounting, CPU sample cadence,
disabled behavior, patch/header parity, existing lookup traversal counts,
three-write attribution over 40 U/R pairs, exact byte totals, acknowledged
upload ordering, no staging upload on R, unchanged transport with profiling
disabled, and existing bulk/staging protections. Renderer patch application is
also checked against the pinned source without compilation. Phone validation
must establish timings, slowdown correlation and unchanged 34-RPC/zero-copy/
ownership/release behavior before selecting another optimization.
