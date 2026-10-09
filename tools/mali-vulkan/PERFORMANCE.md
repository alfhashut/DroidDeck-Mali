# Checkpoint 7P: normal Mali session measurements

The user reports 5–6 FPS, occasionally about 10 FPS, in the phone-proven normal
Launch path. A primary per-frame bottleneck and a performance gain have **not**
yet been established with this instrumentation. These are source findings:

- Every renderer/command-recording RPC waits synchronously for a broker reply,
  including void Vulkan commands. Commands have not been batched or made async.
- Before each renderer queue submission, the ICD sends **all** persistently
  mapped coherent memory in 4096-byte `MB_MEMORY_WRITE` chunks. The renderer's
  512 KiB upload allocation alone contributes 128 round trips per submission.
  This is a calculated contribution, not the measured total RPCs per frame.
  SHM source upload, mapping reads, other allocations and command calls add work.
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

## Reports and interpretation

Reports appear every two seconds while the normal loop runs, including idle or
pool-blocked windows. Fixed-size counters collect every attempt, with bounded
samples for p95 (256 samples; 60 normally fit at the 30 Hz target). No command
trace, per-frame printing, allocations or extra per-command RPCs are added.
The existing STATS request now occurs at the reporting interval. The proxy's
private `vkDroidDeckPerformanceMALI` snapshot only copies/resets local counters
under the existing mutex. Wire v7 and opcodes 83–91 are unchanged; this does not
advertise a new Vulkan extension.

`session.log` has four `MaliPerf` lines per reporting window:

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

For an instrumented baseline using the original pacing/upload ordering, add
this line to the existing `/sdcard/Download/droiddeck-env` settings file:

```text
MALI_VULKAN_PERF_BASELINE=1
```

Only this key with value 0 or 1 is accepted into the isolated Mali environment.
Restart through normal Launch. Remove that line or set it to 0, then repeat
under the same conditions for the two safe optimizations. The `MaliPerf mode`
line confirms the mode. This flag does not change synchronization, ownership,
capability reporting or diagnostics. No device file was modified by this work.

Cheap local checks, with no project compilation:

```sh
cd tools/mali-vulkan
python3 -B -m unittest -v test_perf test_normal.NormalOwnershipTests.test_android_owned_release_timeout_and_restart
```

These build only a tiny mocked-clock metric harness and the standalone ownership
registry with Android buffer stubs. They verify accounting, report cadence,
p95, pool protection, timeout/release accounting and header/patch parity. They
do not establish phone FPS, GPU time, full Vulkan integration or Android timing.
