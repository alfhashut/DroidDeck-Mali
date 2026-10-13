# Development Steam → Gamescope → Android tracing

The external dynamic capture is implemented and phone-tested. Its original
captures do not provide private Gamescope stages or a shared frame identity.
The opt-in internal journal below has now produced a successful synchronized
phone capture. The reported 129 joined frames identify SHM import as the largest
measured downstream scope in that capture; its internal costs remain unresolved.
The subsequent SHM decomposition capture reports ~35.8 ms import, ~21.2 ms
upload_submit, ~20.3 ms upload_queue and ~11.0 ms copy, with no upload wait.
Those are user-provided same-capture results, not a local reanalysis. The new
upload-queue subscopes below await a matched CI build and phone evidence.
Missing stages are emitted as unavailable, not inferred timings.

The original external-only diagnostic requires no build. The new internal
journal requires a matched CI Gamescope/APK build, with no renderer, capability,
Steam-owned file, PanVK, or permanent scheduling changes.

## No-rebuild phone command

From the repository root, leave the experimental hardware-accelerated Steam UI
open. Keep the same page animating or scrolling until collection finishes.
Use a new output directory for each capture:

```sh
python3 -B tools/panvk/stage_phone_test.py \
  --serial E68DS8U4IVYTKJXK \
  --frame-diagnostics --perf-seconds 10 \
  --perf-output /tmp/steam-frame-next --run
```

This automatically selects temporary **webhelper-only `0xff`** and uses the
existing detached affinity watchdog. Existing threads regain their individual
original masks, including original all-core threads; newly created threads use
the existing same-name restoration policy. Parent exit, failure, and deadline
restoration remain in effect. Require `affinity-restored.ok=true` with no errors.
The GPU-thread sampling records its live affinity as well.

Default recording includes CEF scopes, XDamage timestamps and mapped/depth
snapshots, 100-ms thread observations, one-second CPU counters, broker logcat,
SurfaceFlinger graphics tracing, and native-layer latency histories every two
seconds. Periodic histories prevent the 128-entry presentation history expiring
while CEF's report drains. Nothing clears SurfaceFlinger history or reads a
process's stdout pipe. The helper audits regular session log outputs and detects
the existing discard logger. It audits the exact mapped Gamescope ELF by inode;
it neither scans private addresses nor reads renderer process memory.

`--frame-scheduler` additionally records system-wide switch/wakeup events. Keep
it optional: it materially perturbs this PRoot workload, and the phone's vendor
kernel produces `task_state_invalid` importer errors. Those captures cannot
provide a trusted exact sleep/runqueue split. Scheduler recording uses Perfetto's
[compact scheduler encoding](https://raw.githubusercontent.com/google/perfetto/main/protos/perfetto/config/ftrace/ftrace_config.proto).
The recorder stops only its own PID after checking the exact unique output path
and executable name. Duration and 128-MiB file limits bound an abandoned recorder.

## Offline analysis

Use an existing Perfetto `trace_processor_shell` executable. This session used
the official v58.2 executable at `/tmp/droiddeck-trace-processor`:

```sh
python3 -B tools/panvk/analyze_steam_frames.py /tmp/steam-frame-next \
  --trace-processor /tmp/droiddeck-trace-processor \
  > /tmp/steam-frame-next-analysis.json
```

The analyzer checks live ANGLE_VULKAN/PanVK/Mali-G52 proof, affinity restoration,
CEF trace integrity, the Perfetto BOOTTIME domain, and start/end clock-anchor
agreement. It reports system-trace coverage and health. Each native-layer
presentation retains nearby CEF/X11/latch timestamp candidates, explicitly
without assigning a common frame ID. It preserves ambiguous candidates.
Android cumulative handoff/release rates use only counter points inside this
capture, and report their shorter interval separately.

SurfaceFlinger latency columns are desired, actual-present, and frame-ready
timestamps, following [AOSP FrameTracker](https://android.googlesource.com/platform/frameworks/native/%2B/8c9cd3c537f7d4781862e2d95b32247fa5bb3d66/services/surfaceflinger/FrameTracker.cpp).
Zero, pending, old, and Java SurfaceView records are excluded from the native
game-layer presentation count. Histories are deduplicated across snapshots.
The native layer's `BufferTX` gauge measures pending buffer transactions;
departures mean latch **or drop**, per [AOSP BufferStateLayer](https://android.googlesource.com/platform/frameworks/native/%2B/27c20ff12d67b4667edb01160289afc8a4e23373/services/surfaceflinger/BufferStateLayer.h).
Only complete 0→1→0 cycles receive a pending duration; depth>1 is ambiguous.
These durations are neither GPU execution nor AHB ownership/release times.

## Phone observations, 2026-10-11

The final moving-UI capture is `/tmp/steam-frames-graphics-moving`, with analysis
at `/tmp/steam-frames-graphics-moving-analysis.json`. Its 10.198-second interval
had complete CEF and system traces, no reported system trace errors/loss, and
successful mixed-mask restoration for 121 recorded threads. Live renderer:
`(gl=egl-angle,angle=vulkan)`, `ANGLE_VULKAN`, Mali-G52 r1 MC2, panvk-26.2.99.
GPU main and Viz threads were observed on affinity CPUs 0–7.

| Same capture observation | Result |
| --- | --- |
| CEF MainFrame.SubmitCompositorFrame / GPU DrawFrame | 119 / 119; 11.67 events/s |
| Completed GPU swap events | 118; 11.57 events/s |
| Mapped Steam X11 window `0x2200034`, depth 32 | 117 batches; 11.47/s |
| Native game-layer latch observations | 117; 11.47/s |
| Native game-layer actual presentation history | 116; 11.38/s |
| Android handoff / release | 11.44 / 11.44 per second over the internal 10.055-second counter interval |
| CEF swap wall / CPU / off-CPU | 52.61 / 7.03 / 45.58 ms mean |
| Native-layer pending buffer transaction depth | maximum 1 |
| Complete pending transaction cycles | 116; mean 2.97 ms, p50 2.82 ms, p95 3.84 ms |

The native layer cleared pending transactions quickly and showed no deep queue
in this capture. This **does not rule out release-fence/AHB-reuse backpressure**
before the next handoff. The capture did not reproduce the prior 21–22 FPS CEF
workload, so it cannot explain that prior rate gap. Workload, thermal state and
observer overhead were not controlled enough to attribute the changed rate.
Do not subtract these aggregate counts to label dropped/repeated frames.

A separate compact-scheduler capture (`/tmp/steam-frames-compact`) produced
16.03 CEF events/s, 15.07 native latches/s, and 15.17 native presentations/s.
Its 179,160 `task_state_invalid` errors prevent trusting scheduler-state
classification. Its timings are not added to the final capture. The earlier
moving trace reached its file cap before sampling ended and is not suitable for
full-interval attribution. A graphics-only idle capture observed one frame and
is not an FPS benchmark.

## What prevents exact attribution

The installed Gamescope has only `.dynsym`; no relevant private frame/perf
objects are exported. Its stdout pipe is currently consumed by `cat` with no
regular output, matching the session script's discard phase. `session.log`
stayed at 2,478,335 bytes with zero new bytes during the final sample. Do not
infer why its earlier `dd` exited from the present file size alone. Consequently
fresh Gamescope SHM/render timing rows and Gamescope-side warning recurrence
cannot be recovered from that stream. No fresh selected KBASE/JOB fault lines
were observed in the available log tails; that is incomplete coverage, not proof
the warnings stopped. No warnings are suppressed.

SurfaceFlinger FrameTimeline records cover the Java activity, not native
`droiddeck_wayland_game`; those activity tokens must not stand in for Steam
frames. HWC composition can include unchanged layer contents. Neither stream
identifies Gamescope repeats or Android repeats of a particular AHB frame.

Full requested attribution requires a narrow diagnostic instrumentation build
of **Gamescope and the Android native bridge**, and thus an APK containing that
bridge if it cannot be independently replaced. No such build was run. The
required opt-in records are:

- Selected surface commit sequence, replaced-pending sequence, chosen client
  sequence at render start, and SHM import/upload begin/end (wall + thread CPU).
  Hooks belong in the normal session loop and texture upload paths introduced
  by Gamescope patches `0121`, `0124`, and the existing normal detail perf scopes.
- Compositor render begin/end, output acquisition, actual Vulkan submit and
  completion/wait, retaining the Gamescope output frame and AHB key/sync identity.
- Native `mb_normal_publish`, `mb_normal_submit`, ASurfaceTransaction handoff,
  and `mb_normal_release`/release-fence completion with the **same existing
  `(key, frame, sync, fence)` identity**, recorded before state is cleared/reused.
  Pending commit/owned/submitted counts must accompany those records.

A bounded event journal drained after sampling would avoid per-frame logging
in the hot paths. That instrumentation must preserve all rendering and wait
behavior, carry CPU and monotonic timestamps, and report overflow explicitly.
Until those records exist, SHM copy, compositor work, GPU waits and AHB-reuse
backpressure remain competing unmeasured causes. No optimization is proposed.

Validation: 58 lightweight Python tests passed, including clock discontinuities,
foreign-recorder isolation, affinity restoration, ambiguous timestamp/queue
matches, and renderer fallback rejection. `git diff --check`, Python AST parsing,
and whitespace checks for untracked diagnostic files passed.

## Internal frame journal: SHM decomposed, upload queue detail sampling

The external-only limitations above describe the previous capture. Patch
`0126-mali-frame-trace.patch` and the Android hooks now add an **opt-in internal
journal**. The first successful phone capture reported 129 joined frames with
no unmatched publish IDs: whole-frame wall mean 62.54 ms, SHM import 40.89 ms,
SHM copy 13.22 ms, render 7.08 ms, AHB acquire about 0.0027 ms, Android queue
submit 1.23 ms and handoff 1.12 ms. These are the user's same-capture analyzer
results, not a fresh local reanalysis. The next decomposition capture reported ~35.8 ms
import, ~21.2 ms upload_submit, ~20.3 ms upload_queue and ~11.0 ms copy, with
negligible staging and no upload_wait. Nested or differently sampled means must
not be added/subtracted. Upload_queue is now the largest known downstream cost;
its internal proxy/transport/native costs still await the new capture. No GPU
execution time is measured. No optimization is implemented or recommended here.

The default path, renderer, queues, ownership transitions, waits, capabilities,
client atom stride, and pacing remain unchanged. Nothing sets a new affinity
policy at Launch. The existing capture watchdog still applies webhelper-only
`0xff` temporarily and restores the original mixed per-thread masks. PanVK and
Steam libraries are not rebuilt or edited.

### Activation and overhead

The fixed shared directory is
`/storage/emulated/0/Download/DroidDeckFrameTrace`. An explicit `armed` file must
exist **before normal Mali Steam Launch**. Only normal-session startup initializes
the two workers, one in Gamescope and one in Android's shared `libmaliahb.so`.
Unarmed event calls return after an atomic gate check: no clock reads, journal
locks, allocation, worker, or journal I/O. Startup checks the arm marker once.
Diagnostic bookkeeping does not participate in rendering/pacing decisions.

When armed, each process allocates 65,536 fixed records (about 5.5 MiB), checks
control every 100 ms, and records into RAM with a nonblocking try-lock. It never
waits behind another observer: contention/capacity loss increments `dropped`.
The worker serializes JSONL **after the recording window**, independently of
stdout/session.log and its discard logger. Default CPU sampling is every eighth
attempt/frame/fence/timeline value. Wall timestamps are recorded for every
accepted event. Compare an unarmed external capture of the same workload if
observer overhead appears material; instrumentation overhead is not yet phone
quantified.

The guest `steam_frame_journal.py` controller runs automatically during
`--frame-diagnostics` if armed. It requires exactly one fresh valid ready marker
per role, with guest `/proc` liveness for Gamescope only (see activation rules
below). It writes one shared `run start_ns end_ns cpu_stride` control atomically,
one second ahead, with two seconds of trailing guard. Native windows are bounded
to 30 seconds; use the recommended 10-second external sample (maximum internal
sample 27 seconds). A capture does not change an active control window. The
journal header records when each worker armed that capture (`ready_ns`), so late
activation is visible. Disarming flushes an interrupted window with
`complete=false` and exits the workers. Re-arm requires another Stop/Launch.

### Instrumentation and identities

All event timestamps use the phone kernel's `CLOCK_MONOTONIC`; thread CPU uses
`CLOCK_THREAD_CPUTIME_ID`. Guest PRoot and native Android share this clock. Keep
the existing BOOTTIME/Chromium/X11 clock anchors when comparing external traces.

| Source and exact hook | Records / meaning |
| --- | --- |
| Gamescope `mali_x11.inc::X11Commit`, native `Commit`, selection/dissociation | `client_commit`: Gamescope-local monotonic 64-bit commit sequence; XID in `object`, mapped in `value`, selected in `status`. All window commits with a buffer are observed, including unselected windows. Selection, queued pending replacement (`client_coalesced`) and pending discards retain the old/new sequences. A commit already claimed by the current render attempt is excluded from those queued-replacement counts; its attempt result is separate. This is the wlroots commit callback, not CEF production or Xwayland's earlier damage receipt. |
| Normal loop around frame begin/end | Monotonic 64-bit `attempt`, chosen `client` sequence, success/error status. `frame_begin.status=1` means the same known commit as the last successful render. Commit callbacks can reflect state changes without new pixels; equal/different commit sequences do not prove equal/different pixel contents. |
| Loop around `wlr_surface_get_texture` and `vulkan_create_texture_from_wlr_buffer` | `shm_import_begin/end`: combined import, staging acquisition, texture creation, upload recording/submission and any existing wait. `shm_import_end.object` is a local buffer pointer only; it never crosses processes as an identity. |
| `rendervulkan.cpp` actual SHM memcpy through existing channel conversion | `shm_copy_begin/end`, byte count in `value`. Includes copy and channel conversion; distinct from total SHM import. No new copy or readback. |
| `vulkan_mali_normal_present` around ownership acquire | `ahb_acquire_begin/end`; on success assigns existing broker `key` and existing monotonic output `frame`. `object` is slot index; `value` is the previous frame in that slot, exposing reuse. EAGAIN stays an EAGAIN. |
| Around existing `vulkan_composite` | `render_begin/end`, slot and returned timeline sequence; CPU command/render call scope, not GPU execution. |
| Around existing output-completion, exported-sync and output-retire waits | `wait_begin/end`: reason and timeline/sync identity. End means the existing wait returned; includes CPU/RPC/scheduling costs. No new waits or timestamp queries. |
| Native `MB_RENDERER_SUBMIT` around actual `vkQueueSubmit` | `vk_submit_begin/end`, native timeline semaphore ID in `object`, timeline value in `value`, output `key` and producer `fence` when known. No frame exists here yet (`frame=0`). |
| Native `MB_RENDERER_WAIT` and `MB_NORMAL_PUBLISH` sync wait | Actual timeline or sync wait begin/end, result, wall and sampled CPU. Timeline waits use semaphore/value; publish sync wait already knows key/frame/sync/fence. |
| Successful normal publish in Gamescope and Android registry | `publish` carries **the existing `(key,frame,sync,fence)`** on both sides. Android's `object/value` are current owned/submitted pool counts. |
| `mali_normal_commit` and its wl_buffer release listener | `outer_commit/outer_release` retain an immutable copy of that buffer's Gamescope frame/attempt/client IDs until actual Wayland release. These are sender-side commit and callback receipt, not Android latch/scanout. |
| Normal output availability check | `output_pool_blocked` only on blocked/unblocked transitions. Blocked: all three slots unavailable; unblocked: first available slot index. No polling/event per idle loop iteration. |
| `ahb_swapchain_present` around existing `sc_layer_present_ahb` | `android_handoff_begin/end`, immutable key/frame identity and local buffer token. Covers transaction setup/application, not physical presentation. |
| Registry submit / same-token SurfaceControl branch | `android_submit` after existing success; `status=1` for a re-submit. `android_repeat` is the same-token application redraw/geometry branch: no new setBuffer and no claim of a repeated panel refresh. |
| Existing SurfaceControl transaction OnComplete | `transaction_complete` identifies the **incoming** frame; `object` is outstanding callback count before decrement. Optional `ASurfaceTransactionStats_getLatchTime` yields `android_latch_time.value` in ns for that incoming frame. Event `mono_ns` is callback delivery; `value` is the actual reported latch time. Missing symbol/invalid stats remain unavailable, never estimated. |
| Same callback's previous-buffer release | `release_callback` identifies the **replaced** frame, saved when registering the transaction. `value` indicates presence of a release FD. Callback never looks up a mutable buffer list or assigns the incoming frame's ID to the outgoing fence. |
| Compositor release queue, existing poll/readable callback | `release_queued`, `release_fence_pending`, `release_fence_observed`. Observation requires the existing successful readable poll; an error is not ACK. `value=0` on release_queued means no fence was supplied: do not invent a fence signal time. Implicit DMA-BUF import is not a fence-completion observation. |
| Android registry release; Gamescope buffer reuse | `pool_return` after the actual owned/submitted bits clear (with pool counts), followed by actual `outer_release` receipt and later `ahb_acquire_end` of the same key with a new frame. Unsubmitted cancellation is separate `cancel_unsubmitted`, never a release. |

### SHM import decomposition: exact operations, no behavior changes

The old `shm_import_begin/end` surrounds the normal loop's
`wlr_surface_get_texture(surface)`, reinterpretation of `VulkanWlrTexture_t`, and
`vulkan_create_texture_from_wlr_buffer(wrapped->buf, nullptr)`. For a data-pointer
buffer, that function probes DMA-BUF, begins read access, acquires staging,
validates its persistent mapping, copies/converts pixels, ends source access,
creates the sampled destination texture, acquires/begins/records a command
buffer and submits the upload. Texture creation is **before upload**, not a
post-upload registration operation. `staging.submitted(sequence)`, lease
bookkeeping/destructors and returning the texture also remain inside import.
The later `vulkan_mali_normal_upload_done()` and compositor/output work are
outside the journal's SHM parent.

All added events use the existing journal and a snapshot of `g_maliFrameTrace`:
`client` and `attempt` are exact. Output `key/frame` are usually still zero until
AHB acquisition. The analyzer retains those original fields and attaches
`joined_key/joined_frame` via the attempt's exact successful publication; no
timestamp-proximity match or new wire ID is introduced. Local handles/byte
counts/timeline values in `object/value` are diagnostic only.

Each name in this table has `_begin` and `_end` events:

| Stage | Exact code boundary / nesting |
| --- | --- |
| `shm_lookup` | Around the existing `wlr_surface_get_texture` in `mali_normal.cpp`. |
| `shm_source` | Entry of `vulkan_create_texture_from_wlr_buffer`, DMA-BUF probe, `wlr_buffer_begin_data_ptr_access`, width/height setup; ends before staging acquisition. A DMA-BUF/failed-access early return closes this scope but cannot satisfy the data-pointer decomposition. |
| `staging_acquire` | Lease setup, range check and complete `maliStaging.acquire`; also encloses legacy buffer create/allocate/bind when that existing fallback runs. Includes pool scans, cached completion checks, optional allocation/resize/retirement and reuse waits. Pool policy is untouched. |
| `staging_create` | `MaliStagingBackend::create` from API resolution through CreateBuffer, requirements/type selection, AllocateMemory, BindBufferMemory, mapping and existing error cleanup. Nested in acquire, present only when a slot is created. |
| `staging_map` | Around the actual `mapStaging` call on cold slot creation; around existing legacy `MapMemory` on fallback. Does not claim a new map occurs on a warm persistent slot. |
| `staging_validate` | Selection/check of the cached mapped pointer and mapped byte range; includes the legacy map branch if used. Warm validation is separate from an actual map. |
| `shm_copy` | Existing unchanged memcpy plus ARGB/XRGB channel conversion and byte accounting. |
| `shm_source_release` | Existing legacy UnmapMemory, if any, then `wlr_buffer_end_data_ptr_access`. |
| `texture_create` | `new CVulkanTexture`, flags and full `BInit`, including image/memory/view creation and their existing proxy RPCs. Ends before upload recording. |
| `upload_record` | `g_device.commandBuffer()` (allocation/reuse and BeginCommandBuffer) through `copyBufferToImage`. |
| `upload_prepare` | Existing `maliStaging.backend.prepare` call and result check, marking the coherent staging allocation for this upload. No upload is added. |
| `upload_submit` | Full existing `g_device.submit(std::move(cmdBuffer))`: finalization, submit-info/ring bookkeeping, QueueSubmit and pending-command registration. Ends before `staging.submitted(sequence)`. |
| `upload_finalize` | Nested in pooled upload submit only: existing `cmdBuffer->end()`, barriers and EndCommandBuffer. Enabled by the already-existing `maliStagingUploadRecording` flag; compositor submissions are excluded. |
| `upload_queue` | Nested in pooled upload submit only: exact `vk.QueueSubmit` call and existing result check, after ring sealing. The proxy's coherent mirror upload/bulk writes/ACKs, locks, MB_RENDERER_SUBMIT RPC and native queue call are included in this API scope. |
| `upload_wait` | Existing `MaliProfiledWait` inside staging backend reuse, ending **before** successful command-buffer reset; nested in acquire. Also wraps the existing immediate `g_device.wait(sequence)` on legacy fallback. No wait is introduced or moved. Buffer/sequence distinguish actual waits. |
| `shm_cleanup` | Existing legacy DestroyBuffer/FreeMemory after its wait. Absent on the normal persistent-slot path. |

Normal pooled upload has **no immediate completion wait after submission**.
No `upload_wait` event is fabricated for implicit queue ordering or GPU work.
Later output/compositor waits keep their existing `wait_begin/end` events and
are outside import. Backend reuse can still wait for an earlier upload.
Scope destructors close early-return paths; parent import status records import
failure. The added scope helper's `finish()` emits once and never performs any
renderer/resource/synchronization operation.

`stage_spans` summarizes every observed pair using the existing count, wall
mean/p95, CPU sample count/mean, paired wall mean and paired off-CPU mean.
`span_diagnostics` explicitly reports unknown scopes, unpaired edges, duplicate
begins, changed client identities and invalid CPU samples. Ambiguous pairs are
discarded rather than overwritten. CPU requires the same PID/TID and valid
samples at both edges; missing/invalid CPU never becomes zero or GPU time.

`shm_import_decomposition` contains per-import identities/durations and:

- `total_import_mean_ms` for complete joined parent scopes in the sample;
- `stage_inclusive_mean_ms` and `stage_span_counts` for the children. Means use
  the same `remainder_samples` imports; absent optional operations contribute
  zero, without creating fake spans. These stage means **overlap**;
- `covered_import_mean_ms` and `unattributed_inside_import_mean_ms`, using the
  interval union of actual child scopes on the parent's thread, equivalent to
  subtracting mutually exclusive covered time only once;
- `paired_total_import_mean_ms`, `remainder_samples`, `ambiguous_imports` and
  per-import `remainder_unavailable_reasons`, making subset selection explicit.

Remainder is unavailable for incomplete/lossy/late captures, missing required
new stages, malformed/unpaired child scopes, multiple parents, a child on a
different thread/client, a child outside its import or crossing (improperly
nested) children. Old captures with only import/copy remain analyzable, but
cannot supply the new decomposition. No timings from separate runs are combined.

Uncovered gaps include ordinary setup/return/lease bookkeeping, scheduling
between scopes and diagnostic overhead. Included parent scopes still contain
their own internal work: acquire includes pool scans, resize destruction and
retirement; texture creation includes multiple resource RPCs. In particular,
`upload_queue` cannot independently split ICD lock wait, bulk-copy/ACK transport,
broker scheduling and native QueueSubmit time. Native upload submits still lack
a shared output key/fence at that point. The new same-attempt API scope does not
invent a native upload join, GPU execution time or an exact kernel wakeup time.
Use the next synchronized phone capture to locate the dominant API stage before
proposing any further diagnostic split or optimization.

`client` and `attempt` are Gamescope-local diagnostic IDs. They are carried into
its output-buffer metadata and `outer_release`, but **not added to the wire
protocol**. Android uses the existing output IDs. Journal `fence` is the broker's
32-bit producer-fence resource ID, returned in `DD_FENCE_CREATE`'s existing
operation-specific `token` output. The ICD obtains it from the proxy registry
entry's `id`, which came from `MB_SYNC_FENCE_CREATE` and is passed unchanged to
`MB_SYNC_EXPORT`; Android stores it as `native_sync.fence`. `producer.fence`
remains the local opaque `VkFence` proxy handle used for submission, sync export
and destruction. Its pointer bits are never used for cross-process correlation
or recorded as the journal fence ID. Neither identity is a native FD: `sync`
identifies the exported sync wrapper whose FD stays native-side. This adds no
ABI fields or RPCs and changes no synchronization. An older ICD leaves the
fence-create token zero, so matching remains unproven rather than guessing an ID.
Offline matching requires
exact, unique successful Gamescope **and** Android publications of
`(run,key,frame)` with equal nonzero `sync/fence`. It then attaches earlier
Gamescope SHM/render records by `attempt`. Native pre-publication submit records
attach only via a unique successful publication of the same `(key,fence)` in the
same process/run; their timeline waits attach only by the same diagnostic device namespace (`scope`)
and semaphore/value. This namespace increments per native normal device start,
so equal wire semaphore IDs on different connections cannot alias.
Neither timestamp proximity, local pointers, XIDs alone nor AHB slot alone
establish a cross-process frame identity. Ambiguous/missing publications remain
unjoined. The run also separates process/session restarts and capture windows.

The optional latch query uses the existing complete callback and is not a new
capability requirement. See the [NDK SurfaceControl contract](https://developer.android.com/ndk/reference/group/native-activity#asurfacetransactionstats_getlatchtime)
and [AOSP implementation](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android13-qpr3-c-s1-release/native/android/surface_control.cpp).
It does not obtain the transaction's physical present-fence signal time.

### Expected output and analysis

Files are `trace-RUN-PID-gamescope.jsonl` and `trace-RUN-PID-android.jsonl`.
The host pulls only the current control's exact run/PID/role filenames into
`PERF_OUTPUT/frame-journal/`, with its `capture-RUN.json` control receipt. A pull
failure is retained as a `.pull-error.txt` file. No wildcard pull of prior runs.
Example event, with illustrative values only:

```json
{"type":"event","event_seq":73,"stage":"android_handoff_end","mono_ns":123456789000,"cpu_ns":123000,"tid":321,"client":0,"attempt":0,"scope":0,"frame":128,"key":7,"sync":456,"fence":455,"object":2,"value":0,"status":0}
```

Each file begins with format version, role/PID, run, clock, start/end, activation
(`ready_ns`), CPU stride and capacity. Its footer gives accepted `count`,
`dropped`, and `complete`. Missing footer, wrong run/window/clock or sequence
integrity is rejected; incomplete/lossy/late-armed windows are flagged. Rates are
**observed** counts, lower bounds under loss. Journal guard events allow scope
pairing, but reported rates and full spans are restricted to the external
`PERF_SAMPLING_START/END` window. Events carry `within_sample`, and ownership
lifecycles extending into guards are marked partial. Trailing guard frames can
run after affinity restoration; do not use those intervals for a policy
comparison. CPU durations require both samples on the same
PID/TID. Paired wall/CPU/off-CPU means use the same subset. Nested stages overlap;
never sum them. Queue depth is registry ownership/callback count, not an inferred
SurfaceFlinger queue.

Run both analyzers on the same capture. Passing the `frame-journal/` directory
directly also reads its parent's `guest-report.txt`, if present, to retain the
external sample bounds and exclude guard frames after affinity restoration.
Without that report only the full journal control window is available.
The original analyzer retains mandatory
ANGLE_VULKAN/PanVK/Mali-G52 and affinity-restoration validation. Its old
`stages_unobservable` fields describe **external-only** coverage; the new
companion provides the exact-ID internal records and stage scopes:

```sh
python3 -B tools/panvk/analyze_steam_frames.py /tmp/steam-frame-internal \
  --trace-processor /tmp/droiddeck-trace-processor \
  > /tmp/steam-frame-internal-external.json
python3 -B tools/panvk/analyze_frame_journal.py /tmp/steam-frame-internal \
  > /tmp/steam-frame-internal-journal.json
```

Coverage gaps remain: CEF/XDamage lack these commit IDs; native upload submits
need the exact namespace/transaction binding described below, otherwise stay unassigned; no hardware GPU execution clock
or exact kernel fence signal/wakeup timestamp is added; physical Android
scanout and repeated panel refreshes lack a shared ID. SF latency histories
remain independently measured timestamp evidence, with ambiguous matches left
ambiguous. Unselected/unmapped commits, coalescing, pool pressure, application
repeats and per-frame ownership lifetimes become observable. Keep fresh
`KBASE_IOCTL_MEM_JIT_INIT`, KFAULT and GLX/Composite warning capture unchanged;
repeated warnings remain evidence, not an assumed cause.

### Upload queue decomposition: existing proxy/broker operations

This is observation only. `CVulkanDevice::submit` seals the existing upload
ring and calls `vk.QueueSubmit`. The real call path for the pooled SHM upload is:

```
CVulkanDevice::submit / upload_queue
  renderer_QueueSubmit (ICD)
    validate submit, submit_find command/semaphore/fence (registry lock)
    lock connection; walk active coherent mirrors
      [upload ring: fallback/dirty ranges, when present]
      interop_copy_upload_locked -> interop_upload_mapping_locked
        acquire/reuse bounded request storage
        for each <=512-KiB chunk: serialize + memcpy mirror into request
          rpc -> rpc_exchange -> header/write header/write payload
                              -> read reply header/read ACK/decode
        check ACK/accounting
    unlock; serialize submit; renderer_rpc (allocate/pack, lock connection)
      rpc -> rpc_exchange
        broker serve: read header/body, decode device -> native_renderer_command
          validate resources/order -> real native vkQueueSubmit
          update command/semaphore state -> serialize/write reply
      validate reply, unlock/free
    lock connection; mark rings submitted/reset staging command; unlock
```

Bulk `MB_MEMORY_WRITE` dispatch validates the mapped range/reference state,
then `memcpy`s the received bytes into the existing Android mapped allocation.
It does not introduce a new map, flush, or GPU wait. Broker submit continues to
use the Android system Vulkan device. No client PanVK code or artifact changes.

The Gamescope-owned journal sink is passed through the private **local-only**
`vkDroidDeckFrameTraceMALI` callback entrypoint around the existing QueueSubmit,
and cleared immediately afterward. It sends no RPC, creates no worker and
advertises no Vulkan extension. Detailed proxy and broker scopes now use a
shared deterministic stride of **16 fence-less renderer submissions**. The
previous proxy-only `attempt % 8` sampler left Android recording every bulk
request. Two 10-second phone captures dropped 1 and 5 Android events at only
13,650 and 13,597 accepted events, far below capacity. The journal's nonblocking
`trylock` can reject an event while another callback/broker thread is writing;
the footer alone cannot prove the exact lost event or contention site. This
change reduces observer traffic; capacity, try-lock behavior and all rendering,
RPC and synchronization operations remain unchanged. Zero loss still requires
phone validation.

Both endpoints count the actual ordered connection stream from successful
`MB_NORMAL_BEGIN`, including outside the capture window. Only an existing
28-byte `MB_RENDERER_SUBMIT` with wire fence ID zero advances the counter (the
normal pooled upload U); nonzero-fence compositor R submits and memory chunks
do not. With 15, 31, 47, ... completed U requests, both sides select the next U
and its preceding bulk writes/ACKs. Android cannot see a Gamescope attempt ID
before reading those bodies, so this is a truthful equivalent stream sampler,
**not a claim of every 16th CEF frame or Gamescope attempt**. A preceding R
submit/transfer can also be selected while awaiting that U. The existing normal
path serializes QueueSubmit on this connection. There is no new wire field or
cross-process ID.

The proxy freezes one decision for the whole QueueSubmit context; Android
freezes one per request before body read. Begin/end selection never re-evaluates
the modulo after submit increments the counter. A journal loss or a capture
boundary can still truncate a selected pair; the analyzer excludes it. Default
CPU sampling remains every eighth attempt/frame/fence/timeline value.

Every-attempt frame/queue/publication events, `proxy_submit_link`, and existing
native `vk_submit_begin/end` remain unsampled. A small `proxy_detail_policy`
record is added for every observed valid QueueSubmit: original client/attempt
and connection scope, `object=completed_U_count`, `value=16`, `status=0|1`
(unselected/selected, **not** VkResult), CPU sampling disabled. This explicitly
identifies intentional omissions. All proxy detailed scopes below and the new
broker receive/dispatch/mapped-copy/reply scopes use the shared selector.
Essential Android presentation/release events are unchanged. The larger
bounded journal allocation exists only in armed sessions; no JSON/log I/O occurs
inside a scope. Observer overhead still needs phone validation.

All listed scope names have `_begin`/`_end` events:

| Scope | Exact existing boundary |
| --- | --- |
| `proxy_prepare` | QueueSubmit entry through submit validation/resource lookup/previous failure check. |
| `proxy_lock` | Each existing pthread_mutex_lock call only: value 1 registry lookup, 2 mirror walk/upload, 3 renderer RPC, 4 submit final bookkeeping. Includes scheduling/API overhead, not proof of mutex contention by itself. |
| `proxy_mirror` | Locked active-registry walk, coherent/ring selection, existing transfers and unlock; nested transfers overlap. |
| `proxy_dirty` | Existing ring fallback/range-byte/accounting decision before transfer, only when a ring is present. Post-transfer ring marking remains in mirror/finalize scopes. |
| `proxy_bulk_setup` | Limit/capacity calculation and existing conditional realloc/reuse. |
| `proxy_bulk_pack` | Each chunk length/header preparation and mirror-to-request memcpy. |
| `proxy_bulk_rpc` | Each existing memory-write rpc call, including ACK receipt and existing performance accounting. Object=memory resource ID, value=offset. |
| `proxy_bulk_ack` | Existing chunk ACK validation/accounting through loop increment/exit. |
| `proxy_submit_pack` | Submit field serialization (value 0); renderer_rpc allocation/device-prefix packing (value 1). |
| `proxy_submit_rpc` | Existing renderer_rpc call, including packing/lock/transport/reply/free. Object=timeline semaphore resource ID, value=signal value. |
| `rpc_exchange` | Actual rpc_exchange entry through return/error cleanup. |
| `rpc_header` | Existing transport header encoding only. |
| `rpc_send` | Each existing complete mb_write call, header then payload; original short-circuit order retained. |
| `rpc_receive` | Each existing complete mb_read call, reply header then payload. Includes socket blocking and rescheduling; these cannot be separated with this journal. |
| `rpc_decode` | Result decoding after payload receive, including original error handling. Reply-header validation remains exchange remainder. |
| `proxy_reply_handle` | renderer_rpc size/count/output validation, connection unlock, request free. |
| `proxy_finalize` | Existing successful-submit accounting/ring/staging bookkeeping through unlock. |
| `broker_receive` | Existing request-body mb_read only, after header receipt. Requires exactly one active normal device on this connection; object=wire opcode, value=body bytes. CPU samples may be absent when no known frame/timeline exists. |
| `broker_dispatch` | After body receipt/device lookup, immediately around existing memory-write or submit handler. Includes handler validation/native work/state updates. |
| `broker_mapped_copy` | Existing memory-write memcpy into the native mapped allocation only. |
| `vk_submit` (existing) | Existing real native vkQueueSubmit call plus the original tiny perf accounting wrapper. This is API wall time, not GPU execution time. |
| `broker_reply` | Existing reply serialization, original log decision/formatting, header+payload writes through return. No logging behavior changes. |

Identity is unchanged: client/attempt belong to Gamescope, output
`key/frame/sync/fence` retain their existing meanings. `fence` remains a broker
resource ID, never a VkFence or FD. `proxy_submit_link` records the existing wire
semaphore ID/signal value and a **process-local** connection pointer in `scope`.
Android retains its existing device `trace_scope`. A successfully published R
with exactly matching key/frame/fence AND semaphore/value anchors those two
namespaces; conflicting/many-to-one anchors are rejected. U submissions then use
the same namespace/semaphore/value, without a new wire ID. Native `vk_submit`
uses the unique exact submit link even when detailed RPC scopes are intentionally
absent, preserving every-submit attribution. Detailed broker stages additionally
require the complete synchronous RPC bounds described below.

Memory writes have no frame/timeline on the wire. Attribution requires the
mapped namespace, exact memory ID/offset, and **exactly one complete synchronous
client RPC interval containing the native handler span**. That is a causal
transaction bound on the serialized request/ACK stream, not nearest-timestamp
matching. Ambiguous/partial intervals stay unassigned. A reply end may race the
client's final read; the immediately preceding same-thread decoded dispatch
with the same namespace/resource/value binds that reply in request order.
The immediately preceding body receive on the same worker and unique normal
device scope is bound by decoded dispatch order. No resource/frame identity is
claimed before decoding. Native events keep client/attempt zero; the analyzer records the successful
publication identity and `identity_source` separately. No synthetic cross-process
frame/wire IDs are sent.

`stage_spans` gives the existing count/mean/p95, CPU sample count/mean, paired
wall mean and off-CPU mean for all new spans. `upload_queue_decomposition` gives
per-queue identities, inclusive stage interval-union means, real pre-submit
exchange time, RPC wall time, observed broker coverage, and
`unattributed_inside_queue_mean_ms`. Remainder subtracts **only same-thread
client interval union**. Native work runs concurrently with client receive and
is never added to client coverage. Explicitly unselected queues have
`sampling_state=intentionally_unsampled`, no remainder, and no malformed/lost
classification. `selected_queues`, `intentionally_unsampled_queues`,
`complete_selected_samples`, `incomplete_selected_queues`, and
`ambiguous_or_incomplete_queues` expose the sample population. Only complete
selected detail enters new-build decomposition means; missing/ambiguous selected
scopes are excluded. Legacy traces with a complete old detail set are reported
separately as `legacy_complete_samples`; they do not satisfy new-build phone
acceptance. `dropped>0` on either footer invalidates exact decomposition for the
whole capture even if the loss seems unrelated. Incomplete/malformed/lossy queues
have an unavailable remainder with a reason. `broker_complete_samples` requires
one decoded handler for every observed client bulk/submit RPC; absent native
coverage remains null rather than zero. Native per-stage counts distinguish
`paired_rpc_wall_mean_ms` uses exactly the broker-complete queue subset;
`broker_stage_sample_counts` and per-queue span counts expose missing coverage.
RPC time outside observed broker scopes still
includes body reception, transport, scheduling and observer gaps; it is not a
pure network or scheduler measurement.

Coverage gaps: broker header wait precedes request/device identification and
remains inside client RPC wall time only. Body reception is directly measured
when the connection has one normal device; otherwise it stays unassigned.
No broker timestamps are backdated or guessed. Driver-internal locks, kernel
socket-ready-to-running delay, and GPU execution remain unobservable. The
journal cannot assign unsampled/partial native requests by timing proximity.
No optimization recommendation follows until the next capture has complete
sampled decomposition and sufficient exact native attribution.

### Exact CI build and phone procedure

The journal **Python control fix alone needs no new APK/Gamescope build** if the matched
native hooks below are already installed. `frame_journal.py` runs on the host;
`--frame-diagnostics` dynamically pushes `steam_frame_journal.py` into the guest.
The **SHM/upload-queue hooks and shared 16-submit sampler require a new matched Gamescope/proxy/APK CI build** because the
canonical event table is compiled into both processes. Normal Stop/Launch is
still required after re-arm so both workers recreate their ready markers for
the new cycle.

**One new matched APK/Gamescope build is required for these internal hooks.**
Do all heavy compilation in GitHub Actions. After these changes are committed
and pushed to `mali-gpu-experiment`, run **Build APK** (`build.yml`) at that ref:

```sh
gh workflow run build.yml --ref mali-gpu-experiment
```

Its `gamescope` job (**Build experimental Gamescope**, ARM runner) applies patch
0126 and produces `gamescope-patched`; its dependent `build` job (**Build APK**)
embeds that same binary, compiles the Android hooks and glibc proxy ICD, checks the diagnostic
marker in the embedded Gamescope and produces `droiddeck-apk`. Use both jobs from
the same SHA/run. The standalone `build-gamescope.yml` artifact alone lacks the
Android hooks. Other branches skip Gamescope source compilation and are not the
requested matched build. No PanVK workflow/build is needed.

Install the resulting APK using the same signing channel as the current phone
installation (the raw `droiddeck-apk` artifact uses the public test key). Normal
Launch stages its bundled Gamescope over `/usr/local/bin/gamescope` via
`SessionFiles.stage`; rootfs reinstall is unnecessary. Do not uninstall or erase
the current Steam/PanVK runtime. Assuming the APK artifact was downloaded into
`/tmp/droiddeck-frame-apk`, the install command for the existing test-key phone
is:

```sh
adb -s E68DS8U4IVYTKJXK install -r /tmp/droiddeck-frame-apk/app-release-unsigned.apk
```

Then, from the repository root:

1. Use normal **Stop**, then arm, then use the existing experimental
   **Mali Steam Launch**. Leave the proven PanVK client option enabled. Re-arm
   removes the old `armed`, `control`, `ready-*-gamescope` and `ready-*-android`
   markers before touching a fresh `armed`; previous `trace-*.jsonl` and
   `capture-*.json` files are preserved.

   ```sh
   python3 -B tools/panvk/frame_journal.py arm --serial E68DS8U4IVYTKJXK
   ```

2. Once Steam is open, check for **both** current-cycle `ready-PID-gamescope` and
   `ready-PID-android` files. Missing workers mean a stale/mismatched binary,
   startup without the arm marker, or shared-storage access failure; do not call
   an external-only capture internal evidence.

   The guest controller requires exactly one marker per role, valid positive
   PIDs, matching `version=1 pid=PID role=ROLE` contents, and marker modification
   times **at least as new as the current `armed` marker**. Equality is allowed
   for shared-storage timestamp rounding; stale-file removal on arm provides
   the cycle boundary. Malformed, older and duplicate markers fail explicitly.
   Only Gamescope's PID must exist in guest `/proc`. Android runs outside the
   Linux/PRoot guest, so its real native worker PID may be invisible there;
   guest `/proc` cannot establish Android liveness. Its fresh marker establishes
   readiness, while the exact selected Android trace output is required afterward.
   A worker dying after readiness cannot be ruled out by markers alone.

   ```sh
   python3 -B tools/panvk/frame_journal.py status --serial E68DS8U4IVYTKJXK
   ```

3. Keep the same page scrolling/animating until capture finishes. Use a fresh
   host output directory. The helper dynamically pushes the new controller;
   temporary `0xff` and the existing exact mixed-mask restore watchdog are
   automatic.

   ```fish
   set -l capture_dir (mktemp -d /tmp/steam-upload-sampled-XXXXXX)
   python3 -B tools/panvk/stage_phone_test.py \
     --serial E68DS8U4IVYTKJXK \
     --frame-diagnostics --perf-seconds 10 \
     --perf-output "$capture_dir" --run
   ```

4. Require `frame-journal-control`, two current-run journal files, both headers
   armed before start, `complete=true`, `dropped=0`, actual joined frames and
   `affinity-restored.ok=true` without errors. Run both analyzers above; preserve
   the existing live renderer proof. Compare individual same-frame import,
   render/submit/wait, publish-to-handoff, latch, release-queue/fence and reuse
   intervals. Boundary frames without a complete lifecycle stay partial.

   `frame-journal-control.ready` and `capture-RUN.json` identify the two selected
   PID/role workers and record `arm_mtime_ns`; the shared four-field native control
   format is unchanged. `finish()` requires exactly
   `trace-RUN-PID-gamescope.jsonl` and `trace-RUN-PID-android.jsonl` for those
   selected PIDs. It reports `expected_files`, `missing_files`,
   `unexpected_files` and `complete_pair_available`; two arbitrary same-run
   files cannot substitute for a missing Android output. File presence alone
   does not establish complete/lossless trace contents; use the analyzer.

   Analyze the new children, same-import and same-queue remainders, keeping the mandatory
   external renderer/affinity validation. Use the existing trace processor:

   ```fish
   python3 -B tools/panvk/analyze_steam_frames.py "$capture_dir" \
     --trace-processor /tmp/droiddeck-trace-processor \
     > "$capture_dir/external-summary.json"
   python3 -B tools/panvk/analyze_frame_journal.py "$capture_dir/frame-journal" \
     > "$capture_dir/upload-summary.json"
   ```

   Require new proxy/broker `stage_spans`, positive
   `upload_queue_decomposition.complete_selected_samples`, `remainder_samples`
   and `broker_complete_samples`. Both exact current-run footers must contain
   `"dropped":0,"complete":true`; `selected_queues` must be positive and
   `incomplete_selected_queues` must be investigated rather than counted as
   complete. `intentionally_unsampled_queues` should account for ordinary
   omitted detail and must not be treated as loss. Repeat a second 10-second
   moving capture in another fresh directory to check repeatability.
   The following fish-compatible check prints both quality records and fails
   on loss, missing joined publications, or no complete selected/broker samples:

   ```fish
   python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); d=s["upload_queue_decomposition"]; print(s["capture_quality"]); print({k:d[k] for k in ("selected_queues","intentionally_unsampled_queues","complete_selected_samples","broker_complete_samples","incomplete_selected_queues")}); assert s["lossless_complete_window"] and s["joined_frames"]>0 and not s["unmatched_publish_ids"]; assert d["complete_selected_samples"]>0 and d["broker_complete_samples"]>0' "$capture_dir/upload-summary.json"
   ```

   Inspect unassigned native coverage, per-stage sample counts and
   `span_diagnostics`. Unsampled attempts are expected to lack a detailed
   remainder. Compare inclusive stages only
   within this new capture. Do not subtract the old 13.22 ms copy mean from the
   new capture's import mean. The old raw PanVK artifact is reused unchanged.

5. Disarm after collection, even if capture fails, then normal **Stop** and
   **Launch** to verify ordinary restart. Traces remain for analysis; the arm
   marker/control are removed. No affinity policy, driver or client settings are
   changed by this cleanup.

   ```sh
   python3 -B tools/panvk/frame_journal.py disarm --serial E68DS8U4IVYTKJXK
   ```

Phone evidence from this build is required before attributing the bottleneck or
proposing optimization.

### Changed-file inventory

- Gamescope: `tools/gamescope/patches/0126-mali-frame-trace.patch`; `.gitattributes`
  exempts that unified diff's legitimate context whitespace only.
- Shared journal: `tools/mali-vulkan/frame_trace.c`, `frame_trace.h`.
- Android broker/registry: `app/src/main/cpp/malivulkan/system_broker.c`,
  `normal_commands.h`, `renderer_commands.h`, `normal_ownership.c`,
  `normal_ownership.h`.
- Android presentation: `app/src/main/cpp/waylandcomp/src/ahb_swapchain.c`,
  `ahb_swapchain.h`, `sc_layer.c`.
- Native build linkage: `app/src/main/cpp/CMakeLists.txt`,
  `app/src/main/cpp/waylandcomp/CMakeLists.txt`.
- Dynamic capture/control: `tools/mali-vulkan/steam_frame_journal.py`,
  `diagnose_steam_perf.py`; `tools/panvk/frame_journal.py`, `perf_capture.py`,
  `stage_phone_test.py`.
- Offline exact-ID analysis: `tools/panvk/analyze_frame_journal.py`.
- Lightweight tests/fixture linkage: `tools/mali-vulkan/test_frame_trace.py`,
  `test_normal.py`, `test_icd.py`, `test_normal_session.py`,
  `tests/frame_trace_host.c`, `tests/frame_trace_android.c`,
  `tests/android/hardware_buffer.h`, `tests/renderer_host.cpp`.
- SHM decomposition delta: the existing patch/canonical journal/analyzer/docs
  above, `tools/panvk/test_shm_decomposition.py`, and one Python-only CI test
  invocation in `build.yml`. Native broker/presentation behavior is untouched.
- CI and procedure: `.github/workflows/build.yml`, this document.

Initial upload-queue instrumentation delta: `tools/mali-vulkan/frame_trace_proxy.h`,
`icd_proxy.c`, `device_icd.h`, `submit_icd.h`, `interop_icd.h`, `renderer_icd.h`,
canonical `frame_trace.h/.c`, patch 0126, Android `system_broker.c` and
`interop_commands.h`, `tools/panvk/analyze_frame_journal.py`,
`tools/panvk/test_upload_queue.py`, `tools/mali-vulkan/test_upload_trace.py`,
this document and `.github/workflows/build.yml`. No renderer/protocol/sync
behavior changes. Targeted tests include the small extracted real rpc_exchange
harness (no Vulkan SDK/Gamescope build), journal/CPU/identity tests and nested
interval-union/ambiguous namespace/partial transaction tests.

Sampling/loss-control delta: `frame_trace.h/.c`, `frame_trace_proxy.h`,
`icd_proxy.c`, `device_icd.h`, `renderer_icd.h`, patch 0126, Android
`system_broker.c`/`interop_commands.h`, analyzer, `test_upload_queue.py`, new
Python-only `test_upload_sampling.py`, CI-only `test_upload_trace.py`, this
document and `build.yml`. Local validation runs Python model/source-contract,
join/decomposition/control tests and zero-fuzz patch application only. Native
harness execution/compilation and the matched Gamescope/proxy/APK build remain
in GitHub Actions. Journal locking and rendering/RPC/synchronization remain
unchanged. Phone zero-loss acceptance is pending.
