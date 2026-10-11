# Development Steam → Gamescope → Android tracing

The dynamic capture is implemented and phone-tested. **It does not yet provide
the private Gamescope stages or a shared Gamescope/AHB/Android frame identity.**
The exact cause of the earlier 21–22 CEF → ~18 Android FPS limit remains
unmeasured. Missing stages are emitted as unavailable, not inferred timings.

No PanVK, Gamescope, native bridge, APK, Steam-owned file, renderer selection,
capability, or permanent scheduling changes are part of this diagnostic.

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
