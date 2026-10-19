# C8B.1 live Steam performance capture

Follow-up: [client swap CPU/wait split](STEAM_SWAP_TIMING.md) identifies a
predominantly off-CPU swap scope with observed Mali-device completion/event
polling. It keeps the missing per-frame Gamescope/Android correlation explicit.

Development diagnostics only. The proven `-cef-use-vulkan`, client-only
`PANVK_ATOM_STRIDE=56`, audited ICD, ANGLE libraries, and Gamescope/Android
Vulkan path are unchanged. No optimization, PanVK build, or APK build was made.

The strongest live evidence places the slowdown in the **client swap/presentation
path before Android handoff**. CEF itself produces only about 17–20 frames/s.
Its largest measured scope is `SkiaOutputSurfaceImplOnGpu::SwapBuffers`, averaging
31–33 ms in the longer captures and 46 ms in the final five-second smoke check.
Those are thread wall-time scopes, not GPU execution timings. They do not yet
separate GPU completion, readback/copy, or X11 backpressure. An exact attribution
to one of those operations would exceed the available evidence.

## Measurements on 2026-10-11

Active session: `2026-10-11-32-steam`. App PID 24482; Gamescope 28513;
Xwayland 28631; Steam 28878; main webhelper 28945; trace-identified GPU process
29077; main UI renderer 29146. The user kept the real accelerated Steam UI open.

| Stage | 20:06:34–20:07:00 capture | 20:11:37–20:12:03 capture |
| --- | --- | --- |
| CEF UI `MainFrame.SubmitCompositorFrame` | 17.25 events/s | approximately 20.21 events/s |
| CEF GPU `SwapBuffers` | 17.48 events/s; 33.16 ms mean scope | approximately 20.20 events/s; 30.78 ms mean scope |
| Selected X11 window `0x2a00034` | 396 update batches / 25.16 s = 15.74/s | 467 / 25.19 s = 18.54/s |
| Gamescope ingestion | Exact live wlroots commit/import count unavailable | Exact live wlroots commit/import count unavailable |
| Gamescope native submissions | Upload/render calls visible in broker logs; no separate live per-frame ingestion counter | Two queue submissions per output frame in existing path; native queue-call mean 0.77 ms |
| Android buffer handoff | 15.68/s | 18.49/s |
| Android release callback | 15.64/s | 18.49/s |

CEF rates use `(count - 1) / (last trace timestamp - first trace timestamp)` for
each specific event stream. Names/processes are never added together as frames.
The 20:06 trace completed with `dataLossOccurred=false`. The 20:11 trace retained
events spanning the capture but timed out draining completion; its CEF figures
are provisional. The final five-second smoke check completed without data loss.
The helper now allows 18 seconds for draining instead of eight. A first four-MiB
trace wrapped; its counts divided by the whole capture would have understated
production. Reports expose retained spans, completion status and loss explicitly.

Android rates above use cumulative counter differences between native log
timestamps inside the capture. `presented-total` means initial buffer handoff
to an Android surface transaction. It is **not physical scanout feedback**.
Release is the real buffer-release callback. Their similar rates and ownership
of only one or two buffers provide no evidence of an accumulating Android queue.

CPU in the 20:11 capture, with **one core = 100%**:

| Process/thread | Mean CPU |
| --- | ---: |
| Main steamwebhelper 28945 | 190.0% |
| GPU webhelper 29077 | 95.9% |
| UI renderer webhelper 29146 | 38.1% |
| Steam | 12.0% |
| Xwayland | 31.0% |
| Gamescope | 33.2% |
| Session proot | 3.7% |
| Sampler proot | 1.5% |
| Android app, total | 38.2% |
| Broker TID 28538 (`session-start`), included in app total | 15.6% |

Guest process means are weighted by sample duration. Native app/thread means
use the 24 samples within the capture. The app total includes its threads;
adding the broker to it would double-count CPU. Trace overhead is not subtracted.
The helper also reports its own CPU in subsequent captures.

Native Gamescope/Android Vulkan timeline wait averaged 4.45 ms/call and Android
transaction enqueue 0.88 ms/call in that capture. Native queue-call duration
excludes other proxy/transport work. These are **not client PanVK timings**.
Current noninvasive counters expose no PanVK GPU submission-to-completion timer.

## Earlier Gamescope cost evidence, separate from the live capture

The session log stopped growing at 19:41, before the captures above. The sampler
reports its unchanged size and zero new bytes instead of treating its tail as
fresh. Gamescope stdout/stderr still point to a pipe; the helper never reads
that pipe or interferes with its consumer.

The last three recorded Gamescope windows show 19.4–20.5 committed FPS:

| Measured work per attempt | Range |
| --- | ---: |
| Whole frame work | 32.8–35.9 ms |
| SHM import/upload | 22.9–24.6 ms |
| SHM memcpy/conversion, included above | 9.3–9.8 ms |
| SHM submit scope, included above | 11.5–12.2 ms |
| Compositor CPU scope | 3.1–4.0 ms |
| Output timeline RPC | 4.8–5.3 ms |

Each 1280×800 SHM frame transfers 4,096,000 bytes through eight staging writes;
the compositor uniform write adds 584 bytes. Existing staging slots are reused.
SHM import/upload is the largest recorded Gamescope work stage. These older
numbers and the live client swap scope must not be summed: they were measured
at different times, on different pipeline stages, with potentially overlapping
work. They establish another substantial copy/transport cost, not its share of
the later capture's frame period.

## Window, warning and frame correlation

The selected `0x2a00034` remains viewable (`map_state=2`), 1280×800, depth 32,
visual `0x214`. Two other viewable full-size webhelper windows, `0x1600005` and
`0x1600006`, update somewhat faster; their counts are not added to the selected
window. Hidden 1×1 windows remain unmapped/unviewable. Lifecycle evidence already
identifies ingestion as `Xwayland-SHM-to-existing-client-copy`.

The historical tail keeps `KBASE_IOCTL_MEM_JIT_INIT failed: Cannot allocate
memory (tiler heap creation will not work)` visible. No new occurrence was
observed in the sampled client log intervals. Prior BadMatch/BadDrawable/
GLXBadPixmap/AcquirePixmap failures remain visible as historical records;
none appeared as fresh matching lines in these intervals. No KFAULT was observed.
This does not establish a causal link between those historical failures and FPS.

Reports retain monotonic sample boundaries, Chromium event timestamps and
available frame IDs, X server millisecond update timestamps, window snapshots,
native epoch timestamps, and fresh-log offsets. CPU samples have clock labels;
host and phone monotonic clocks must not be compared directly. Epoch samples
permit correlation across the host/native logs and phone capture boundaries.

There is no shared frame ID across all these APIs. XDamage rectangles/distinct
server-millisecond batches are update observations, not exact Xwayland commits.
Do not infer a dropped-frame count by subtracting CEF events, XDamage batches
and Android handoffs. Gamescope's existing `missed` means a 60-Hz budget miss,
not a dropped frame. Actual repeated scanout and commit coalescing remain
unmeasured; the sampler states these gaps explicitly. Exact commit/coalescing
counts would need a small opt-in Gamescope/session instrumentation change in a
future build. No build is needed for the current diagnostics.

## Rerun

From the repository root, with the proven accelerated Steam session running,
navigate or scroll for the sample duration:

```sh
python3 -B tools/panvk/stage_phone_test.py \
  --serial E68DS8U4IVYTKJXK --perf-diagnostics --perf-seconds 25 \
  --perf-output /tmp/droiddeck-steam-perf-next --run
```

Use a new output directory each time. The fixed ADB-only diagnostic receiver
runs the dynamically pushed helper; it does not restart Steam. CPU counters and
XDamage observation need no renderer changes. CEF tracing is bounded to 16 MiB,
stops in cleanup, and refuses to stop another trace if its own start was refused.
Missing DevTools/XDamage counters are reported without changing startup flags.

The laptop directory contains `guest-report.txt`, `native-logcat.txt`, and
`native-cpu.jsonl`. The phone also retains the UUID-labelled diagnostic report
under `Download/DroidDeck/`. `--perf-seconds 5` is suitable for a smoke check.
No artifact argument or APK/PanVK rebuild is required. Normal Stop/restart and
the independent hardware renderer acceptance criteria are unchanged.

This workstation's captures are `/tmp/steam-perf-live-2.txt`,
`/tmp/steam-perf-final/`, and `/tmp/steam-perf-smoke/`; the longer native log is
`/tmp/steam-perf-native-live.log`. Raw diagnostic output is not committed.
The profiling result is a client swap/presentation bottleneck with a substantial
separately recorded SHM cost. No optimization is proposed before separating the
swap scope's internal costs and obtaining fresh Gamescope commit/import counters.
