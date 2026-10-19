# Checkpoint 6: persistent diagnostic session

## Source audit, before implementation

Baseline: branch `mali-gpu-experiment`, commit `edb5712`, Gamescope 3.16.29
with DroidDeck patches through 0119; wlroots 0.20.2, pinned by
`tools/wlroots/release.env`. Checkpoints 1–5 are phone validated. Checkpoint 6
requires separate phone acceptance; host execution cannot establish Mali or
Android display correctness.

1. `SessionService.addClientEnvironment` launches Linux processes using the
   existing `LinuxRuntime`/`GuestCommand` proot machinery. The guest receives
   `/usr/bin/env -i`, HOME=/root, XDG_SESSION_TYPE=wayland,
   XDG_RUNTIME_DIR=/run/droiddeck (see LinuxRuntime.GUEST_RUNTIME_DIR),
   WAYLAND_DISPLAY=wayland-0 and GAMESCOPE_FORCE_GENERAL_QUEUE=1. The runtime
   directory is bound into the guest; the actual Android files directory stays
   outside the short guest socket path. GuestCommand stages the APK's Gamescope.
2. `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` starts Gamescope with
   `--backend wayland --expose-wayland -f`, output/nested dimensions and refresh
   options. Steam mode adds its own switches and child command. Desktop mode
   uses labwc. DISPLAY belongs to Gamescope's Xwayland child environment; it is
   not the Android output socket. Normal startup is not a headless or DRM path.
3. `CompositorHost`/`WaylandCompositor` own Android's native Wayland compositor.
   SurfaceView attach/detach controls its Android window; Choreographer drives
   vsync. `waylandcomp/src/compositor.c` binds fixed socket `wayland-0` so the
   Linux environment and Android socket agree.
4. Stock output flows through Gamescope's Wayland backend, as a client of this
   Android server. The normal server implements wl_compositor, wl_shm,
   xdg-shell, linux-dmabuf and presentation feedback;
   the custom banner AHB protocol is handled by `ahb_swapchain.c`.
   Android does not advertise a standard Wayland explicit-sync/syncobj global
   here: ahb_swapchain exports/imports implicit dma-buf sync_file fences and
   sc_layer supplies SurfaceControl acquire/previous-release fences. Gamescope
   has separate wlroots syncobj support on its client-facing normal server;
   this requires the normal DRM/timeline infrastructure.
   `sc_layer.c` imports/presents AHBs with SurfaceControl, with other compositor
   paths available for buffers that need composition. wl_surface.frame and
   wl_buffer.release provide the ordinary Wayland callback/lifetime mechanisms.
5. Gamescope already has `IBackend`/`IBackendConnector::Present(FrameInfo*)`.
   Its HeadlessBackend is a real backend but its connector Present currently
   does nothing. Its normal Init also initializes Vulkan and the wlroots
   session. This is the smallest existing backend hook for the diagnostic:
   preserve stock initialization, add an explicitly selected diagnostic route
   for rendering/presenting through the checkpoint-5 direct-AHB transport.
6. Gamescope's existing wlroots renderer wraps/locks real wlr_buffers. Its
   `vulkan_create_texture_from_wlr_buffer` uploads SHM bytes into real Vulkan
   textures. Normal wlserver tracks wlr_surface commits and hands buffers to
   steamcompmgr, which constructs FrameInfo. A single-surface diagnostic needs
   the same wlroots compositor, renderer, surface commit/texture path and
   backend connector, without Xwayland, Steam, input, focus policy or effects.
7. Checkpoint 5 uses a separate diagnostic SurfaceControl consumer; it displays
   one exact GPU AHB for three seconds then replaces it and waits for Android's
   previous-release fence. Calling that function per frame cannot provide a
   continuous session. A persistent consumer must keep its control/window and
   current AHB reference, and release the previous AHB only after the next
   transaction's release fence signals.
8. Gamescope's renderer already rotates descriptor sets, resets command
   buffers, waits its real KHR timeline and retires completed commands. No
   reset-command-pool, reset-descriptor-pool or general Vulkan expansion is
   justified by this path. Producer fences can be created/exported/closed and
   destroyed per submitted frame. Native child ownership remains in the broker.

## Intended diagnostic design

Both flags stay outside production Launch. 6A and 6B use one initialized real
Gamescope renderer and the existing headless connector. Three 256x256 RGBA8
AHB targets match Gamescope's three-output-image convention. Ownership is
FREE -> RENDERING -> READY -> ANDROID_OWNED -> FREE, with the last transition
requiring the real previous-buffer release fence. Serialized presentation
bounds queued work; the held current frame stays visible between transactions.

6A changes deterministic layer placement through real FrameInfo/BLIT.
6B uses an isolated, automatically allocated socket in the existing guest
XDG_RUNTIME_DIR, with a real wl_shm client and Gamescope's wlroots surface
tracking/import path. It does not reuse or replace Android's wayland-0.
Client callbacks pace commits, and two SHM buffers are protected by release.
No user input, Steam, Proton, DXVK or production-session routing is included.

Keep Vulkan 1.1, real timeline/scalar/image-format-list extensions, FP32,
correctly typed descriptors and PASSTHRU. Read back selected frames only.
A 30 Hz deadline bounds production; late frames are not caught up in a burst.
Native resource snapshots must return to baseline, including the consumer,
AHB tokens and exported FDs. Client/server, Android consumer and Vulkan work
must drain before the corresponding objects are destroyed. Failure must retain
its original error and never print PASS.

## Implemented backend, client and environment

Patch 0120 adds two early main entrypoints and reuses the checkpoint-5 renderer
initializer. The existing `CHeadlessBackend` gets diagnostic-only Init and
connector Present hooks; stock initialization still runs with compatibility
mode off. `IBackend::Set`, PostInit and destruction are used. A FrameInfo with
one real source texture passes through `vulkan_composite` and the existing
FP32 BLIT shader on every frame. 6A moves a half-size quadrant texture.

The stock nested Wayland backend would add its WSI swapchain and normal
Android Wayland/AHB-protocol dependencies to the proven direct-AHB path.
Reusing the existing headless connector keeps that additional Vulkan/WSI
surface outside this checkpoint while exercising Gamescope's backend contract.
The small client-facing wlroots server reuses Gamescope's renderer/surface
adapter; it is not a replacement for DroidDeck's normal Android Wayland server.

6B's `mali_session_wayland.cpp` uses the same `vulkan_renderer_create`,
`VulkanWlrTexture_t`, `wlr_compositor_create`, wlr_surface commit/destroy events,
`wlr_surface_get_texture` and `vulkan_create_texture_from_wlr_buffer` as normal
Gamescope. It tracks one actual wl_surface and constructs the single layer
from its committed wlr_buffer. It is a diagnostic roleless surface, with no
xdg-shell/window management, focus, input, Xwayland or steamcompmgr thread.
This deliberately establishes the surface -> renderer -> existing backend
boundary; it does not yet run arbitrary desktop application sessions.

The server binds `gamescope-mali-<Gamescope PID>` in XDG_RUNTIME_DIR (the
existing GuestCommand private runtime bind, guest `/run/droiddeck`). It prints
the socket and spawns the same staged executable with an internal
`--mali-shm-client <socket> <frames>` entrypoint. The child connects with that
explicit socket name. No global WAYLAND_DISPLAY/DISPLAY changes are required;
Android's wayland-0 and the normal Launch environment remain separate.
No extra client binary or APK rebuild between buttons is needed.

Client input is two 256x256 ARGB8888 SHM buffers from one CLOEXEC memfd. The
client paints a moving 64x64 square cycling red/green/blue/white; attaches,
damages and commits, then waits for wl_surface.frame before advancing. It
also requires wl_buffer.release before rewriting a SHM buffer. The actual
Gamescope upload converts mandatory wl_shm ARGB/XRGB CPU input to RGBA8 for
the narrowly supported Vulkan texture format. wl_shm is the input transport;
all output is still computed by the Gamescope shader into native GPU AHBs.

The last callback is awaited, then a null attach/commit releases the last
surface buffer. The client checks commits == callbacks == buffer releases,
no pending callback, destroys its objects, unmaps/closes SHM and disconnects.
The server requires matching commits/presents, surface destruction and child
exit 0, and destroys clients/display/renderer. A watchdog bounds missing
Wayland commits and child exit; failures print protocol/errno or child status.

## Output ownership, synchronization and pacing

Three imported RGBA8 storage-capable AHBs are allocated once; their Vulkan
image/view/allocation identities remain stable for the session. The tested
`MaliOutputPool` enforces Free -> Rendering -> Ready -> Android -> Free.
Acquisition without a free slot returns backpressure before recording any
GPU work. With serialized transactions there is one held Android output
between presentations; a transaction temporarily references new and previous
buffers while it replaces them. The measured maximum simultaneous Android
output ownership is two during a swap, and one between swaps. The native
STATS result supplies this peak; the pool leaves spare rendering targets.

The native consumer retains its SurfaceControl, window, retirement AHB and
current producer AHB. Each transaction transfers a duplicate of the borrowed
producer sync FD to Android. OnComplete obtains present and previous-release
fences; all returned FDs are closed. The previous AHB reference is dropped only
after its release fence signals, and that exact previous token is returned
by PRESENT. END replaces the last producer with a retirement buffer, detaches
the control and waits for final release. Surface generation changes fail the
session, including destruction/recreation that returns the same native pointer.

The broker separately rejects submission referencing its Android-held image
and rejects destroying/freeing the held image/allocation/token. A completed
Vulkan timeline or producer fence cannot authorize Android buffer reuse.
Broker disconnect and native device destruction retire the consumer before
Vulkan children/AHB allocations are destroyed.

Every submit uses the existing native KHR timeline with strictly increasing
values. Gamescope reuses/reset completed command buffers, its upload ring and
24 descriptor sets. A fresh exportable binary producer fence is created for
each output submit, exported, waited, presented, closed and destroyed. Command
identity is retained through handoff and retired afterward. This needs no new
Vulkan entrypoint, fence reset, command-pool reset or descriptor-pool reset.

6A defaults to 300 presented frames; 6B defaults to 150 committed/presented
frames. A steady-clock 33,333 microsecond deadline targets 30 FPS. Late frames
reset the deadline rather than building an unlimited catch-up queue. Callbacks
are sent after presentation and pacing. Actual duration/FPS and late counts
are reported; performance is not an acceptance gate. MALI_SESSION_FRAMES is a
bounded 3..3000 override for automated tests and is not set by the Android UI.

Frames 1, 30, 60 and the final frame copy the actual output AHB image to a GPU
readback buffer. Every pixel is compared to the expected changing pattern and
a hash is logged. No CPU-generated output or presentation copy is substituted.
Other frames do not perform full-frame readback. Initial low-level logging is
throttled after three frames; progress is summarized every 30 frames.

## Private protocol and accounting

The wire stays v7 with an explicit session BEGIN opt-in. Legacy v1–v7 requests
are unchanged. Separate `vkDroidDeckSessionTEST` avoids changing the existing
checkpoint-4/5 private interop output ABI; it is not a Vulkan extension or a
claim of Vulkan 1.2. Four narrowly validated, device-owned RPCs are added:

| Opcode | Operation | Payload/result |
|---|---|---|
| 83 | BEGIN | device -> initialize persistent consumer |
| 84 | PRESENT | device, AHB token, producer sync token -> previous released token |
| 85 | END | device -> last released token |
| 86 | STATS | device -> live counts and presentation/export totals |

STATS returns 100 wire bytes: twenty 32-bit live counts and five 32-bit totals
(presented, released, exported FDs created/closed and peak Android ownership).
The separate private C output also has a released-token field; its size is 104
bytes. Neither changes the older interop ABI.

STATS covers devices, queues, command pools/buffers, semaphores, fences,
buffers, memory, images, views, samplers, descriptor pools/sets, shaders,
pipelines, descriptor/pipeline layouts, AHB tokens, sync tokens, open exported
FDs and pending commands. Descriptor and pipeline layouts share the layouts
count. Export totals count actual nonnegative native SYNC_FDs; a valid -1
export denotes already signaled completion and does not create an open FD.
Platform-transferred duplicates belong to Android; present/release FDs are
short-lived consumer-owned FDs and are always closed before an RPC returns.
The asynchronous consumer contract independently checks /proc/self/fd baseline
and AHB/window reference balance through repeated swaps and surface loss.

The connection baseline has no diagnostic device/children. BEGIN logs the
already initialized renderer counts. Before device destruction, after the
consumer/output/client teardown and renderer child destruction, Gamescope
requires all child counts zero, presentations == releases and exported FDs
created == closed. The remaining one device/one implicit queue are then
synchronously destroyed; the broker logs their zero baseline. A final cached
physical-device enumeration checks the proxy's retained destroy acknowledgement,
so a failed void vkDestroyDevice transport cannot produce PASS. Mock native
allocation/destruction counters independently assert exact balance.
The app-global query loader/UI/broker listener are outside this per-connection
baseline. Bounded proxy stale-handle tombstones are freed with the device.

## Stop, errors and limits

SIGTERM/SIGINT stop new frames and run normal cleanup, returning nonzero with
STOPPED rather than PASS. The existing Android “Stop broker and close” removes
the preview; its generation check fails the next presentation and triggers
cleanup, then the shared worker stops the broker. Client disconnect, missing
surface/commit, preview loss, broker disconnect and Vulkan failures stop
production and return nonzero. Vulkan results, including VK_TIMEOUT and
VK_ERROR_DEVICE_LOST, are preserved; Wayland errno/protocol errors and child
termination status are printed. Failure cleanup first removes the client/server
and backend, then requests safe broker-owned device/child cleanup.

Checkpoint 6 uses one five-second monotonic release-wait budget at stop,
covering retirement transaction allocation, SurfaceControl OnComplete and the
real previous-release fence. Persistent in-flight replacement waits also use a
bounded budget so stop cannot become stuck behind an earlier missing release.
Checkpoint 5's consumer code and waits are unchanged.

Before waiting, production stops. Success logs outstanding-at-stop, released
and timed-out counts. A timeout is sticky, returns VK_TIMEOUT/nonzero, prints
every still-owned output token, last frame, presentation/release totals and
available sync-token/fence state, and requires `release timeouts=0` for PASS.
Neither the pool nor native accounting treats timeout as release. During a
failed replacement, both old and newly published outputs remain protected.

After timeout the native broker drains GPU work and destroys safe children,
including unowned outputs and producer sync FDs. It quarantines the exact
unreleased image/allocation/AHB, consumer callback/control storage and required
device/instance/library parents; the worker and Gamescope can exit without
unsafe destruction or an indefinite release wait. These Android-process-owned
resources remain retained until Android process teardown, even if a late
callback subsequently arrives. Guest process exit alone cannot reclaim them.
This exceptional retention is reported as failed cleanup, never a zero baseline
or PASS. Normal successful cleanup still returns every count to baseline.

The release tests override the native wait budget to 200 ms. They cover normal
and delayed release, missing release/callback, retirement allocation failure,
sticky failure, rejection of further presentation and a stalled two-buffer
replacement. Broker integration verifies that only unreleased output allocations
and their parents survive, both 6A/6B fail nonzero, and no release/PASS is faked.

This checkpoint does not change normal Launch, advertise missing Vulkan
features, add input/Steam/Proton/DXVK, prove desktop window policy, or claim
phone validation. The real Mali compiler, AHB reuse/queue-family transitions
and Android motion still need the phone procedure below.


## Validation and one-build phone acceptance

Host Vulkan execution uses a model, not a GPU. The tests compile/link the actual
Gamescope renderer TU, backend.cpp, HeadlessBackend.cpp and diagnostic wlroots
integration against actual Wayland client/server and pinned wlroots 0.20.2.
The asynchronous SurfaceControl contract compiles the actual native consumer
with platform mocks, delayed release fences, reference assertions and FD counts.
Default-length runs cover 300 output frames and 150 actual client commits.
Fault cases include late preview/consumer error, VK_TIMEOUT, native
VK_ERROR_DEVICE_LOST, abrupt real client death, SIGTERM stop, live broker
shutdown and a lost device-destruction acknowledgement. All failures must
return nonzero and omit PASS. The pool contract forces exhaustion, illegal
transitions and 600 recycling iterations; native guards independently reject
Android-owned image submission/destruction.

Reproduce the host suites with the dependencies listed in test_renderer.py and
a real minimal build of pinned wlroots (set WLR_BUILD to its build directory,
which contains libwlroots-0.20.so and generated protocol headers):

```sh
export GAMESCOPE_SOURCE=/path/to/pristine/gamescope-3.16.29
export WLR_HEADERS=/path/to/pinned/wlroots/include
export WLR_BUILD=/path/to/pinned/wlroots-build
export VULKAN_HEADERS=/path/to/Vulkan-Headers/include
export JAVA_HOME=/path/to/jdk
python3 -m unittest discover -s tools/mali-vulkan -p 'test_*.py'
python3 -m unittest discover -s tools/gamescope -p 'test_*.py'
python3 -m unittest discover -s tools/tests -p 'test_*.py'
```

Local Unix sockets are required. Existing repository launcher tests assume a
workspace path without spaces; their two path-splitting failures are unrelated
to Mali. A temporary source copy without spaces passes the repository suite.
No launcher fixes are included in this checkpoint.

Checkpoint 6 baseline validation before the bounded-release follow-up:

| Check | Result and scope |
|---|---|
| All Mali host tests | 73 passed, 80.166 s; includes all checkpoint-1–5 tests |
| ARM/QEMU tests | 65 passed, 94.427 s; actual AArch64 broker/consumer with native mocks, real host Wayland integration |
| Gamescope tests | 25 passed, 6.141 s; complete strict patch application and reverse checks |
| Repository tests | 297 run, two existing path-with-spaces failures, one skipped; same suite passes from a temporary path without spaces (297 run, one skipped) |
| Android NDK r27d | AArch64 API-26 shared broker/consumer built with `-Wall -Wextra -Werror` and 16 KiB ELF alignment |
| AArch64 glibc | Proxy ICD, loader test and capability inventory built with strict warnings |
| Gamescope cross-compilation | Complete actual renderer, backend, HeadlessBackend, diagnostic Wayland and main translation units compiled for AArch64; real vendored sol/LuaJIT headers used |
| Complete packaged Gamescope/APK | Not built locally; ARM container exits with `exec format error` on the x86_64 host without ARM binfmt. Native ARM Actions build is required |
| Workflow/scripts/diff | actionlint, shell syntax, Python compilation and `git diff --check` passed |

Cross-compiling these translation units does not establish successful linking
of the full packaged Gamescope or Kotlin/APK compilation. QEMU/mock results do
not establish real Mali dispatch or Android continuous presentation. Those
remaining checks are the Actions and phone acceptance gates below.

Bounded-release follow-up validation: 77 Mali tests passed (82.892 s), including
12 Checkpoint 6 tests; 25 Gamescope tests passed (6.352 s). The strict Android
NDK AArch64 API-26 shared build, strict source-whitespace/zero-fuzz patch replay
and `git diff --check` passed. The ARM/QEMU suite above was not rerun for this
follow-up. Native release fault tests use a 200 ms budget, including a permanently
missing fence/callback and a two-buffer in-flight replacement timeout.

One GitHub Actions run:

1. Once these reviewed changes are in your remote mali-gpu-experiment branch,
   run **Build APK** (`.github/workflows/build.yml`) for that branch. No commit
   or push is performed by this implementation task.
2. The **Build experimental Gamescope** ARM job builds the entire patch stack,
   including 0120, and smoke-tests both new options with an intentionally missing
   ICD. Both must exit before normal startup with the expected Vulkan error.
3. The APK job stages that same gamescope-patched artifact, checks both option
   markers, builds the updated glibc proxy and Android native library, and signs
   the APK. Its APK-content check must report the updated Gamescope as current.
   Download **droiddeck-apk**, containing `app-release.apk`, from this same run.
   This branch artifact is signed with the public test key; it is separate from
   the release-key signing job that runs on main. Use an installation with a
   compatible signer and preserve existing app data if the phone uses a different
   signer. Use this run's APK, not an older pinned Gamescope.
4. Both new buttons and all checkpoint-1–5 buttons are in this APK; no second
   build, runtime selection change or production Launch step is required.

Exact phone order (same APK, same broker Activity):

```sh
adb install -r /path/to/signed-DroidDeck.apk
adb shell am start -n com.droiddeck.launcher/.gpu.SystemVulkanBrokerActivity
adb logcat -v threadtime -s MaliVulkanBroker MaliProxyICD > checkpoint-6-phone.log
```

Keep the Activity and its fixed preview visible. Ensure the Linux runtime is
installed. Press each button separately and wait for its complete result:

1. **Run Gamescope first frame test**: require the phone-proven 5B path, zero
   mismatches, sequence/counter 4/4, exact AHB presentation and exit 0.
2. **Run Gamescope persistent session test**: visibly moving quadrant rectangle,
   300 rendered/presented/released frames, selected readbacks zero mismatches,
   monotonic native timeline, pool size 3, peak Android ownership 2, zero reuse
   violations, zero sync leaks, acknowledged zero resource baseline, PASS/exit 0.
3. Repeat the same 6A button, without rebuilding or restarting the broker.
4. **Run Gamescope Wayland client test**: visibly moving color-cycling square,
   real socket/client PID, 150 wl_surface commits, 150 frame callbacks and
   150 wl_buffer releases; surface removed, client exit 0, 150 GPU output
   presentations/releases, correct selected readbacks, zero baseline, PASS/exit 0.
5. Repeat the same 6B button, without rebuilding or restarting the broker.

Both 6A and 6B must visibly update throughout; final-frame-only presentation
is not acceptance. A temporary black/paused preview, stale content, any mismatch,
missing release, error, stalled cleanup or nonzero exit is not phone PASS.
Optionally close the Activity during a run to exercise its existing stop path;
that run should drain safely and report nonzero, then reopen the Activity.
Stop the logcat capture and retain the complete log and UI result for each run.
Checkpoint 6 remains **phone acceptance pending** until this sequence succeeds.
