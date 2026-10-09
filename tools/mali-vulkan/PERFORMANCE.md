# Checkpoint 7P: normal Mali session measurements

Phone profiling supplied for CP7P identifies mapped-memory RPC traffic as the
first major bottleneck: **416 RPCs/frame**, two queue submissions/frame. The
Android compositor reported 93 GPU/zero-copy frames in ten seconds (9.3 FPS),
about 1.05 ms average `render_scene`, no copy path and no pool drops. A 15-frame
window had 6240 RPCs, 542.350 ms broker service, 1435.472 ms broker thread CPU,
29.657 ms across 30 queue submissions and 117.509 ms across 60 timeline waits.
These are the user's phone measurements before the bulk-write change.

The subsequent normal-Launch phone A/B validates bulk mapped writes: steady
windows now have **exactly 106 RPCs/frame** (4558/43, 4770/45, 4346/41 and
4240/40), down 74.5% from 416. Performance is about **20–22 FPS**, roughly 3.5×
the ~6 FPS baseline. The compositor reported 215 GPU/zero-copy frames in ten
seconds (21.5 FPS, ~1.11 ms scene average) and another 200 in ten seconds
(20 FPS, ~1.35 ms), with no pool drops. This confirms the write optimization;
it does not identify the distribution or RTT cost of the remaining 106 calls.
The next change adds opcode/category reports only, without further batching
or changes to synchronization or presentation. Source audit:

- Every renderer/command-recording RPC waits synchronously for a broker reply,
  including void Vulkan commands. Commands have not been batched or made async.
- Before the bulk-write change, each renderer queue submission sent **all**
  persistently mapped coherent memory in 4096-byte `MB_MEMORY_WRITE` chunks.
  The 512 KiB upload allocation alone contributed 128 round trips/submission,
  256/frame, about 61.5% of the measured 416. SHM source upload, mapping reads,
  other allocations and command calls contribute the remaining work.
- SHM source textures are imported each committed frame; fences and sync tokens
  are created/closed each frame. The three AHB outputs, connection, upload buffer,
  renderer, pipeline and descriptor infrastructure persist. Resource counts in
  the reports expose this churn; reuse needs a separate correctness review.
  Freed proxy wrapper tombstones remain in the connection's resource list;
  handle lookups/submission scan that list. Longer-run host CPU measurements
  should check this cost before attempting safe wrapper reclamation.
- Routine output readbacks, pixel checks, selected-frame hashes and command logs
  are already disabled in normal mode. Diagnostic paths retain them. The two
  timeline waits (completion and command retirement), real producer SYNC_FD wait,
  publication validation and final teardown drain are unchanged. The native
  publication's second sync check returns immediately once completed.
- No explicit per-frame idle call or AHB allocation appears in the normal runner.
  The renderer can drain when its upload ring wraps; `idle-RPCs` measures this.

Two safe changes are enabled by default: check for a FREE output before SHM
import/upload, and pace from frame start rather than adding 33.33 ms after frame
work. A full pool still waits for real release; it never advances ownership.
The pacing correction removes an avoidable interval when rendering is slow.
It cannot by itself demonstrate or promise a particular FPS gain.

Read-only ADB inspection of the existing phone APK's exported
`2026-10-10-01-mali-wayland/session.log` confirmed 1560×720 output, a last
reported session-average FPS of 6.00, 1111 rendered/committed frames, 1108
actual Android presentations/releases, three commits cancelled before display,
1111 exported/closed FDs, zero release timeout and clean exit. Periodic samples
usually showed two Android-owned buffers, no drops, no pending commands and
no open sync FDs after each frame. Those observations do not identify GPU/RPC
time or rule out short pool stalls: the old build samples after its waits.
No APK was installed and no phone configuration/session was changed.

## Bulk mapped-memory writes

Old path: `renderer_QueueSubmit()` walks coherent mappings, calls
`interop_copy_mapping()`, then `interop_rpc()` for each 4 KiB chunk. Opcode
**41 (`MB_MEMORY_WRITE`)** reaches `native_interop_command()`, which validates
and copies into the real mapped allocation before replying. The proxy waits
for every reply before issuing opcode 76 (`MB_RENDERER_SUBMIT`).

The new path keeps opcode 41, wire v7, little-endian schema and zero-payload
acknowledgment. Its payload is `device:u32, memory:u32, offset:u64, length:u32,
length bytes`. Writes use up to **512 KiB data/message** (524308-byte request),
within the existing 524544-byte broker request buffer. This bounds messages,
not allocations or mapped ranges. A range larger than 512 KiB is split into
`ceil(length/524288)` acknowledged writes, including its final partial chunk.
Reads and wire-v6 writes retain the existing 4 KiB bound. Old v7 4 KiB writes
remain valid, including Checkpoint 6 callers. Use matching proxy/broker assets;
an older broker will reject larger writes, rather than submit incomplete data.

`interop_upload_mapping()` builds the full request directly in a reusable
per-device heap buffer. The connection mutex protects buffer growth, filling,
all sends and acknowledgment checks for the range. The buffer is bounded to
512 KiB + 20 bytes and freed by both existing device/instance cleanup paths.
There is no per-chunk allocation, large stack allocation or fire-and-forget
operation. All coherent ranges are still uploaded in full, including unchanged
bytes; dirty tracking and readback transport are outside this optimization.

The native handler checks host visibility, pending GPU use, AHB exclusion,
allocation bounds, mapped bounds, nonzero length, the version-specific maximum,
and exact payload length before copying. Subtraction-based range checks avoid
overflow; `length == message_bytes - 20` avoids unchecked header+length sums.
Existing Map/Unmap/Flush/Invalidate semantics remain. Success acknowledges the
completed native copy. Missing/malformed acknowledgments disconnect the stream;
allocation, native or socket errors stop uploading and return before submission.
If a later chunk fails, earlier writes may exist in mapped memory, but that
queue submission never executes. No synchronization or output ownership changes.

| Known persistent upload buffer | Before | Expected after |
| --- | --- | --- |
| Writes per 512 KiB range/submission | 128 | 1 |
| Writes for two submissions/frame | 256 | 2 |
| Total RPCs/frame, changing only that contribution | 416 | 162 |

This table was the first estimate, changing only the persistent allocation.
Other mapped writes also collapse: the phone-proven actual total is **106**.
Upload bytes remain unchanged for the same workload. Existing CP7P counters
still observe opcode 41 through the same measured `rpc()` wrapper and retain
write calls/frame, bytes/frame, service/CPU and FPS.

## Reports and interpretation

Reports appear every two seconds while the normal loop runs, including idle or
pool-blocked windows. Fixed-size counters collect every attempt, with bounded
samples for p95 (256 samples; 60 normally fit at the 30 Hz target). No command
trace, per-frame printing, allocations or extra per-command RPCs are added.
The existing STATS request now occurs at the reporting interval. The proxy's
private `vkDroidDeckPerformanceMALI` snapshot only copies/resets local counters
under the existing mutex. Wire v7 and opcodes 83–91 are unchanged; this does not
advertise a new Vulkan extension.

`session.log` retains these four `MaliPerf` lines per reporting window:

| Report | Meaning |
| --- | --- |
| `frame` | Window committed/Android FPS, attempts, average/p95 frame work and completion cadence, missed deadlines, last frame's work/RPCs/time. Work covers SHM import, render, GPU waits, publish and source cleanup. Cadence additionally includes time between frames. |
| `stages` | Average CPU wall time per attempt for SHM import/upload, compositor, pool acquisition, submit/timeline/export/wait/publish RPCs, and Wayland commit. Release acknowledgment lifetime is measured separately from commit to actual acknowledgment. |
| `RPC` | RPCs per committed frame, count/time/average/worst latency and opcode, acknowledged upload bytes and write calls per frame, recorded copies/compute BLITs, read/idle/AHB/fence/image/allocation counts. Includes source texture cleanup. |
| `budget window` | Main thread CPU execution/off-CPU wall time, plus a separate partition into blocking timeline/sync RPCs, other RPCs, and host time outside RPCs. Time outside frame attempts includes pacing, client callbacks and event processing. Output backpressure overlaps that time. Time-weighted sampled pool occupancy and Android-owned counts, pending GPU commands, live FDs, three most expensive RPC opcodes. |

Pool occupancy is sampled at event-loop ticks, not a continuous GPU trace.
Guest Android-owned average/peak and current native-owned count are labelled
separately: real release can precede delivery of its Wayland acknowledgment.
Stage times overlap RPC totals and must **not** be added to them. RPC time is
round-trip wall time, excluding caller-side encoding/handle lookup and mutex
acquisition; it includes broker execution, scheduling and socket waits. Host
non-RPC time is a residual wall time, **not** measured CPU utilization.
Main thread CPU uses `CLOCK_THREAD_CPUTIME_ID` twice per frame; off-CPU includes
waiting, scheduling and execution in the broker/proot/other processes. These
CPU/off-CPU values overlap the RPC partition. They do not measure GPU execution.
Copies/BLITs count recorded operations, not bytes transferred by the GPU.
Missed frames exceed 33.33 ms work, or 34.33 ms cadence (1 ms jitter allowance).

### Client opcode and category breakdown

Each two-second report now also prints an `opcode-window` header, one `opcode`
row for each opcode actually called, eight `category` rows (including zero
buckets), and `RPC leaders`. The existing proxy timer surrounds the entire
`rpc_exchange()`: request header/payload sends, reply header/payload receives,
and reply validation/result decoding. It includes failed calls and the complete
synchronous wait paid by Gamescope, rather than native broker `service=` time.
No timers or work were added to individual calls. Formatting and classification
run only at the existing aggregate boundary, using fixed-size local storage.

Accounting scopes are explicit:

- `count` and `count-pct` cover all calls in the normal loop window, including
  periodic STATS and other calls outside frame attempts. Initialization before
  the first loop tick and shutdown after the loop are excluded.
- `frame-count` covers the same completed frame attempts as the existing `RPC`
  line. `frame-RPCs/frame` divides that count by committed frames, so these rows
  reconcile to the phone's 106 RPCs/frame. Failed attempts remain included;
  with zero committed frames the divisor is one, as in the existing report.
- `outside-count` covers calls between frame attempts. A local snapshot at
  frame begin or report time drains each call exactly once. Reporting STATS
  is normally one extra call/window; it never inflates the frame RPC count.
- `RTT-total`, `RTT-avg` and `RTT-max` are window client round-trip milliseconds.
  Category `RTT-pct` divides category RTT by the entire window's RPC RTT.
  Categories partition calls without overlap: counts/times sum to the opcode
  totals. Category average is weighted by calls; maximum is the greatest
  individual RTT, not a sum of opcode maxima. RTT includes native execution,
  synchronization, transport and scheduling; it is not CPU or GPU execution.
- `RPC leaders` names the largest in-frame opcode by count, the largest opcode
  and category by cumulative window RTT, and command recording's RTT share.
  Greatest count and greatest time need not identify the same opcode. Empty
  windows report zero percentages and `none` leaders; no stale opcode rows.

Example from the **mocked unit test**, not phone measurements (other rows omitted):

```text
MaliPerf opcode-window: scope=normal-loop frames=2 attempts=2 count=13 frame-count=10 outside-count=3 frame-RPCs/frame=5.00 client-RTT=22.000ms (frame counts match existing RPC report; includes failed calls)
MaliPerf opcode: op=41 name=MB_MEMORY_WRITE category=mapped-memory-transfer count=4 count-pct=30.77 frame-count=4 outside-count=0 frame-RPCs/frame=2.00 RTT-total=4.000ms RTT-avg=1.000ms RTT-max=1.500ms
MaliPerf category: name=timeline-synchronization count=3 count-pct=23.08 frame-count=2 outside-count=1 frame-RPCs/frame=1.00 RTT-total=9.000ms RTT-pct=40.91 RTT-avg=3.000ms RTT-max=3.000ms
MaliPerf RPC leaders: most-frame-count-op=MB_MEMORY_WRITE largest-RTT-op=MB_RENDERER_WAIT largest-RTT-category=timeline-synchronization command-recording-RTT-pct=18.18
```

The name/category table covers every existing opcode, checked against protocol
headers without compiling Vulkan. Classification is by opcode, not payload:

| Category | Opcodes |
| --- | --- |
| mapped-memory-transfer | 38–43: map/unmap/read/write/flush/invalidate |
| command-recording | 21–22, 26, 48–51, 73–75, 77, 79–81: begin/end/reset, events, binds, dispatch, barriers, copies, clears/fills |
| descriptor-resource-updates | 37, 47, 70: memory binds and descriptor updates |
| queue-submit | 31, 76 |
| timeline-synchronization | 25, 29–30, 55–57, 62–63, 82: status/waits, SYNC_FD export/wait/close, timeline counter/wait, idle |
| allocation-resource-management | 2–3, 14–20, 23–24, 27–28, 32–36, 44–46, 54, 60–61, 64–69, 71–72, 78: create/allocate/free/destroy, requirements, queue acquisition |
| AHB-presentation | 52–53, 58–59, 83–91: AHB and diagnostic/normal output-session lifecycle/publication/stats |
| other | 1, 4–13: inventory/capability queries; unused opcode slot 0 is UNKNOWN |

Generic opcode 61 destroys several resource kinds; all are counted as resource
management. Semaphore/fence/event creation is also management, rather than a
wait. AHB creation/release belongs to AHB/presentation. These are accounting
labels, with no change to wire definitions or Vulkan behavior.

Command recording still uses synchronous per-call replies. It is **not yet
established as the next bottleneck**. Coherent `MapMemory`/invalidate also read
back mapped ranges through opcode 40 in 4 KiB chunks; the write optimization
did not change that. Inspect the next steady phone reports for actual opcode
counts and category RTT before choosing another optimization. No exact
decomposition of the 106 calls can be inferred from submit totals alone.

`app.log` / the `MaliVulkanBroker` logcat tag has matching native summaries:

- `native`: broker service totals, worst opcode, actual `vkQueueSubmit`,
  `vkWaitSemaphoresKHR`, `vkGetFenceFdKHR` and sync wait times/call counts.
  Broker thread CPU is sampled per window, including reply/event-loop work.
  Service excludes idle header wait/reply write and includes payload read and
  validation. Native calls are subsets of service time. The first window also
  includes output initialization; ignore warm-up when comparing steady state.
- `Android`: SurfaceControl transaction enqueue CPU time, publication-to-submit
  latency and submitted-to-real-release lifetime totals. These are asynchronous
  observations, not GPU timestamps, scanout completion or CPU blocking time.
  Releases can fall in a later window than their submissions.
- `held`: at most three held buffer keys, frame, sync/fence and age per window.
- `teardown`: actual bounded Android release wait duration and timeout result.

If write RPC time dominates and native service is small, transport/scheduling is
the leading candidate. A large native timeline wait instead suggests GPU work
or its dependencies. A full pool plus large backpressure/held age suggests the
Android release/presentation path. Compare steady windows from the same run;
do not infer an exact transport time by subtracting mismatched windows.

## Phone validation

CI must build the matching proxy, broker, shared AHB registry/compositor and
patched Gamescope. Do not combine new Gamescope with an older proxy. No APK,
Gamescope, cross build or full suite was run locally for this change.

After installing that CI APK, use normal Launch at a fixed resolution. Keep
the screen visible and compare several steady windows after warm-up, including
input, Stop and restart. Capture both session and app logs with Share logs.
Check balanced FD/resources, no release timeout and no output read/idle/AHB
creation in steady windows before interpreting performance.
For the new profiling, verify opcode/category counts and RTT sums, in-frame
calls still near 106/frame, and separate outside-frame STATS. Compare leaders
and command-recording RTT share across several steady windows; retain broker
service/CPU and FPS alongside them. This report-format change still requires
CI/phone validation; it has not been measured on the phone locally.

For an instrumented baseline using the original pacing/upload ordering, add
this line to the existing `/sdcard/Download/droiddeck-env` settings file:

```text
MALI_VULKAN_PERF_BASELINE=1
```

Only this key with value 0 or 1 is accepted into the isolated Mali environment.
Restart through normal Launch. Remove that line or set it to 0, then repeat
under the same conditions for the two safe optimizations. The `MaliPerf mode`
line confirms the mode. This flag does not change synchronization, ownership,
capability reporting or diagnostics, and does not revert bulk mapped writes.
No device file was modified by this work.

Cheap local checks, with no project compilation:

```sh
cd tools/mali-vulkan
python3 -B -m unittest -v test_bulk test_perf test_normal.NormalOwnershipTests.test_android_owned_release_timeout_and_restart
```

These build only a tiny mocked-clock metric harness and the standalone ownership
registry with Android buffer stubs. They verify accounting, report cadence,
p95, all opcode names/categories, complete client timer accounting (including
failed calls), category/count/RTT reconciliation, average/max/percentages,
outside-frame STATS isolation, idle/reset behavior, pool protection,
timeout/release accounting and header/patch parity. They
do not establish phone FPS, GPU time, full Vulkan integration or Android timing.
`test_bulk` additionally compiles only extracted transfer, native memory-switch,
queue-submit, cleanup and metric helpers with in-memory stubs. It checks exact
bytes, request reuse, multiple coherent ranges/submissions, partial chunks,
upload/error ordering, malformed acknowledgments, offsets/overflow, allocation
failure, native host-visible/pending/AHB guards and legacy read/write bounds.
