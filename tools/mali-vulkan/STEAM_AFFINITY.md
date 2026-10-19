# Development Steam webhelper scheduling experiment, 2026-10-11

All-core affinity repeatedly reduced client swap latency and increased CEF
production in this phone session. It did **not** improve Android handoff/release
in these samples. There is evidence of a scheduling contribution, but no exact
poll wakeup measurement proving unchanged Mali completion latency or establishing
the dominant remaining GPU/driver cost. No optimization was applied.

The installed ARM64 `steamrtarm64/steamwebhelper.sh` contains:

```sh
exec taskset 0x7c $(pwd)/steamwebhelper "$@" &> ~/.steam/steam/logs/steamwebhelper.log
```

CPU0–5 report maximum 1.8 GHz, CPU6–7 report 2.0 GHz; all eight CPUs were online.
The live baseline was **mixed**: 111 threads used CPUs 2–6 and three used 0–7.
Consequently A observes that actual existing policy; it does not force those
three CEF threads into a narrower mask. B overrides all experimental webhelper
threads with `0xff`; C uses `0xfc` (CPUs 2–7). The main Steam process is measured
but its affinity is not changed.

## Same-run measurements

Real session: `2026-10-11-34-steam`; GPU process/main thread 1284,
VizCompositor 1362, renderer 1401. Each row is a separate approximately
10-second capture; no stage durations are summed across rows. Sampling occurred
on the current live UI without confirmed repeated user navigation. These short
captures are exploratory, not a statistically controlled workload benchmark.

| Arm, capture directory in `/tmp` | Swap samples | Swap wall ms | Swap CPU ms | Swap off-CPU ms | CEF main renderer FPS | Selected X11 update batches/s | Android handoff / release per s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| A, `steam-affinity-a2` | 191 | 35.01 | 4.95 | 30.06 | 18.94 | 18.35 | 18.56 / 18.56 |
| B, `steam-affinity-b1` | 216 | 31.70 | 5.34 | 26.36 | 21.40 | 21.11 | 17.79 / 17.67 |
| A repeat, `steam-affinity-a3` | 185 | 36.23 | 5.05 | 31.18 | 18.27 | 17.77 | 17.68 / 17.80 |
| C, `steam-affinity-c1` | 193 | 37.16 | 5.20 | 31.96 | 18.68 | 18.68 | 18.18 / 18.31 |
| B repeat, `steam-affinity-b2` | 228 | 29.77 | 5.28 | 24.49 | 22.09 | 22.09 | 17.66 / 17.66 |

CPU/off-CPU come from each completed swap's exact trace `tdur` and `dur - tdur`.
All completed in-window swaps are included, including those below 20 ms; this
avoids censoring faster results. C also had two browser-process frame-submit
events, reported separately in the raw data rather than added to renderer FPS.
X11 refers to window `0x2400034`; other windows' batches are reported separately.
Android rates use two or more cumulative counter points **inside the same arm**,
spanning approximately eight seconds, rather than pretending their two-second
logging windows exactly coincide with the full ten-second trace. Handoff is not
physical scanout. Every trace completed without reported data loss or an event
cap hit. All arms returned live CEF `SystemInfo.getInfo` evidence of:

```text
displayType = ANGLE_VULKAN
glImplementationParts = (gl=egl-angle,angle=vulkan)
glRenderer = ANGLE (ARM, Vulkan 1.3.354 (Mali-G52 r1 MC2 (0x74021000)), panvk-26.2.99)
```

| Arm | GPU-main CPU seconds | GPU-main completed runqueue delay seconds | Blocked, stable Mali ppoll observations inside swaps |
| --- | ---: | ---: | ---: |
| A | 3.273 | 2.859 | 114 |
| B | 3.709 | 2.177 | 97 |
| A repeat | 3.160 | 2.721 | 127 |
| C | 3.153 | 2.268 | 124 |
| B repeat | 3.902 | 2.437 | 67 |

These scheduler counters cover each approximately ten-second **thread sampling
subinterval**, including work outside swap. They must not be assigned wholesale
to individual swaps. The [kernel counter definition](https://docs.kernel.org/scheduler/sched-stats.html#proc-pid-schedstat)
distinguishes CPU runtime from waiting on a runqueue. Counts of sampled Mali
polls are not elapsed poll durations, and cannot establish faster GPU execution.

## Separating sleeping from runnable delay

The dynamic helper has no permitted `sched_wakeup`/`sched_switch` trace or exact
poll entry/exit probe. It reports `exact_poll_sleep_ms` and
`exact_poll_wakeup_to_run_ms` as unavailable. It does not interpret all 30 ms of
off-CPU swap time as device sleep or claim a per-poll wakeup latency.

Per-swap scheduler bounds account for clock/read brackets **and pending runqueue
waits crossing scope boundaries**. Completed runqueue counters are not continuous
live timers. The residual bounds describe non-runnable time of any cause, not
specifically a Mali poll. In these captures the bounds remain too wide to resolve
the split: mean runnable lower bounds are zero; mean upper bounds are roughly
23–28 ms. Whole-run counters show a scheduling contribution more clearly than
the individual short-swap bounds. `summarize_affinity.py` recomputes the corrected
boundary accounting consistently for all saved arms.

Requested thread sampling is 5 ms, but maximum actual gaps were 140, 280, 160,
227 and 348 ms respectively. The watchdog itself averaged approximately 20% of
one CPU core; other observer work is also reported. The sampler therefore has
material overhead and cannot resolve short wakeups reliably. Affinity changes
can change CPU throughput, driver submission timing and contention as well as
queue delay; the result does not isolate wakeup latency causally. C did not
reproduce B's swap improvement, so CPU7 exclusion alone is not proven to explain
the baseline.

The existing JIT-init warning remains visible in baseline client logs. No fresh
matching warning or KFAULT was observed during these five log intervals. Old
Composite/GLX errors remain documented; no fresh matching failure was observed
in these intervals. None is suppressed or assumed to explain the result.

## Temporary scope and restoration

`--client-affinity` requires `--swap-diagnostics --run`. The worker selects only
actual `steamwebhelper` processes in the validated isolated Steam session,
requiring its display/session identity, Vulkan environment and atom stride 56.
Gamescope, Xwayland, the Android app/broker, games and Proton/FEX are excluded.
No Steam-owned file, renderer setting, PanVK binary/capability, Vulkan bridge,
startup configuration or APK is changed.

Every pre-existing thread's mask and start-time identity is saved before mutation.
Effective masks are verified. Normal cleanup, handled termination, pipe EOF and
a bounded watchdog deadline trigger restoration; it occurs before trace report
processing. A lock prevents overlapping affinity experiments. Recycled TIDs
are skipped. New threads have no pre-experiment mask: their restoration policy
comes from an unambiguous pre-existing thread name or original process leader,
and this inference is printed explicitly. C created two WSI threads; both were
returned to their same-name baseline CPUs 2–6. All five captures reported
verified restoration, without errors. B repeat exercised the final restoration
code, including a follow-up discovery pass for newly inherited threads.

The watchdog is in the diagnostic PRoot instance. Forced PRoot teardown/SIGKILL
can kill both sampler and watchdog; this is not a crash-proof transaction. If a
report lacks `affinity-restored` with `ok=true`, do not continue comparing arms:
use normal Stop and Launch to reinstate the unchanged Steam wrapper policy.
Normal Stop/restart behavior is preserved.

## Exact no-rebuild commands

Run from the repository root with the existing accelerated session open. Keep
the page and workload the same for each arm. Each command restores masks before
returning. The unique output root prevents overwriting a prior capture.

```sh
capture_root=$(mktemp -d /tmp/droiddeck-affinity.XXXXXX)
python3 -B tools/panvk/stage_phone_test.py --serial E68DS8U4IVYTKJXK \
  --swap-diagnostics --client-affinity 0x7c --perf-seconds 10 \
  --perf-output "$capture_root/A" --run
python3 -B tools/panvk/stage_phone_test.py --serial E68DS8U4IVYTKJXK \
  --swap-diagnostics --client-affinity 0xff --perf-seconds 10 \
  --perf-output "$capture_root/B" --run
python3 -B tools/panvk/stage_phone_test.py --serial E68DS8U4IVYTKJXK \
  --swap-diagnostics --client-affinity 0xfc --perf-seconds 10 \
  --perf-output "$capture_root/C" --run
python3 -B tools/panvk/summarize_affinity.py \
  "$capture_root/A" "$capture_root/B" "$capture_root/C"
```

Outputs include timestamped CEF frame/swap scopes, per-thread CPU/affinity and
scheduler state, X11 batches/windows, warning log coverage, native Android
counter timestamps, native app/broker thread CPU, and restoration results.
No PanVK or APK rebuild and no Steam restart is needed.
