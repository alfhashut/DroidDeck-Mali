# Checkpoint 7: normal session integration audit

Audited before production edits against `mali-gpu-experiment` at `5a4353f`,
Gamescope 3.16.29 with patches 0001–0120, and the wlroots revision pinned in
`tools/wlroots/release.env`. Checkpoint 6B is phone validated by the reported
150-frame result. The first sections preserve the pre-edit audit; the
implementation and validation sections below describe the current working tree.

## Existing normal Launch path

| Step | Exact repository entry points and behavior |
| --- | --- |
| Launch | `MainActivity` constructs `FrontEndActions(onPlay = { startSteamSession() })`; `startSteamSession()` calls `startSession(Intent(SessionActivity), steamSession = true)`. Desktop uses `MODE_DESKTOP`; application launches use `MODE_RUN`. `startSession()` checks runtime installation and the Steam phantom-process gate. `startActivity()` applies installation guards and the launch animation; running sessions are rejoined. |
| Android surface | `SessionActivity.surfaceCreated()` computes `outputSize()` from existing display/settings preferences, publishes `SessionState.outputSize` and refresh, sets scale/effects/HDR, then calls `CompositorHost.startOrAttach()`. It calls `SessionService.start()` only when no session is running. `surfaceChanged()` calls `CompositorHost.resize()`; `surfaceDestroyed()` detaches the current Surface. |
| Compositor | `CompositorHost.startOrAttach()` calls `WaylandCompositor.nativeStartWithSurface()` once, subsequently `nativeSetSurface()`, and drives native vsync through Choreographer. The native `waylandcomp/src/compositor.c` creates **fixed `wayland-0`**, unlinks stale socket/lock, and advertises compositor, SHM, xdg-shell, DMA-BUF and `banner_ahb_v1`. It stays alive across guest sessions. |
| Display driver | `SessionActivity.surfaceCreated()` installs/resolves `TurnipDriver` and passes its directory, library and native-library directory into `CompositorHost.startOrAttach()`. This is Android/Bionic display Vulkan, distinct from the guest ICD. Mali must select the system/vendor display driver, not a glibc ICD or imported Turnip. |
| Service | `SessionService.onStartCommand()` owns foreground-service startup, duplicate-running guard, mode and arguments, generation, locks, orphan reaping and `runSession(gen)`. `runSession()` stages `SessionFiles`, opens session logs, constructs the clean guest environment, components and binds, and starts proot through `HostProcess.start()`. Exit calls `stopSession(status)`. |
| Runtime | `LinuxRuntime.command()` builds the packaged proot command with `--kill-on-exit`, UID/GID emulation, runtime/rootfs and host binds. `LinuxRuntime.GUEST_RUNTIME_DIR` is `/run/droiddeck`; `.wayland-rt` is bound there to avoid Unix socket path limits. The app files directory is also available at its own host path. |
| Guest environment | `SessionService.addClientEnvironment()` starts `/usr/bin/env -i`, then adds HOME/USER/PATH/LANG/TZ, `XDG_SESSION_TYPE=wayland`, `XDG_RUNTIME_DIR=/run/droiddeck`, **`WAYLAND_DISPLAY=wayland-0`**, and `GAMESCOPE_FORCE_GENERAL_QUEUE=1`. Existing Zink/Turnip and client options follow. |
| Guest Vulkan | `LinuxRuntime.vulkanIcd()` prefers the runtime freedreno manifest. `LinuxVulkanDriver.resolveIcdPath()` supplies optional `BL_VK_DRIVER`; the session script checks its manifest/library and selects `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`, otherwise retaining the runtime driver. Mali must have explicit per-session selection and hard failure, with no Turnip fallback. |
| Gamescope command | `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` first-pass startup waits for the outer Wayland server and constructs **`gamescope --backend wayland --expose-wayland -f -W … -H … -w … -h …`** with existing resolution/refresh settings. It sets `BL_INSIDE=1` and launches itself as Gamescope's client command. The second pass starts the selected application. Steam adds Steam-specific switches; desktop can run labwc directly. |
| Input | `SessionActivity.movePointer()`, touch/mouse listeners and `onKey` send existing JNI events through `WaylandCompositor.nativeSendPointer()`, `nativeSendTouch()`, `nativeSendSceneInput()` and `nativeSendKey()`. The Android Wayland seat delivers these to the focused nested Gamescope surface. `CWaylandInputThread` in Gamescope's `Backends/WaylandBackend.cpp` receives pointer, touch and keyboard events and calls `wlserver_*` seat forwarding functions. No separate Android input protocol is needed. |
| Ready/state | `SessionEvents` and `SessionState` already model IDLE, preparation/startup, READY, SUSPENDED, STOPPING and FAILED. `SessionActivity.firstFrameListener` calls `SessionEvents.firstFrame()`; readiness requires a live guest PID and first frame. Failures have code/message/status. These should remain the normal UI state authority. |
| Stop/restart | Activity drawer Stop and the foreground notification call `SessionService.stop()`. `stopSession()` claims stop under `stopLock`, transitions STOPPING, stops auxiliary components and asynchronously tears down the proot tree. `teardown()` snapshots PID start times, sends SIGTERM, waits `GRACE_MS`, then kills remaining matching descendants. `finishSessionStop()` checks the generation and transitions IDLE on zero or FAILED on nonzero, notifies listeners and stops the service. A Mali broker must remain alive until guest GPU/release teardown has finished. |
| Crashes | `HostProcess.start()` exit callback reaches `stopSession(status)`; `OrphanReaper`/`killGuestLeftovers()` handle stale processes. The native broker owns objects per connection and cleans them on disconnect. Checkpoint 6 release timeout retains the exact Android-owned allocation and native parents in quarantine, never treating timeout as release. |
| Lifecycle | `SessionActivity.onStop()` sets activity visibility false; service policy controls suspension. `onPause()` releases transient input. `surfaceDestroyed()` pauses vsync and detaches the Surface. `onDestroy()` deliberately does not kill a background session. `SessionService.onTaskRemoved()` stops unless PiP policy retains it; `onDestroy()` also calls stop. |

## Output handoff audit

`CWaylandBackend::Init()` connects to the outer Wayland socket, binds the
required globals, initializes Vulkan and `wlsession_init()`, and starts
`CWaylandInputThread`. `CWaylandConnector::Present()` composes through the real
Gamescope renderer and exports/imports backend framebuffer DMA-BUFs. Neither
turning its Vulkan version check off nor moving the Headless diagnostic into
Launch supplies a valid Mali output handoff.

Checkpoint 4A established that relevant generic Linux DMA-BUF queries are
importable but not exportable. Checkpoints 4D–6 established real Android AHB
allocation, import into vendor Vulkan, producer SYNC_FD export and release.
No generic DMA-BUF exportability may be invented.

The Android compositor already supports `banner_ahb_v1` version 2 in
`waylandcomp/src/ahb_swapchain.c`. Its `attach()` receives a real AHB over a
Unix socket and associates it with an **existing DMA-BUF wl_buffer**.
`ahb_swapchain_present()` uses `sc_layer_present_ahb()` to display it.
`handle_released()` waits for the actual Android release fence before sending
any deferred `wl_buffer.release`. This can carry an AHB without importing it
into the Android Vulkan renderer, but the existing protocol cannot create an
AHB-only wl_buffer. That is the missing normal-path transport, not a version
gate. AHB-only transport must preserve that real release mechanism, including
on disconnect; broker allocation ownership must also remain protected.

The Checkpoint 6 private `DD_SESSION_BEGIN/PRESENT/END/STATS` entrypoints
(opcodes 83–86, wire v7) own a separate debug SurfaceControl consumer. They
must stay diagnostic and cannot become the normal Wayland presentation API.
Its fixed 256×256 client, injected child, selected GPU readbacks, hashes and
frame limit are also inappropriate for normal Launch.

## Integration acceptance boundary

A completed integration must use the real nested Wayland backend and the
normal Android Surface; launch an interactive Wayland child through the normal
session command; use existing pointer/touch/keyboard forwarding; respect the
UI resolution; and stop producing before draining GPU and Android ownership
with a five-second release deadline. A timeout fails and quarantines resources.
Adreno must retain existing driver selection and startup behavior. Native
Vulkan features remain truthful Vulkan 1.1 with real KHR timeline, EXT scalar
layout and KHR image-format-list, FP32/typed descriptors/PASSTHRU.

No Checkpoint 7 phone result, FPS improvement or normal-session PASS is
established by this source audit.

## Implemented normal Launch path

The integration uses the existing Launch action, Activity, foreground Service,
runtime/proot builder, session script, Android compositor and Gamescope Wayland
backend. There is no second Android launcher or debug-Activity requirement.

1. `MainActivity`'s `FrontEndActions.onPlay` calls `startSteamSession()` and
   `startSession()`. The existing method name does not force Steam: a supported
   GPU changes the intent mode to `MaliSessionSelection.MODE` (`mali-wayland`),
   removes Steam UI/URL extras and bypasses only the Steam phantom-process gate.
   Actual GPU discovery runs off the main thread through `VulkanInfo.query()`.
   Selection uses vendor `0x13b5`, device `0x74021000`, API at least 1.1 and no
   query error; the display name is not a gate. Other devices retain their path.
   A pending GPU query, startup phase or STOPPING phase rejects duplicate starts;
   an already running session can still be rejoined.
2. `SessionActivity.surfaceCreated()` uses `loadingMode()` so a live session
   reopened from the notification retains its Mali mode. It passes the normal
   Surface and UI-selected resolution to `CompositorHost.startOrAttach()`.
   Mali skips Turnip installation, selects system/vendor display Vulkan through
   the existing loader, sets `DROIDDECK_MALI_NORMAL_SESSION=1` before the first
   compositor start and disables HDR/frame generation for this path. Other GPUs
   keep their existing display initialization. Native Mali display initialization
   requests Vulkan 1.1 and its swapchain subset; it skips desktop DMA-BUF import
   formats and frame-generation feature probing. The compositor remains the
   process-wide server at `.wayland-rt/wayland-0`.
3. `SessionService.onStartCommand()` retains the existing session generation,
   foreground service, lifecycle, state, lock and duplicate-start machinery.
   `runSession()` dispatches only Mali mode to `runMaliSession(gen)` before
   Steam/components setup. That function stages `SessionFiles`, opens normal
   session logs and starts `MaliNormalBroker` on the service worker.
4. `MaliNormalBroker.start()` requires the selected GPU and started compositor.
   It waits up to ten seconds for a connectable outer Wayland Unix socket;
   `CompositorHost.isStarted` alone cannot prove its native worker has bound it.
   It then leases `SystemVulkanBroker.startNormal()`, executes the existing v1
   native inventory RPC with a three-second read timeout and validates the
   response header, size, result and Mali IDs/API. A socket filename alone is
   not the broker readiness handshake. Assets are staged atomically and checked
   for the expected manifest and AArch64 ELF header before the guest starts.
5. `runMaliSession()` constructs `/usr/bin/env -i` and calls the existing
   `LinuxRuntime.command()` and `HostProcess.start()`. `LinuxRuntime.java`
   binds the app files directory at its real path and `.wayland-rt` at
   `/run/droiddeck`, making both the proxy/broker and outer display reachable.
   Generation/running checks under `maliStartupLock` and `stopLock` prevent a
   cancelled startup from launching after teardown.
6. The Mali branch of `/usr/local/bin/droiddeck-session` validates opt-in,
   outer `wayland-0`, both sockets and the selected manifest, then executes:

   ```sh
   gamescope --backend wayland --mali-wayland-session --expose-wayland -f \
     -W "$BL_WIDTH" -H "$BL_HEIGHT" -- \
     /usr/local/bin/gamescope --mali-interactive-client
   ```

   This early branch never enters Steam, Proton, DXVK or the normal desktop's
   client setup. `BL_WIDTH/BL_HEIGHT` come from `SessionState.outputSize`, not
   the Checkpoint 6 256x256 diagnostic size. Mali uses the existing Steam-page
   resolution preferences through `SessionPrefs.prefMode()`.

The guest environment selects only the session's proxy:

```text
XDG_SESSION_TYPE=wayland
XDG_RUNTIME_DIR=/run/droiddeck
WAYLAND_DISPLAY=wayland-0
VK_DRIVER_FILES=<app files>/mali-vulkan/mali_proxy_icd.json
VK_ICD_FILENAMES=<same manifest>
MALI_VULKAN_BROKER_SOCKET=<app files>/mali-vulkan/broker.sock
MALI_VULKAN_NORMAL_SESSION=1
MALI_VULKAN_RENDERER_TEST=1
MALI_VULKAN_AHB_TEST=1
VK_LOADER_LAYERS_DISABLE=*
GAMESCOPE_FORCE_GENERAL_QUEUE=1
GLIBC_TUNABLES=glibc.pthread.rseq=0
```

`broker.sock` above denotes the path returned by `startNormal()`; the code does
not invent a second socket. The two legacy TEST-named environment gates reuse
the proven renderer/AHB subset. They do not launch a diagnostic. The normal
environment does not enable `MALI_VULKAN_SESSION_TEST`, Turnip or loader tracing.
No global guest driver setting/rootfs manifest is replaced. Android display
Vulkan remains distinct from Gamescope's glibc proxy Vulkan. The manifest's
conservative instance API `1.0.0` is unchanged; physical device reporting remains
the real vendor API (1.1.131 on the validated phone), with real KHR timeline,
EXT scalar layout and KHR image-format-list support. FP32, typed dummy
descriptors and PASSTHRU reuse Checkpoints 5/6. No Vulkan 1.2 or robustness2
support is fabricated.

## Gamescope backend, AHB transport and input

Patch `0121-mali-normal-wayland-session.patch` adds the normal runner and argv
client to the same Gamescope binary. `mali_normal_main()` initializes the proven
renderer, installs the actual `CWaylandBackend`, creates its real
`CWaylandConnector` and calls `PostInit()`/`Present()`. Output uses actual
`vulkan_composite()` and BLIT. The HeadlessBackend diagnostic is not selected.

The desktop framebuffer DMA-BUF handoff is replaced only for this mode:

- `libmaliahb.so` shares an Android-local AHB registry between the existing
  Bionic broker and Android compositor. Registry keys are opaque, monotonic
  integers; no Vulkan pointer/native handle is serialized to the guest.
- `banner_ahb_v1` version 3 adds `create_broker_buffer` to create a real AHB-only
  `wl_buffer` from a registered key. Existing version-1/2 requests remain.
  This buffer has no fabricated DMA-BUF planes/exportability. Generated server
  protocol files and the Gamescope protocol XML are included in the changes.
- Three output AHBs use the UI resolution (bounded to 4096 per dimension and
  64 MiB allocations in normal mode). Diagnostic limits stay unchanged.
- GPU timeline completion and the real producer SYNC_FD wait precede publication
  and the nested `wl_surface.commit`. The registry independently protects the
  image/allocation against rendering, reuse and destruction while owned.
- Android's existing `ahb_swapchain_present()` -> `sc_layer_present_ahb()` uses
  the normal Activity Surface. Its actual previous-release callback/fence
  authorizes registry release and `wl_buffer.release`. Cancellation of a commit
  that never reached Android is distinct and never increments Android releases.

The wire remains v7. Optional, explicitly enabled normal RPCs are separate from
the unchanged Checkpoint 6 opcodes 83–86:

| Opcode | Operation | Payload/result |
| --- | --- | --- |
| 87 | BEGIN | device, verbose -> begin normal session |
| 88 | REGISTER | device, AHB token -> Android-local registry key |
| 89 | PUBLISH | device, AHB token, producer sync token, frame -> publish ownership |
| 90 | END | device -> GPU drain and bounded real Android release wait |
| 91 | STATS | device -> 20 live counts and 6 ownership/presentation/FD totals |

`vkDroidDeckWaylandMALI` is a private transport entrypoint, not an advertised
Vulkan extension. No new standard Vulkan entrypoint was required. The original
`vkDroidDeckSessionTEST` remains diagnostic-only.

Input follows `SessionActivity` touch/mouse/keyboard handlers -> existing
`WaylandCompositor` JNI -> Android's Wayland seat -> Gamescope's existing
`CWaylandInputThread` listeners -> normal runner's wlroots seat -> the real
argv Wayland client. No new Android input protocol was introduced. Touch is
mapped to pointer movement/click through those listeners. The client paints
a dark background, movable/color-changing square and heartbeat using two
release-protected SHM buffers. Pointer/touch moves the square, clicks change
color, arrows/WASD move it, Space/Enter change color and Escape exits. It runs
until exit/stop, with no diagnostic frame limit. Input counts are summarized.
Existing Android pointer UI remains; a new Gamescope cursor layer and gamepad
mapping are not implemented for this initial client.

This remains an experimental **single-client** Gamescope session, using a small
wlroots SHM surface server inside Gamescope and the real nested output backend.
It does not establish general xdg-shell/multi-window/Xwayland/steamcompmgr support
or arbitrary application compatibility. Those are outside this checkpoint.

## Session state, stop and performance

Existing `SessionEvents` remains authoritative: preparation/compositor/guest
startup -> READY only after guest PID plus first Android frame; STOPPING -> IDLE
on clean exit, or FAILED with code/message on failure. Mali gets explicit
experimental startup/paused text. Notification reattachment uses live mode.
The existing service suspension, Surface detach/rebind and task-removal policy
remain active and need phone lifecycle validation.

User Stop resumes a suspended guest, signals the matching Gamescope process,
and grants seven seconds for its own client/GPU/Android cleanup before the
existing PID-start-time-checked proot tree teardown. Gamescope stops submissions
and its child, detaches output, drains GPU work, then END waits up to five
seconds for real Android releases. Timeout logs token/key/frame/ownership,
presentation/consumer state and available sync/fence information, returns
failure and retains the exact owned allocation/native parents. Timeout never
returns an owned AHB to Free and never counts it as a successful release.

The Service stops the broker after guest teardown, including on startup or
guest failure. A startup lock closes the stop/start race; a failed native stop
keeps its broker lease. Native quarantine makes cleanup fail and prevents a new
normal lease until Android app-process restart. This exception is not a
successful restart or a zero baseline. Clean stop permits the normal next Launch
without restarting the app. Broker/compositor disconnects return failure rather
than using the upstream input-thread abort in Mali mode.

Normal frames disable readbacks/hashes, descriptor tracing and opcode INFO
spam. `MALI_VULKAN_VERBOSE=1` enables deep renderer/transport traces. Every
three seconds, summaries report rendered frames, actual Android presentations,
releases, backpressure attempts (`dropped`), late frames, average FPS, owned
buffers, pending GPU work and open sync FDs. Final accounting separately reports
commits cancelled before display and validates child counts/FD balance before
the acknowledged device/queue destruction. Backpressure attempts are not
necessarily distinct lost client frames. No phone FPS improvement is claimed;
compare measured phone summaries to the reported Checkpoint 6B ~5.75 FPS.

## Low-memory recovery validation and remaining gates

Checkpoint 6 core session/consumer/patch/protocol/test files matched HEAD at
recovery. No empty/truncated files, conflict markers or malformed patch structure
were found. The remaining uncommitted work is Checkpoint 7, not a replacement
Checkpoint 6 implementation.

Only cheap checks are authorized in this continuation:

```sh
# Does not invoke the compiling NormalBrokerTests/NormalSessionTests fixtures.
cd tools/mali-vulkan
python3 -B -m unittest test_normal_launch.NormalLaunchTests -v
```

These tests execute the production shell branch with a tiny argv-capturing stub
and real local socket filenames, and inspect startup/order/duplicate/reattachment
contracts. They do not execute Android Activities, native Vulkan or Gamescope.
Local Unix socket restrictions may require running outside the tool sandbox.
Python AST/XML parsing, shell syntax, patch parsing and `git diff --check` are
also cheap checks. No APK build, Gradle, Gamescope/NDK/glibc compilation, QEMU,
containers, full suites or artifact packaging are run in this continuation.

Deferred CI gates: Android compilation/unit tests including
`MaliSessionSelectionTest`; complete pinned Gamescope patch-stack application,
compile/link and packaging; native/proxy builds; Checkpoint 1–6 regression tests;
normal native ownership/release/nested-input/restart/failure tests; workflow and
APK-content checks. Existing tests for these are included, but this low-memory
pass does not claim they passed against the final changes. Use one normal
`Build APK` Actions run on the reviewed remote `mali-gpu-experiment` revision;
its experimental ARM Gamescope job must include 0121, and the APK job must carry
that same Gamescope plus updated proxy/native libraries. No commit/push or
Actions dispatch is performed here.

Phone gate, using that single APK:

1. `adb install -r /path/to/app-release.apk` with a compatible signer. Keep
   the existing Linux runtime installed; select a conservative UI resolution
   such as 720p in the existing session settings.
2. Open normal DroidDeck and press normal Launch. Do not open the Vulkan debug
   Activity. Require Mali experimental startup, broker-ready event, nested
   `wayland-0`, real renderer and visible interactive client in the normal view.
3. Move/touch/click and use arrows/WASD/Space; confirm the painted square moves
   and changes color. Leave it running for at least two minutes. Retain session,
   app and compositor logs with FPS/release/resource summaries.
4. Stop through the normal drawer or notification. Require clean guest exit,
   UI Idle, no release timeout, balanced exported FDs and zero session baseline.
5. Launch again without reboot/app restart, interact again and stop again.
6. Check background/foreground and Surface recreation/notification reattachment,
   plus unexpected guest/broker/compositor loss. Failures must be useful and
   bounded; no unsafe reuse or false success is acceptable.

Checkpoint 7 phone acceptance and final-build validation remain pending.

## Results of this low-memory continuation

- Seven targeted `NormalLaunchTests` passed in 0.033 seconds; no compiler or
  native/Gamescope test fixture was invoked. Sandbox socket restrictions were
  handled by rerunning only this same lightweight class outside the sandbox.
- Shell syntax passed for the session and build scripts; seven changed
  Python/XML files parsed; resource names are unique.
- Both patch files parse; copied Gamescope/native normal ABI and AHB protocol
  XML match; changed files have no empty/NUL/truncation/conflict-marker or
  trailing-whitespace findings; `git diff --check` passed.
- No APK/Gradle, full suites, NDK/glibc/Gamescope compilation, QEMU, containers,
  dependency rebuilds, artifact packaging, commits or pushes were run.

The focused continuation changed `MainActivity.kt`, `SessionActivity.kt`,
`SystemVulkanBroker.kt`, `MaliNormalBroker.kt`, default `strings.xml`,
`test_normal_launch.py` and this document. The remaining status entries below
were existing Checkpoint 7 work preserved through recovery.

## Working-tree status after the continuation

Branch `mali-gpu-experiment`, HEAD `5a4353f`: 30 tracked modifications and 17
untracked files; no staged changes. Full changed-file list:

```text
 M .github/workflows/build.yml
 M app/src/main/cpp/CMakeLists.txt
 M app/src/main/cpp/malivulkan/interop_commands.h
 M app/src/main/cpp/malivulkan/interop_objects.h
 M app/src/main/cpp/malivulkan/renderer_commands.h
 M app/src/main/cpp/malivulkan/submit_commands.h
 M app/src/main/cpp/malivulkan/system_broker.c
 M app/src/main/cpp/waylandcomp/CMakeLists.txt
 M app/src/main/cpp/waylandcomp/generated/banner-ahb-v1-protocol.c
 M app/src/main/cpp/waylandcomp/generated/banner-ahb-v1-server-protocol.h
 M app/src/main/cpp/waylandcomp/protocols/banner-ahb-v1.xml
 M app/src/main/cpp/waylandcomp/src/ahb_swapchain.c
 M app/src/main/cpp/waylandcomp/src/compositor.c
 M app/src/main/cpp/waylandcomp/src/droiddeck_ext.h
 M app/src/main/cpp/waylandcomp/src/vk_present.c
 M app/src/main/cpp/waylandcomp/src/wl_dmabuf.c
 M app/src/main/java/com/droiddeck/launcher/MainActivity.kt
 M app/src/main/java/com/droiddeck/launcher/SessionActivity.kt
 M app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanBroker.kt
 M app/src/main/java/com/droiddeck/launcher/session/SessionPrefs.kt
 M app/src/main/java/com/droiddeck/launcher/session/SessionService.kt
 M app/src/main/res/values/strings.xml
 M tools/gamescope/build-in-arch.sh
 M tools/gamescope/test_vk_enumerate_only.py
 M tools/linuxfs/overlay/usr/local/bin/droiddeck-session
 M tools/mali-vulkan/device_icd.h
 M tools/mali-vulkan/icd_proxy.c
 M tools/mali-vulkan/interop_icd.h
 M tools/mali-vulkan/test_icd.py
 M tools/mali-vulkan/tests/mock_renderer.h
?? app/src/main/cpp/malivulkan/normal_commands.h
?? app/src/main/cpp/malivulkan/normal_ownership.c
?? app/src/main/cpp/malivulkan/normal_ownership.h
?? app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt
?? app/src/main/java/com/droiddeck/launcher/gpu/MaliSessionSelection.kt
?? app/src/test/java/com/droiddeck/launcher/gpu/MaliSessionSelectionTest.kt
?? tools/gamescope/patches/0121-mali-normal-wayland-session.patch
?? tools/mali-vulkan/NORMAL_SESSION.md
?? tools/mali-vulkan/normal_api.h
?? tools/mali-vulkan/normal_protocol.h
?? tools/mali-vulkan/test_normal.py
?? tools/mali-vulkan/test_normal_launch.py
?? tools/mali-vulkan/test_normal_session.py
?? tools/mali-vulkan/tests/normal_android_release.c
?? tools/mali-vulkan/tests/normal_host.cpp
?? tools/mali-vulkan/tests/normal_outer.c
?? tools/mali-vulkan/tests/normal_ownership.c
```
