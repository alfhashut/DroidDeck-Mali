# Development Steam → Gamescope → Android tracing

The external dynamic capture is implemented and phone-tested. Its original
captures do not provide private Gamescope stages or a shared frame identity.
The opt-in internal journal below adds those hooks and awaits CI/phone validation.
The exact cause of the earlier 21–22 CEF → ~18 Android FPS limit remains
unmeasured. Missing stages are emitted as unavailable, not inferred timings.

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

## Internal frame journal: development implementation, awaiting CI/phone evidence

The external-only limitations above describe the previous capture. Patch
`0126-mali-frame-trace.patch` and the Android hooks now add an **opt-in internal
journal**. This code has host validation only. No new phone measurement has been
made, and neither the earlier 22→18 gap nor its cause is established.

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

When armed, each process allocates 16,384 fixed records (about 1.4 MiB), checks
control every 100 ms, and records into RAM with a nonblocking try-lock. It never
waits behind another observer: contention/capacity loss increments `dropped`.
The worker serializes JSONL **after the recording window**, independently of
stdout/session.log and its discard logger. Default CPU sampling is every eighth
attempt/frame/fence/timeline value. Wall timestamps are recorded for every
accepted event. Compare an unarmed external capture of the same workload if
observer overhead appears material; instrumentation overhead is not yet phone
quantified.

The guest `steam_frame_journal.py` controller runs automatically during
`--frame-diagnostics` if armed. It requires exactly one live ready worker of each
role. It writes one shared `run start_ns end_ns cpu_stride` control atomically,
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

Run both analyzers on the same capture. The original analyzer retains mandatory
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
without an output key/fence remain unassigned; no hardware GPU execution clock
or exact kernel fence signal/wakeup timestamp is added; physical Android
scanout and repeated panel refreshes lack a shared ID. SF latency histories
remain independently measured timestamp evidence, with ambiguous matches left
ambiguous. Unselected/unmapped commits, coalescing, pool pressure, application
repeats and per-frame ownership lifetimes become observable. Keep fresh
`KBASE_IOCTL_MEM_JIT_INIT`, KFAULT and GLX/Composite warning capture unchanged;
repeated warnings remain evidence, not an assumed cause.

### Exact CI build and phone procedure

**One new matched APK/Gamescope build is required for these internal hooks.**
Do all heavy compilation in GitHub Actions. After these changes are committed
and pushed to `mali-gpu-experiment`, run **Build APK** (`build.yml`) at that ref:

```sh
gh workflow run build.yml --ref mali-gpu-experiment
```

Its `gamescope` job (**Build experimental Gamescope**, ARM runner) applies patch
0126 and produces `gamescope-patched`; its dependent `build` job (**Build APK**)
embeds that same binary, compiles the Android hooks, checks the diagnostic
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

1. Arm before launching; use normal **Stop**, then the existing experimental
   **Mali Steam Launch**. Leave the proven PanVK client option enabled.

   ```sh
   python3 -B tools/panvk/frame_journal.py arm --serial E68DS8U4IVYTKJXK
   ```

2. Once Steam is open, check for **both** live `ready-PID-gamescope` and
   `ready-PID-android` files. Missing workers mean a stale/mismatched binary,
   startup without the arm marker, or shared-storage access failure; do not call
   an external-only capture internal evidence.

   ```sh
   python3 -B tools/panvk/frame_journal.py status --serial E68DS8U4IVYTKJXK
   ```

3. Keep the same page scrolling/animating until capture finishes. Use a fresh
   host output directory. The helper dynamically pushes the new controller;
   temporary `0xff` and the existing exact mixed-mask restore watchdog are
   automatic.

   ```sh
   python3 -B tools/panvk/stage_phone_test.py \
     --serial E68DS8U4IVYTKJXK \
     --frame-diagnostics --perf-seconds 10 \
     --perf-output /tmp/steam-frame-internal --run
   ```

4. Require `frame-journal-control`, two current-run journal files, both headers
   armed before start, `complete=true`, `dropped=0`, actual joined frames and
   `affinity-restored.ok=true` without errors. Run both analyzers above; preserve
   the existing live renderer proof. Compare individual same-frame import,
   render/submit/wait, publish-to-handoff, latch, release-queue/fence and reuse
   intervals. Boundary frames without a complete lifecycle stay partial.

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
- CI and procedure: `.github/workflows/build.yml`, this document.
