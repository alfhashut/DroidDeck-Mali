# C8B.1 client swap timing, 2026-10-11

The dominant measured cost **inside the client swap scope is off-CPU waiting**.
Read-only syscall observations identify the GPU thread polling `/dev/mali0` for
input while inside slow swaps. The pinned kbase implementation polls that device
in its job-atom completion wait. This points to the Mali completion/event path,
including wakeup and scheduling delay, rather than CPU copying or an identified
X11 socket stall. It does not establish pure hardware GPU execution duration.
It also does not exclude CPU work on other threads or dependencies preceding
the device wait.

No optimization, API interposition, debugger attach, binary replacement, PanVK
build, APK build, renderer flag change or Gamescope behavior change was made.
ANGLE Vulkan, the audited raw PanVK ICD and atom stride 56 remain in use.

## Same-run evidence

The real session was `2026-10-11-34-steam`: GPU webhelper PID/TID 1284,
browser webhelper 1094, VizCompositor TID 1362. Roles came from the installed
CEF's `SystemInfo.getProcessInfo`, not inherited zygote argv. The GPU's actual
maps include the Steam EGL/GLES/Vulkan libraries and audited PanVK library.
Existing renderer logs retain ANGLE Vulkan / Mali-G52 / PanVK evidence.

Each row below summarizes its own run. Runs are not added together.

| Capture | Slow swaps | Mean swap wall | Mean swap thread CPU | Mean swap off-CPU |
| --- | ---: | ---: | ---: | ---: |
| Five-second follow-up `/tmp/steam-swap-probe-2` | 105 | 31.43 ms | 5.14 ms | 26.29 ms |
| Ten-second `/tmp/steam-swap-final` | 151 | 34.93 ms | 5.07 ms | 29.85 ms |
| Five-second clock check `/tmp/steam-swap-clock-check` | 103 | 29.77 ms | 5.29 ms | 24.48 ms |

"Slow" means a completed `SkiaOutputSurfaceImplOnGpu::SwapBuffers` scope of at
least 20 ms. The final two captures exclude scopes straddling sample boundaries,
including clock uncertainty. All three traces completed without reported data
loss or a diagnostic event-cap hit. CPU comes from each exact trace scope's
`tdur`; off-CPU is that same scope's `dur - tdur`. These are not process-wide CPU
averages or sums of nested timings. Approximately 82–85% of slow-swap wall time
was off-CPU in these captures.

In the ten-second capture, 70 blocked-state observations inside slow swaps
identified `ppoll` with a single `/dev/mali0` descriptor requesting `POLLIN`.
These observations occurred in 47 of the 151 slow swaps. Another 22 `ppoll`
observations had no stable descriptor attribution; 18 other blocked observations
were also unclassified. No identified X11-socket wait was observed inside those
swaps. This is sampled evidence, not proof that every off-CPU interval has the
same cause. Counts are not converted to exact syscall durations.

Scheduler delay is material and must remain part of the interpretation. Across
the ten-second sampling interval, the GPU main thread accumulated 2.86 seconds
of CPU runtime and 2.54 seconds of runqueue delay. Those whole-interval counters
also include work outside swap and must not be allocated wholesale to swaps.
The earlier clock check printed per-swap completed-counter brackets with means
1.54–16.30 ms. Those brackets did not account for pending runqueue waits crossing
swap boundaries and must not be treated as conservative wakeup-delay bounds.
The [affinity experiment](STEAM_AFFINITY.md) documents corrected, wider bounds.
Neither capture supports a precise split between device sleep and wakeup delay.

The artifact's `BUILD_INFO.txt` pins source commit
`efd07bab401b263a9ac114560ecdb9a2934f5d43`. Its
[`kbase_wait_atom` implementation](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/src/panfrost/lib/kmod/kbase_kmod.c#L2378)
checks atom completion, polls the device for input, then consumes completion
events. This source corroborates the observed device-wait interpretation;
the sampler does not capture a native call stack proving the exact caller of
every poll. No source file or driver configuration was changed.

## API visibility and limitations

The diagnostic queries the **installed CEF's** trace categories and enables
available GPU/ANGLE/viz categories only. `gpu.angle` exists in this build, but
the captures did not emit separately timed EGL swap, Vulkan wait/present, or
Xlib image/flush/sync entry points. The actual emitted scopes were:

- `SkiaOutputSurfaceImplOnGpu::SwapBuffers` and `SkiaRenderer::SwapBuffers`.
- `VulkanQueueSubmitHook`, a CEF hook scope, not a hardware GPU timer or an
  independently measured exact driver-entry boundary.

Absent `eglSwapBuffers`, `vkWaitForFences`, `vkWaitSemaphores`,
`vkQueuePresentKHR`, `XSync`, `XFlush`, `XPutImage`, `XShmPutImage`, `XGetImage`
and `XCopyArea` events are **uninstrumented**, not zero-cost. Installed library
string checks are reported separately; finding an API name does not establish
an active tracing hook. No extra Vulkan timestamp queries or waits are injected.

There was no named GLComposer wait scope in these captures. This does not rule
out a dependency introduced by Steam's GLComposer path. The helper also lists
process thread names and samples discovered GLComposer/CCompositorGL threads,
plus Steam/browser/GPU main threads and VizCompositor where present. That
thread-name coverage is a fallback, not proof that an unnamed thread is absent.

ADB shell could not read syscall state or collect perf events/uprobes. The
same-UID in-runtime diagnostic could read syscall state, wchan and scheduler
counters. It uses cached read-only proc handles, with 5 ms requested sampling.
Actual gaps are reported: the ten-second capture had gaps up to about 196 ms
during other sampling work. A zero wchan is inconclusive and is sampled again.

To classify a `ppoll`, the helper reads at most eight `pollfd` structs (64 bytes)
from the observed argument address through a read-only proc memory handle.
It then checks descriptor links and rechecks the syscall arguments. Changed
arguments discard descriptor attribution. Two matching snapshots do not prove
continuous blocking. No process memory is written and no ptrace attach occurs.
If proc memory is denied, descriptor details remain unavailable. Linux
UNIX_DIAG peer discovery was denied on this phone, so unidentified sockets are
not labelled X11 sockets. Permission failures remain visible.

## Correlation

The historical `slow-swap` records contain exact trace start/end, thread CPU/off-CPU,
conservative scheduler bounds, observations inside that scope, nested available
API scopes, and candidate X11 update timestamps by window. Nested scopes use
interval unions; overlapping timings are not summed as exclusive costs.

Trace time is calibrated against phone monotonic time using three clock-sync
markers, retaining the smallest request/response bracket. The final clock check
had approximately ±1.23 ms uncertainty. X server time is independently read
through the XSync `SERVERTIME` counter without creating or changing a window;
its query bracket and millisecond quantization are included in correlation.
An overlapping X11 update is a timestamp candidate, **not proof of frame identity**.

Exact per-frame Gamescope import/upload and Android release timestamps are
unavailable in the current build. Its session log again showed zero appended
bytes during the ten-second capture, so old SHM timings are not attached to new
swaps. Native Android logs provide timestamped cumulative release counters in
roughly two-second windows, not individual release callback times. They are
saved alongside the trace without fabricating a same-frame join. Adding precise
import/release IDs and timestamps would require Gamescope/native instrumentation
in a future build; it is not necessary for the present CPU-versus-wait finding.

The JIT-init warning remains visible in historical client logs. No new matching
warning was observed during these client-log intervals, and it is not used to
explain the measured slowdown.

## No-rebuild rerun

Keep the proven accelerated Steam UI open and navigate during capture. From the
repository root, use a new laptop output directory:

```sh
python3 -B tools/panvk/stage_phone_test.py \
  --serial E68DS8U4IVYTKJXK --swap-diagnostics --perf-seconds 10 \
  --perf-output /tmp/droiddeck-steam-swap-next --run
```

The dynamically pushed helper uses the existing development receiver. It saves
`guest-report.txt`, `native-cpu.jsonl`, and `native-logcat.txt` on the laptop and
retains the UUID-labelled report on the phone. No Steam restart is needed.
Tracing is stopped in cleanup; a rejected trace start does not stop someone
else's trace. Proc handles are closed. This mode cannot substitute for the
independent hardware-rendering acceptance test. Normal startup/Stop is unchanged.
The current helper emits `swap-sample` for every completed in-window swap, with
a `slow` flag, and includes CEF frame-production categories for affinity A/B.
