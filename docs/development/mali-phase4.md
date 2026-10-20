# DroidDeck-Mali 0.5 port: Phase 4 final normal path

Base: Phase 3 commit `ed9dcabe` on `mali-0.5-port`.
Reference: final `mali-gpu-experiment` commit `3e1d9255`.

The port now connects the final optimized Gamescope normal renderer to the
already imported final proxy/broker implementation. ARM64 Steam and its dedicated
Xwayland use software GL and SHM; Gamescope alone uses the Mali proxy ICD, Android
system Vulkan, and AHB/SYNC_FD presentation. The automatic hardware gate remains
ARM vendor `0x13b5`, device `0x74021000`, Vulkan >= 1.1. SteamVR is excluded.
The earlier Desktop action still enters the normal interactive SHM client.

PanVK client integration, CEF Vulkan acceleration, `PANVK_ATOM_STRIDE`, shared CPU
staging/memfd activation and broader Mali support remain deferred. No proprietary
Mali binary is added. No local APK, Gamescope or driver build was performed and
no commit was created.

**Final coordinated implementation**

Phase 1 already imported the final native/proxy staging, live registry, dependency,
bulk transport, wait accounting and protocol implementations. Those sources and
all wire versions/IDs remain unchanged. Phase 4 supplies their Gamescope consumers
as seven overlays after the frozen Phase 3 patches. This advances the old normal
core to the final reference implementation rather than stopping at an early
performance checkpoint. All seven overlays must be applied together.

- Persistent mapped staging is reused only below the proven completed watermark;
  retirement, failed submission and shutdown retain completion requirements.
- Live registries replace historical-object walks and reject stale/wrong-parent
  IDs. Upload dependencies preserve ordering and planned layouts without reporting
  unsupported Vulkan features.
- Upload-ring writers register modified ranges; missing, saturated or invalid
  metadata falls back conservatively. Persistent staging uses acknowledged bounded
  writes up to 2 MiB; ordinary v7 writes remain 512 KiB, v6 operations remain 4 KiB,
  and the shader limit remains 512 KiB. Partial/malformed replies prevent submit.
- Default normal pacing is 60 Hz, with the reference 30 Hz diagnostic override,
  baseline comparison, verbose mode, frame-cost scopes and wait attribution.
- AHB publication, producer completion, Android release acknowledgement, timeout
  quarantine, same-UID socket checks and explicit disconnect cleanup remain in force.
  The Phase 3 unpublished/retired-buffer presentation guard is retained.

**0.5 runtime and Steam adaptations**

Mali Steam retains `MODE_STEAM` with a separate backend flag. It follows the current
0.5 session preparation rather than replacing that body with the old branch:
maintenance checks, session folders, bindings/storage, collections, networking,
audio/controllers, terminal, agent/store setup and runtime bookkeeping stay present.
Only Mali bypasses Turnip/Zink driver selection and forces the validated ARM64 client.
Other routes retain their ARM64/x86-64 preference, FEX environment and current CEF
arguments. SteamVR retains its ordinary route.

The current Steam script keeps its bootstrap, channel (`steamdeck_publicbeta`),
exit-status/restart handling and ordinary game/session behavior. The Mali software
UI branch skips automatic Proton provisioning, queued game launches and unsupported
scaling advertisements. It uses `-cef-use-angle=gl` with swrast/llvmpipe and never
adds `-cef-use-vulkan`. The outer Gamescope owns Xwayland's separate process group;
the Steam child must not run the ordinary orphan reaper against that sibling.
The private proxy/broker environment is removed from both Xwayland and nested clients.

Startup and teardown share the existing normal broker lock. Mali Steam PID/start-time
registration and final startup bookkeeping use `stopLock` plus generation checks,
preventing a cancelled startup from restoring a PID or starting state. Stop asks
Steam to exit, terminates/drains Gamescope, and stops the broker only after guest
release processing. Release failure retains the broker lease/quarantine and reports
failure. Normal idle/restart finalization remains through current 0.5 APIs.

`LinuxRuntime.command`, `logDir`, `prootTempDir`, `SessionFiles`, `SessionTerminal`
and current session events are reused. No `debugLogDir()` or Download override
API is restored. The existing app-specific environment override accepts only
`MALI_VULKAN_PERF_BASELINE=0|1`, `MALI_VULKAN_PERF_HZ=30|60` and
`MALI_VULKAN_VERBOSE=0|1` for Mali. It cannot replace the proxy, journal path or
shared-staging policy.

**Journal and measurement semantics**

The final native journal implementation already existed in Phase 1; 0217 wires
its Gamescope hooks and the compositor now records handoff/release/latch observations.
Android callbacks carry immutable current/previous identities captured on the
compositor thread. They never traverse live compositor buffers from callback threads.
All newer 0.5 presentation, Quest/VR/Cardboard, single-pixel and non-Mali code remains.

Records live in `files/logs/mali-frame-trace`, selected through the app-owned
`DROIDDECK_FRAME_TRACE_DIR`. The existing PRoot files-directory bind exposes that
same path to the guest. There is no shared Download fallback. Unarmed sessions have
no polling worker. Ready/control validation requires one fresh worker per role.
Records stop at capacity; contention and overflow increment explicit dropped counts.

The release APK cannot use `adb run-as`. The explicit journal broadcast receiver
therefore requires `android.permission.DUMP`, following the Phase 2 shell diagnostic
access pattern. It has no intent filter, cannot launch a broker/session, and requires
an idle/stopped session to arm. Export copies only journal/capture basenames into a
bounded ZIP in the app-specific external files directory for `adb pull`; active
records and controls stay private. This permission/export flow still needs device
validation on the release APK.

Example workflow after installing the CI APK:

```sh
python3 tools/mali-vulkan/frame_journal_control.py arm --serial SERIAL
# Launch the normal Mali session, then run in its Terminal:
droiddeck-mali-diagnostics journal --seconds 10
# After capture completion, on the host:
python3 tools/mali-vulkan/frame_journal_control.py pull --serial SERIAL --output frames.zip
python3 tools/mali-vulkan/frame_journal_control.py disarm --serial SERIAL
```

The ZIP retains prior captures. Extract the Gamescope/Android JSONL pair for one
run into its own directory, then invoke `analyze_frame_journal.py` on that directory.
Do not combine runs. `droiddeck-mali-diagnostics perf`, `cef` and `x11` observe a
proven running software Steam client; no guessed DISPLAY or PanVK staging occurs.
The `perf --swap-details --client-affinity 0xff --frame-details` mode retains the
reference explicit temporary affinity experiment and restoration; it is opt-in.

Interpretation remains deliberately limited:

- `vkQueueSubmit` wall time is a host call duration, not GPU execution time.
- `rpc_receive` includes socket blocking and scheduling; offCPU includes all
  descheduling, not just scheduler wakeup latency.
- Android latch timestamps and callback delivery do not establish physical scanout.
- A release-fence timestamp is its first observed readable time, not kernel signal time.
- Dropped records, missing workers, incomplete captures and ambiguous identities
  prevent exact decomposition. Nested scopes overlap and must not simply be summed.
- CEF/XDamage events do not manufacture Gamescope/AHB frame identities.

Only Python observers/analyzers are staged. Three pure analyzers retain their
existing source location under `tools/panvk`; staging them does not install or
enable a PanVK driver.

**Why each existing file changes**

| File | Required change |
| --- | --- |
| `.github/workflows/build.yml` | Stage pure diagnostic assets and run the Phase 4 host suite; preserve source/cache logic, same-run overlay, runtimeAssets preparation and APK validation. |
| `app/src/main/AndroidManifest.xml` | Declare the explicit DUMP-protected journal control/export receiver for release-APK diagnostics. |
| `app/src/main/cpp/waylandcomp/src/ahb_swapchain.c` | Add identity snapshots, handoff timing and first-readable release observations without weakening publication/release checks. |
| `app/src/main/cpp/waylandcomp/src/ahb_swapchain.h` | Expose the compositor-thread snapshot and its shared identity type. |
| `app/src/main/cpp/waylandcomp/src/sc_layer.c` | Record optional latch/transaction/release observations with immutable identities while retaining current presentation behavior. |
| `app/src/main/java/com/droiddeck/launcher/MainActivity.kt` | Select the Mali backend for exact-gated Steam requests while retaining Steam mode and excluding SteamVR. |
| `app/src/main/java/com/droiddeck/launcher/SessionActivity.kt` | Propagate the backend, retain it on reattachment and skip Proton/repair expectations for the Mali software UI. |
| `app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt` | Supply a mode-0700 current app log directory to both journal workers; continue forcing shared staging off. |
| `app/src/main/java/com/droiddeck/launcher/session/SessionFiles.kt` | Atomically stage the diagnostic dispatcher and its twelve Python dependencies through current 0.5 asset handling. |
| `app/src/main/java/com/droiddeck/launcher/session/SessionService.kt` | Integrate service-owned Mali Steam startup/stop into current session APIs, isolate driver selection and controls, register startup atomically, and expose the existing Terminal for normal diagnostics. |
| `tools/build_local.sh` | Include pure Python diagnostic staging in the existing asset recipe; the recipe was not executed locally. |
| `tools/gamescope/PATCHES.md` | Document reference mapping, coordinated overlays, application order, isolation and measurement limits. |
| `tools/gamescope/check-diagnostics.sh` | Reject stale component artifacts lacking Xwayland/staging/range/journal private-API markers while preserving diagnostic smoke checks. |
| `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` | Add the isolated Mali Steam outer route and software child environment while retaining current ordinary Steam/FEX/bootstrap logic. |
| `tools/mali-vulkan/diagnose_steam_cef.py` | Use the software-only client resolver rather than the absent/deferred PanVK runner. |
| `tools/mali-vulkan/diagnose_steam_perf.py` | Use that resolver and inherit the current session's journal path. |
| `tools/mali-vulkan/diagnose_steam_x11.py` | Resolve the existing software client's X connection without a PanVK dependency. |
| `tools/mali-vulkan/frame_trace.c` | Replace the hardcoded shared Download root with an explicit bounded app-specific root; preserve record/clock/drop semantics. |
| `tools/mali-vulkan/steam_frame_journal.py` | Resolve the same app-specific root and fail clearly when unconfigured; preserve fresh-worker/control/loss semantics. |
| `tools/mali-vulkan/test_bulk.py` | Check final applied protocol/header parity and 2 MiB staging bounds alongside executable transport fixtures. |
| `tools/mali-vulkan/test_diagnostics.py` | Include final component markers in the smoke-check fixture; retain Phase 2 isolation checks. |
| `tools/mali-vulkan/test_frame_cost.py` | Target the resequenced attribution patch and distinguish added work from unchanged diff context. |
| `tools/mali-vulkan/test_frame_trace.py` | Validate canonical journal copies and fence identity against the new namespace. |
| `tools/mali-vulkan/test_perf.py` | Inspect actual fully patched sources and adapt tiny performance mocks to the final ABI/journal instrumentation. |
| `tools/mali-vulkan/test_phase3.py` | Extend expected order and authorized Steam/performance controls while retaining Phase 3 release/ownership/no-PanVK/no-shared-staging checks. |
| `tools/mali-vulkan/test_steam_launch.py` | Exercise current 0.5 software launch/restart/stop/bootstrap behavior, current channel and CEF arrays; omit deferred PanVK activation cases. |
| `tools/mali-vulkan/test_upload_ring.py` | Target 0214; keep executable conservative range/fallback tests. |
| `tools/mali-vulkan/test_wait.py` | Accept added trace scopes at the existing wait sites without changing the executable wait accounting tests. |
| `tools/release/runtime_inputs.py` | Fingerprint the three pure analyzer dependencies alongside existing Mali sources and dynamic linuxfs outputs. |

No CMake, native broker/proxy implementation, `check_session_assets.py`, runtime API,
GPU-detection, FEX implementation, wlroots, PanVK-driver or existing Gamescope patch
replacement is needed. The shared `maliahb` target already compiles the journal once
for both Android consumers.

**Final Gamescope order**

All previous twenty files are byte-identical, checked with frozen SHA-256 values.
New overlays are manually reconciled against their exact predecessor on pinned
Gamescope 3.16.29, including 0.5 touch tracking and Phase 3 child isolation. The full
Mali stack applies with zero fuzz and zero offsets and reverses to the 0.5 base.
Final installed ABI/protocol/journal headers match canonical Phase 1 sources.

```text
0002-steamcompmgr-fallback-appid-focus.patch
0009-fix-arm64-steam-night-mode.patch
0019-steamcompmgr-arm64-virtual-white.patch
0020-color-p3-red-is-wide-gamut.patch
0100-realtime-queue-and-gamepad-cursor.patch
0101-nested-wayland-cursor-routing.patch
0110-wayland-backend-touch.patch
0111-wayland-pointer-warps-in-passthrough.patch
0112-restore-iconified-game-on-resume.patch
0114-take-override-redirect-from-mapnotify.patch
0115-limiter-paces-per-window.patch
0200-vulkan-enumerate-only.patch
0201-vulkan-capabilities.patch
0202-vulkan-create-device-test.patch
0203-vulkan-submit-test.patch
0204-vulkan-memory-ahb-tests.patch
0205-vulkan-gamescope-renderer-tests.patch
0206-vulkan-blit-input-proof.patch
0210-mali-persistent-session.patch
0211-mali-normal-wayland-session.patch
0212-mali-optimized-normal-resources.patch
0213-mali-frame-cost-attribution.patch
0214-mali-upload-ring-ranges.patch
0215-mali-steam-xwayland-client.patch
0216-mali-steam-x11-presentation-diagnostics.patch
0217-mali-frame-trace.patch
0218-mali-staging-bulk-writes.patch
```

Reference mappings: 0212 advances final 0121; 0213–0218 correspond to 0122–0127.
Existing canonical normal/performance headers are not inserted twice. The 0218
protocol extension completes the matching 2 MiB native/proxy path imported in Phase 1.

**Lightweight validation**

- Phase 4: 100 final-path tests and 65 pure analyzer tests passed. Executable
  fixtures cover staging/watermark reuse, live/stale registry IDs, upload
  dependencies/ranges, bounded 2 MiB/fragmented transport, actual native/proxy waits,
  real C journal overflow/callback identity, loss invalidation and software Steam
  launch/bootstrap/restart/stop. No full Gamescope renderer was built.
- Phase 2 regression: 63 broker/proxy/diagnostic tests and 25 small diagnostic
  translation-unit tests passed against the complete new stack.
- Phase 3 regression: all 26 passed, including the real host ownership/release
  harness, quarantine/restart, protocol, child isolation and patch application.
- Phase 1 regression: 101 Mali and 124 isolated PanVK/tool contracts passed.
  The historical runner's deferred source-boundary cases remain labelled; the
  applicable final-path cases run in the Phase 4 suite. Testing existing shared
  transport code does not enable it in a normal session.
- Unmodified 0.5 session regressions: 23 passed. Existing Steam cold/desktop
  launch regressions: 16 passed. Runtime input/fingerprint tests: 14 passed.
- The two new Kotlin bounded-control tests are left to CI's existing
  `testReleaseUnitTest`, alongside the prior hardware-gate tests. No Gradle build
  was run locally.
- Changed Python syntax, shell syntax, Android XML, workflow YAML and all 36
  embedded workflow shell steps pass validation. All-file `git diff --check`
  includes newly added files through a temporary index; the real index is unchanged.

Unix socket fixtures needed execution outside the sandbox after its socket policy
rejected the first attempts. They use local mocks and prove no device rendering.
The source helper copies pristine Gamescope into temporary directories and applies
patches; it never configures or builds Gamescope. Pure diagnostic staging tests
write only into temporary output directories.

Reproduce final-path checks with public Vulkan headers and pristine source:

```sh
python3 -B tools/mali-vulkan/run_phase4_tests.py \
  --gamescope-source /tmp/droiddeck-mali-phase2-source/gamescope-3.16.29 \
  --vulkan-headers /tmp/droiddeck-mali-phase2-headers/Vulkan-Headers-1.3.296/include
```

**CI/device validation still required**

GitHub Actions has not been triggered. It must compile/link the full ARM64
Gamescope patch stack and Android native/Kotlin APK; run release unit tests,
cursor/component smoke tests and SONAME auditing; confirm the packaged shared
registry/journal symbols; overlay the same-run component using matching source
fingerprints; and complete current runtimeAssets/asset validation. Host tests do
not replace these compiler/linker/package checks.

On the exact validated Mali-G52 phone, verify software Steam/Xwayland boot/login,
input/resize and normal UI presentation through actual proxy/broker Vulkan and
AHB/SYNC_FD. Check swrast/llvmpipe and OpenGL CEF arguments in the child and the Mali
proxy only in Gamescope. Exercise Stop, immediate restart, startup cancellation,
activity recreation/background/surface loss, Xwayland/client/broker loss and delayed
or missing Android releases. Confirm no stale sockets/groups, no broker shutdown
before releases drain, and quarantine on timeout.

Measure sustained memory/registry size, staging reuse and bounded 2 MiB writes
with real completion under UI load; compare baseline/default pacing without
interpreting host call time as GPU time. Validate release-APK journal permission,
arming before startup, both fresh workers, capture/export and loss accounting.
For repeated captures, analyze each run separately. Actual performance and display
timing remain unmeasured here. Ordinary Adreno ARM64/x86-64 Steam/FEX, desktop,
storage/store/session and Quest/VR/Cardboard behavior still requires its CI/hardware
regression checks.

**Every added or modified file**

Inventory follows; `A` means added in this phase, `M` means modified from Phase 3.

19 added files, 29 modified files, no deletions (48 total).

```text
A  app/src/main/java/com/droiddeck/launcher/gpu/MaliFrameJournalReceiver.kt
A  app/src/main/java/com/droiddeck/launcher/gpu/MaliSessionControls.kt
A  app/src/test/java/com/droiddeck/launcher/gpu/MaliSessionControlsTest.kt
A  docs/development/mali-phase4.md
A  tools/gamescope/patches/0212-mali-optimized-normal-resources.patch
A  tools/gamescope/patches/0213-mali-frame-cost-attribution.patch
A  tools/gamescope/patches/0214-mali-upload-ring-ranges.patch
A  tools/gamescope/patches/0215-mali-steam-xwayland-client.patch
A  tools/gamescope/patches/0216-mali-steam-x11-presentation-diagnostics.patch
A  tools/gamescope/patches/0217-mali-frame-trace.patch
A  tools/gamescope/patches/0218-mali-staging-bulk-writes.patch
A  tools/linuxfs/overlay/usr/local/bin/droiddeck-mali-diagnostics
A  tools/mali-vulkan/capture_frame_journal.py
A  tools/mali-vulkan/frame_journal_control.py
A  tools/mali-vulkan/gamescope_source.py
A  tools/mali-vulkan/run_phase4_tests.py
A  tools/mali-vulkan/stage-diagnostics.py
A  tools/mali-vulkan/steam_client_environment.py
A  tools/mali-vulkan/test_phase4.py
M  .github/workflows/build.yml
M  app/src/main/AndroidManifest.xml
M  app/src/main/cpp/waylandcomp/src/ahb_swapchain.c
M  app/src/main/cpp/waylandcomp/src/ahb_swapchain.h
M  app/src/main/cpp/waylandcomp/src/sc_layer.c
M  app/src/main/java/com/droiddeck/launcher/MainActivity.kt
M  app/src/main/java/com/droiddeck/launcher/SessionActivity.kt
M  app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt
M  app/src/main/java/com/droiddeck/launcher/session/SessionFiles.kt
M  app/src/main/java/com/droiddeck/launcher/session/SessionService.kt
M  tools/build_local.sh
M  tools/gamescope/PATCHES.md
M  tools/gamescope/check-diagnostics.sh
M  tools/linuxfs/overlay/usr/local/bin/droiddeck-session
M  tools/mali-vulkan/diagnose_steam_cef.py
M  tools/mali-vulkan/diagnose_steam_perf.py
M  tools/mali-vulkan/diagnose_steam_x11.py
M  tools/mali-vulkan/frame_trace.c
M  tools/mali-vulkan/steam_frame_journal.py
M  tools/mali-vulkan/test_bulk.py
M  tools/mali-vulkan/test_diagnostics.py
M  tools/mali-vulkan/test_frame_cost.py
M  tools/mali-vulkan/test_frame_trace.py
M  tools/mali-vulkan/test_perf.py
M  tools/mali-vulkan/test_phase3.py
M  tools/mali-vulkan/test_steam_launch.py
M  tools/mali-vulkan/test_upload_ring.py
M  tools/mali-vulkan/test_wait.py
M  tools/release/runtime_inputs.py
```
