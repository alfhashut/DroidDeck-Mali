# DroidDeck-Mali 0.5 port: Phase 3 normal presentation

Base: Phase 2 commit `cc58bcd5` on `mali-0.5-port`.
Reference: `mali-gpu-experiment` (`3e1d9255`), with the normal Gamescope core
from its pre-performance/pre-Steam checkpoint `7fd567ad`.

The added route is Gamescope/glibc → proxy ICD → service-owned Android/Bionic
broker → real system Vulkan → Mali-G52 → AHardwareBuffer/SYNC_FD → the current
DroidDeck Android compositor. The initial Phase 3 client was the reference
interactive SHM diagnostic. The current non-Steam Desktop route reuses the normal
KDE launcher inside this bridge, as described below.

**Desktop follow-up and current device evidence**

The authoritative script is
`tools/linuxfs/overlay/usr/local/bin/droiddeck-session`. Gradle's
`stageSessionScripts` packages it into the managed session assets, and
`SessionFiles.stage()` reinstalls it during launch preparation. Installed-runtime
edits are therefore temporary; this follow-up changes the repository source.

The `mali-wayland` entry now starts:

```sh
BL_MALI_DESKTOP=1 gamescope --backend wayland --mali-wayland-session --expose-wayland -f \
  -W "$BL_WIDTH" -H "$BL_HEIGHT" -- /usr/local/bin/droiddeck-session lxqt
```

`lxqt` is the existing 0.5 internal Desktop mode. It runs the existing desktop
preparation and `/usr/local/bin/droiddeck-desktop`, which starts
`dbus-run-session -- startplasma-wayland`. No second KDE launcher or renamed mode
is introduced. The child leaves `BL_INSIDE` unset so that the existing direct
Desktop branch executes; the Steam Mali/Xwayland route retains its own
`BL_INSIDE=1` child setup and exact command.

Only this Mali desktop child gets a private `$XDG_RUNTIME_DIR/mali-desktop`
directory (mode 0700). Its `wayland-0` symlink points to the nested socket supplied
by Gamescope, accepting relative and absolute `WAYLAND_DISPLAY` values. This
preserves KDE's existing parent-at-wayland-0 / own-socket-at-wayland-1 convention.
Desktop socket cleanup stays within that private directory and cannot unlink the
Android display, Gamescope's nested display or broker socket. A missing parent
socket fails startup. The ordinary non-Mali Desktop route is unchanged; new KDE-script checks
are gated to the explicit Mali desktop child. The native `gamescope --mali-interactive-client` diagnostic remains
available separately; it is no longer the normal Desktop child.

User-reported real Mali-G52 validation proves the interactive Mali client at
approximately 60 FPS, clean Stop without stale Gamescope/proot/session processes,
and successful relaunch. Shared staging and PanVK were OFF. The subsequent KDE
device run establishes: **renderer initialization PASS; KDE routing PASS; KDE
launcher reached PASS; first KDE frame FAIL/PENDING; required host protocols missing.**
KDE reaches startplasma-wayland, KWin, kcminit and desktop portals, but Gamescope
reports commits=0, frames=0 and presented=0 before a clean watchdog teardown. No
Android crash was reported. This is not successful KDE device validation.

**Socket alias and cold-start watchdog investigation**

For the reported generated `wayland-1` socket, the intended paths are:

```text
/run/droiddeck/wayland-0                 Android compositor (untouched)
/run/droiddeck/wayland-1                 Gamescope nested server
/run/droiddeck/mali-desktop/wayland-0 -> /run/droiddeck/wayland-1
/run/droiddeck/mali-desktop/wayland-1    KWin's own socket
```

The desktop child exports `XDG_RUNTIME_DIR=/run/droiddeck/mali-desktop` and
`WAYLAND_DISPLAY=wayland-0`. KDE reporting wayland-0 is therefore expected: it
names the private alias, not Android's socket. The parent is taken from Gamescope's
actual generated socket name, not assumed to be 0 or 1. The reported lockfile
warning for outer wayland-0 is followed by successful allocation of wayland-1;
it is not evidence that the private alias points to Android. Only the private directory
is cleaned, with `wayland-0*` excluded. New Mali-only checks/logs verify the alias
at handoff, after desktop cleanup, before Plasma and immediately before executing
KWin. A missing/non-socket alias fails cleanly instead of falling back to Android.
The logs prove path existence at those stages; they do not prove KWin submitted a
frame. The latest device logs now prove alias survival at all three checkpoints.

A host socket test now runs the actual session, desktop and KWin wrapper scripts
with filesystem paths redirected and mocked KDE binaries. It completes a socket
handshake from the wrapper's parent alias to the mock Gamescope server after
cleanup and Plasma entry. It repeats with generated names wayland-7 and absolute
wayland-11, preserves Android/broker socket inodes, creates KWin's separate socket,
and proves alias loss fails before Plasma. These are host proofs, not real KWin
protocol or phone rendering tests.

The old watchdog starts after StartServer forks the child, before the child's
locale/font/desktop/Plasma preparation, and resets only on successful Present.
The reported child start at 00:57:39 and watchdog failure at 00:57:54 match that
15-second limit, leaving about 13 seconds after the desktop step at 00:57:41.
The later 90-second device run still produced zero commits and frames. Startup
delay alone therefore does not explain this result.

New patch `0219-mali-desktop-first-frame-grace.patch` gives only the explicit
`BL_MALI_DESKTOP=1` plus `droiddeck-session lxqt` non-Xwayland child up to 90 seconds
for its first successful Present. Then it uses the original 15-second no-progress
limit from the latest successful Present. Client connection, surface creation,
commits and setup messages cannot prolong startup. Failure remains bounded at 90
seconds if no frame arrives. Interactive diagnostics retain their original
15-second startup/progress behavior, even if the desktop flag is inherited with a
different child command. Steam's existing Xwayland behavior is unchanged. Child
exit, outer-display failure and Stop are still handled independently of the timer.
At that checkpoint all 27 earlier patches were unchanged; 0219 followed 0218.

**Confirmed Wayland compatibility gap and 0220 implementation**

The device registry contained only `wl_shm`, `wl_compositor` and `wl_seat`.
The custom `StartServer` bypasses normal Gamescope's `wlserver_init()`, which
initializes additional protocol handlers. Its raw-surface listener also rejects
a second surface, whereas KWin uses a single-pixel background, SHM content
subsurfaces and cursor surfaces. The desktop build pins KWin 6.7.5 in `tools/desktop-kde/build-kwin.sh`; its
local nested-scale patch leaves backend initialization requirements unchanged. Upstream [KWin 6.7.5 WaylandDisplay::initialize](https://github.com/KDE/kwin/blob/v6.7.5/src/backends/wayland/wayland_display.cpp)
requires compositor, subcompositor, xdg-shell, single-pixel-buffer, viewporter,
seat, pointer constraints and presentation-time. Six required globals were absent:
`wl_subcompositor`, `xdg_wm_base`, `wp_single_pixel_buffer_manager_v1`,
`wp_viewporter`, `zwp_pointer_constraints_v1` and `wp_presentation`. This is a
confirmed initialization blocker. It does not prove there are no later blockers.
The proot/systemd/priority/FUSE/portal warnings are not assigned as root cause.

New overlay `0220-mali-kwin-host-protocols.patch` supplies these six globals using
the exact Gamescope submodule's wlroots 0.20.2 APIs (commit
`d783533489e1f75d6886c2ab5c5960090ef268f8`). Normal Gamescope already reuses
wlroots' xdg-shell and pointer-constraint handlers; the other handlers are also
available in this wlroots. No protocol servers are invented for the nested
registry. wlroots handles roles, configure/ack, viewport validation, synchronized
subsurfaces and object cleanup. Constraints may be requested and destroyed but
remain inactive by compositor policy. A single nested KDE output is configured
to the existing Gamescope output size.

Only the explicit KDE child uses wlroots' scene graph and Pixman renderer to
compose its surface tree, including the single-pixel background and cursor,
into one SHM frame. Existing Gamescope Vulkan upload/composite and Android AHB
presentation consume that frame. No new GPU driver or rendering backend is
selected. This adds CPU composition overhead that requires device measurement.
The interactive raw-surface renderer and Steam/Xwayland path remain unchanged.

wp_presentation feedback is sampled for the composed surfaces and forwarded
from actual outer Android presentation events. The metadata-only wl_output has
no synthetic presentation timer; ZERO_COPY is cleared. Discard and shutdown
destroy outstanding feedback, with at most 64 pending outer requests. Frame
callbacks follow successful existing Mali Present operations. No AHB reuse or
release acknowledgment depends on these new feedback objects.

The desktop captures the complete, unfiltered parent registry in
`$BL_DEBUG_DIR/mali-desktop-wayland.txt`, now including runtime/display context
and the wayland-info exit status. Socket names remain generated dynamically;
the private alias, cleanup exclusions and KWin wrapper arguments are unchanged.
0219's explicit 90-second first-frame grace, subsequent 15-second watchdog,
interactive timeout and Steam exemption are unchanged. All prior patch contents
remain byte-identical; the stack now ends with 0218, 0219, 0220.

Latest real-device evidence before 0220:

- Renderer initialization, KDE routing and launcher reached: PASS.
- Socket alias through cleanup, before Plasma and KWin wrapper entry: PASS.
- Explicit 90-second allowance: PASS; zero commits/frames throughout.
- Captured registry: insufficient for KWin 6.7.5.
- First KDE frame: FAIL/PENDING.
- Protocol compatibility implementation: pending CI/device validation.

CI must compile/link the complete ARM64 Gamescope stack and run the new real
wlroots bind/use tests alongside cursor routing, SONAME, fingerprint and APK
checks. The host protocol fixture checks configure/ack and popup lifecycle,
SHM/single-pixel subsurface pixels, sync/desync, viewport cropping, pointer
constraint requests, frame callbacks, presentation timestamp/sequence/flag
forwarding, discard, unmap/disconnect and invalid-buffer protocol rejection.
It uses a mock outer presentation server, not Android or real KWin. Existing
launcher tests check dynamic names wayland-7/11, protected outer socket inodes,
alias survival and failure on alias loss; watchdog and Steam regressions remain.

On the phone, record `pacman -Q kwin`, the full parent registry and KWin backend
stderr. Verify all required globals, xdg configure/ack, first SHM/subsurface
commits, a visible first KDE frame, input/cursor, sustained rendering and actual
outer presentation/release counts. Exercise Stop and relaunch with no stale
processes/sockets and balanced release accounting. Keep shared staging and PanVK
OFF. A successful host fixture is not successful KDE device validation.

0220 follow-up files:

| File | Purpose |
| --- | --- |
| `tools/gamescope/patches/0220-mali-kwin-host-protocols.patch` | Add the desktop-only wlroots protocol/scene adapter, outer presentation feedback wiring and small C client/server tests. |
| `tools/gamescope/build-in-arch.sh` | Run both new protocol tests alongside cursor routing after the CI build. |
| `tools/linuxfs/desktop/droiddeck-desktop` | Record registry context and query status around the existing full wayland-info capture. |
| `tools/mali-vulkan/test_desktop_protocols.py` | Run real protocol fixtures when public wlroots headers/library are supplied; enforce overlay scope, unchanged older patches and legacy paths. |
| `tools/mali-vulkan/run_phase3_tests.py` | Include the protocol suite in Phase 3 host checks. |
| `tools/mali-vulkan/test_desktop_startup.py` | Retain deadline/alias tests, check the expanded registry artifact and allow the new desktop import adapter while freezing existing GPU presentation code. |
| `tools/mali-vulkan/test_normal_session.py` | Link the new C adapter in the existing optional full normal-session host fixture; this heavier fixture was not run locally. |
| `tools/mali-vulkan/test_phase3.py` | Require 0220 last and check full-stack application without fuzz/offsets. |
| `tools/gamescope/PATCHES.md` | Document the protocol implementation, composition, timing and new patch order. |
| `docs/development/mali-phase3.md` | Record the confirmed device blocker and separate host results from pending KDE validation. |

Lightweight verification: 65 tests passed across `test_desktop_protocols`,
`test_desktop_startup`, `test_steam_launch`, `test_normal_launch`, `test_phase3`,
`test_phase5` and `test_phase6`. The two protocol executions compiled only the
new C adapter and fixture against a public prebuilt wlroots 0.20.2 library; no
Gamescope executable, Android code or driver was built. Final patch scope/order
contracts passed again after preserving Meson's existing enable_tests switch.
Python and shell syntax checks and tracked/new-file `git diff --check` passed.

No broker/proxy opcode, Vulkan synchronization, AHB/release ownership, PanVK,
shared-staging gate or Steam client behavior changes. Shared staging and PanVK
remain OFF for the device follow-up. There is no local Android/Gamescope/NDK build
or claim of an additional device result. CI must compile the new Gamescope patch
and package all three managed scripts from one matching source revision.

**Startup-investigation changed files and checks**

| File | Change |
| --- | --- |
| `tools/gamescope/patches/0219-mali-desktop-first-frame-grace.patch` | Add the scoped first-frame watchdog and pure deadline helper; preserve all 27 existing patch bytes and rendering/cleanup code. |
| `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` | Verify and log the existing private socket alias after handoff; keep routing and transport options unchanged. |
| `tools/linuxfs/desktop/droiddeck-desktop` | Add Mali-only alias checks after cleanup and before Plasma, plus parent-global capture; ordinary Desktop behavior is unchanged. |
| `tools/linuxfs/desktop/kwin_wayland_wrapper` | Verify/log the same alias immediately before KWin; retain all existing wrapper arguments and non-Mali behavior. |
| `tools/mali-vulkan/test_desktop_startup.py` | Compile only the standalone deadline helper, test virtual-time bounds and explicit route selection, and exercise real launcher scripts with mocked KDE and socket handshakes. |
| `tools/mali-vulkan/run_phase3_tests.py` | Include the focused startup tests in the existing CI host suite. |
| `tools/mali-vulkan/test_phase3.py` | Extend explicit patch order through 0219 and retain zero-fuzz/zero-offset/header/ownership checks. |
| `tools/gamescope/PATCHES.md` | Document 0219, exact scope and the unresolved KWin protocol gap. |
| `docs/development/mali-phase3.md` | Record this diagnosis, actual device status, socket paths, test results and remaining validation. |

Lightweight results: **55 focused startup/launcher/patch/isolation tests and 38
existing Desktop/Steam/x86/asset/runtime-input tests passed**. The complete
28-patch stack applies to pristine pinned Gamescope 3.16.29; all Mali hunks apply
with zero fuzz and zero offset. Canonical Vulkan/proxy/AHB headers remain equal.
Three changed shell scripts pass `bash -n`; three changed/new Python files parse.
All-file `git diff --check` includes the new patch and test through a temporary
index; no files are staged or committed. The small C++ fixture exercises real
watchdog code with a synthetic monotonic clock, without waiting 90 seconds or
compiling Gamescope. No NDK, APK, Mesa or PanVK build ran locally.

On the next matching CI/device run require the 90-second KDE-startup log, the
handoff/after-cleanup/before-Plasma/KWin alias checkpoints, parent-global capture
and full backend error log. A successful frame must log the transition back to
15 seconds, followed by real nonzero commits/frames/presented/releases. Test Stop
during startup, bounded no-frame failure at 90 seconds, post-first-frame hang
failure at 15 seconds, and clean restart. Repeat the unchanged interactive client
and Steam routes. Resolve and validate the missing protocol support before
claiming KDE presentation success; neither grace nor mock socket tests establish it.

The remaining sections record the original Phase 3 scope and checks. Later phases
add the documented optional Steam/PanVK/shared-staging paths; their defaults and
implementation are unchanged by this desktop follow-up.

**Launch scope**

The automatic entry is the existing non-Steam Desktop action, only for ARM vendor
`0x13b5`, device `0x74021000`, Vulkan >= 1.1. That action selects `mali-wayland`
on the validated device. Other GPUs retain their requested desktop route. Steam,
Steam-desktop requests, SteamVR, game URLs and program launch requests retain 0.5
routing. The wider redirection and Steam backend at the reference tip are deliberately
omitted: they depend on later Xwayland/client work outside this phase.

`MaliSessionSelection` uses numeric IDs, not the GPU name, and rejects query errors,
missing fields, malformed API versions and other Mali device IDs. The broker repeats
the hardware check against a real inventory reply before staging the proxy and
starting the guest. Shared staging is forced to `0`; the normal shell entry rejects
an enabled value. PanVK remains separate. No proprietary Mali library is copied.

**Ownership and 0.5 adaptations**

- `SystemVulkanBroker` grants an exclusive normal lease. Every diagnostic command,
  diagnostic start and diagnostic stop rejects that lease. A failed native stop
  retains the wrapper lease; a quarantined session prevents another normal start.
- `MaliNormalBroker` waits for an actual connectable Android Wayland socket, then
  performs the reference v1 inventory handshake with bounded reads. It stages the
  existing glibc proxy/manifest atomically, uses the Phase 2 manifest validator and
  checks the ELF architecture. Its normal transport remains wire v7 with unchanged
  IDs, layouts, same-UID checks and disconnect cleanup from Phase 1.
- `SessionService` uses current `LinuxRuntime.command`, persistent
  `LinuxRuntime.prootTempDir`, `SessionFiles.stage`, `liveDir/shm`, session log folders
  and `SessionEvents`. No runtime API, including `debugLogDir()`, is restored.
  The ordinary session body, maintenance gate, storage handling, driver selection,
  terminal setup, Steam/download/store/agent behavior and cleanup remain in place.
- Startup preparation and normal teardown share `maliStartupLock`; cancellation
  cannot finish and admit a restart while the old startup is still staging files
  or acquiring its broker. Guest PID registration uses `stopLock` and generation
  checks. STOPPING is published under that lock, closing the launch race.
- Stop sends SIGTERM to the identified Gamescope descendant and allows seven
  seconds for its real five-second Android release drain. PID start times protect
  against reuse. Generic process cleanup follows; only then does the service stop
  its broker. Idle/failed completion, suspend resumption, foreground/lock cleanup,
  store teardown and newer 0.5 finalization remain through the current APIs.
  A release timeout reports failure and retains/quarantines resources, rather than
  declaring a buffer reusable. Backend/start-time state is cleared for restart.
- The current compositor links the same `maliahb` shared library as the broker,
  preventing two independent registries. `banner_ahb_v1` v3 creates an opaque-key
  AHB-only `wl_buffer`, with zero DMA-BUF planes and all plane FDs unset. Versions
  1/2 retain their existing behavior. Single-pixel buffers and 0.5 Quest/VR/Cardboard,
  scene composition, presentation timing and ordinary DMA-BUF paths are retained.
- The normal display uses system Vulkan 1.1 on the exact validated device. It
  requests real supported extensions, advertises no DMA-BUF formats, and disables
  unsupported HDR/frame-generation use on this route. Other devices keep the
  current 1.3/custom-driver behavior. Existing SurfaceControl release callbacks
  already provide the needed release fences; `sc_layer.c` needs no behavioral port.
- The compositor rejects unpublished/retired AHB presentation before an Android
  transaction. This adds a fail-closed check to the reference handoff. Real readable
  release fences acknowledge ownership; error events do not. Watcher allocation
  failure retains the buffer. Cancellation before display and actual release remain
  distinct, and destroying a Wayland object never acknowledges Android completion.

**Why each existing file changes**

| File | Required change |
| --- | --- |
| `.github/workflows/build.yml` | Install Wayland development headers for the small release harness and run Phase 3 contracts; preserve current build/cache/overlay/runtimeAssets/validation logic. |
| `app/src/main/cpp/CMakeLists.txt` | Define the existing shared registry before entering the compositor subdirectory. |
| `app/src/main/cpp/waylandcomp/CMakeLists.txt` | Link `maliahb`; retain a standalone target fallback without duplicating the registry inside either consumer library. |
| `app/src/main/cpp/waylandcomp/protocols/banner-ahb-v1.xml` | Extend the existing protocol to v3 with the opaque broker-buffer request. |
| `app/src/main/cpp/waylandcomp/generated/banner-ahb-v1-protocol.c` | Update the generated request table, signature, type table and version for v3. |
| `app/src/main/cpp/waylandcomp/generated/banner-ahb-v1-server-protocol.h` | Expose the matching generated server callback and since-version constant. |
| `app/src/main/cpp/waylandcomp/src/ahb_swapchain.c` | Import the exact registry AHB, validate publication, distinguish cancellation/submission, acknowledge real release fences and retain on errors. |
| `app/src/main/cpp/waylandcomp/src/compositor.c` | Create plane-free AHB buffers and recognize their content/fullscreen handoff while preserving current scene/Quest/single-pixel behavior. |
| `app/src/main/cpp/waylandcomp/src/droiddeck_ext.h` | Declare the compositor's AHB-only buffer factory. |
| `app/src/main/cpp/waylandcomp/src/vk_present.c` | Gate system-Vulkan 1.1/AHB-only behavior and real feature selection to the normal Mali display. |
| `app/src/main/cpp/waylandcomp/src/wl_dmabuf.c` | Advertise no DMA-BUF formats for the AHB-only route. |
| `app/src/main/java/com/droiddeck/launcher/MainActivity.kt` | Resolve the exact hardware gate for the non-Steam Desktop entry and reject launches while stopping; retain maintenance/storage/waiting logic. |
| `app/src/main/java/com/droiddeck/launcher/SessionActivity.kt` | Use system Vulkan for Mali, preserve live backend selection on reattachment, provide Mali loading titles, and avoid unsupported HDR/framegen features. |
| `app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanBroker.kt` | Add exclusive service leases and reject competing diagnostic commands, asset staging and teardown. |
| `app/src/main/java/com/droiddeck/launcher/session/SessionPrefs.kt` | Reuse fullscreen/Steam display preferences for the isolated normal mode. |
| `app/src/main/java/com/droiddeck/launcher/session/SessionService.kt` | Own normal startup, guest registration, stop/drain, broker lifetime and restart state using current 0.5 APIs. |
| `app/src/main/java/com/droiddeck/launcher/session/SessionState.kt` | Track the live Mali backend across Activity reattachment and teardown. |
| `app/src/main/res/values/strings.xml` | Add normal Mali starting/paused labels. |
| `tools/gamescope/PATCHES.md` | Document namespace, provenance, manual reconciliation, order and scope. |
| `tools/gamescope/build-in-arch.sh` | Require batch/zero-fuzz application without replacing packaging, cursor tests or SONAME checks. |
| `tools/gamescope/check-diagnostics.sh` | Verify new persistent/normal command markers and fail-closed entry smoke checks; same-run artifact validation picks these up. |
| `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` | Dispatch only explicit normal mode before ordinary setup; require broker/manifest/display and reject shared staging. All later 0.5 script behavior is unchanged. |
| `tools/mali-vulkan/test_diagnostics.py` | Permit the now-authorized normal lease owner while keeping shell diagnostic access isolated; cover the extended component smoke checker. |
| `tools/mali-vulkan/test_normal_launch.py` | Adapt the manifest-validator location; add actual native broker restart/socket tests and shared-staging refusal. |
| `tools/mali-vulkan/tests/normal_android_release.c` | Execute the unpublished-presentation rejection against the actual compositor code in addition to existing real Wayland event-loop/fence tests. |

The existing Phase 1 `malivulkan` ownership implementation, proxy and protocol files
already contain the required normal implementation and remain byte-identical.
No manifest changes, removed APIs, new broker JNI symbols, GPU detection changes,
wlroots tree, PanVK client wiring or Steam script changes are necessary.

**Gamescope reconciliation and final order**

0210 ports persistent renderer/output ownership and its diagnostic clients. Its
old `interop_test_api.h` insertion is omitted because 0204 already supplies the
same canonical ABI. 0211 ports the pre-performance normal core, manually merging
the touch-down guard into the current Wayland backend. Both patches are regenerated
against the exact 0.5 stack on pristine 3.16.29. Canonical normal headers match
Phase 1 exactly. The nested client receives no private proxy/broker environment.
Later staging, detailed performance, upload-ring, Xwayland/Steam and frame-journal
patches are omitted. Only ownership/error/final-cleanup accounting remains.

All previous patches retain their exact contents. Application order:

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
```

**Lightweight results**

- Phase 3: 26 passed. Real host-compiled native ownership/release code, a real
  Wayland event loop with eventfd release fences, real proxy/loader plus mock
  Android Vulkan, normal v7 protocol rejection and parent ownership, actual native
  broker restart/socket/blocked-shutdown checks, normal launch script execution,
  service ownership/source contracts, full patch application and output-pool execution.
- Phase 2 regressions: 63 broker/proxy/diagnostic tests and 25 diagnostic translation-unit
  tests passed against the full new stack. Only small diagnostic units were compiled.
- Phase 1 regressions: 101 Mali and 124 PanVK isolated contracts passed.
- Existing 0.5 session regressions: 23 passed, covering guest environment, temporary
  storage cleanup, cursor routing, run status, log handling and session assets.
- Existing Steam cold/desktop launch regressions: 16 passed.
- Python syntax, shell syntax, Android XML, workflow YAML and 36 embedded workflow
  shell steps passed validation. Existing diagnostic patches and all Phase 1 native
  broker/proxy protocol files remain unchanged. All-file `git diff --check` passes.

The Unix socket tests needed execution outside the filesystem sandbox after its
socket policy rejected the first attempts. The successful runs above use local
host mocks only; they do not prove Android Vulkan execution or real GPU presentation.
The four new Kotlin selection tests are added to the existing CI unit-test task;
no local Gradle, APK, full Gamescope or driver build was run.

Reproduce the Phase 3 suite with public Vulkan headers and pristine source:

```sh
python3 -B tools/mali-vulkan/run_phase3_tests.py \
  --gamescope-source /tmp/droiddeck-mali-phase2-source/gamescope-3.16.29 \
  --vulkan-headers /tmp/droiddeck-mali-phase2-headers/Vulkan-Headers-1.3.296/include
```

**CI and device validation still required**

GitHub Actions has not been triggered here. It must compile/link the complete ARM64
Gamescope Wayland/headless/renderer stack and APK; run the existing cursor-routing,
component smoke, SONAME and runtimeAssets checks; validate fingerprints/same-run
artifacts; verify that both native consumers load one packaged `libmaliahb.so`;
and run `testReleaseUnitTest`, including the new hardware-gate and existing manifest
validator tests. Full renderer/normal-session host fixtures in `test_session.py`
and `test_normal_session.py` also require a CI environment with Gamescope's pinned
wlroots and renderer dependencies; the lightweight runner deliberately does not
compile them locally or claim their results.

The original Phase 3 checklist below targeted the interactive client. The current
Desktop entry targets KDE and requires the follow-up validation described above.
For the retained diagnostic client, verify the broker handshake and real GPU
inventory, the actual Gamescope BLIT output through the proxy, touch/mouse/keyboard, exact AHB handoff and producer
SYNC_FD completion. Exercise normal Stop with a displayed last buffer, restart,
activity recreation/background/surface detach, startup cancellation, child/broker
loss and deliberately delayed/missing Android release. Confirm real release fences,
balanced normal teardown, no stale broker or nested-display sockets, and quarantine
rather than reuse on timeout. Surface/output-size transitions and optional display
settings require device evidence for this fixed-output nested core. Ordinary Adreno
Steam/desktop/game and Quest/VR/Cardboard regression checks still require their
hardware; their existing implementations have been retained.

Shared CPU staging, persistent staging performance work, PanVK client integration,
Steam/Xwayland/CEF/ANGLE acceleration, broad Mali device support and detailed
performance/frame-journal instrumentation remain deferred. No commit was created.

**Every added or modified file**

```text
M	.github/workflows/build.yml
M	app/src/main/cpp/CMakeLists.txt
M	app/src/main/cpp/waylandcomp/CMakeLists.txt
M	app/src/main/cpp/waylandcomp/generated/banner-ahb-v1-protocol.c
M	app/src/main/cpp/waylandcomp/generated/banner-ahb-v1-server-protocol.h
M	app/src/main/cpp/waylandcomp/protocols/banner-ahb-v1.xml
M	app/src/main/cpp/waylandcomp/src/ahb_swapchain.c
M	app/src/main/cpp/waylandcomp/src/compositor.c
M	app/src/main/cpp/waylandcomp/src/droiddeck_ext.h
M	app/src/main/cpp/waylandcomp/src/vk_present.c
M	app/src/main/cpp/waylandcomp/src/wl_dmabuf.c
M	app/src/main/java/com/droiddeck/launcher/MainActivity.kt
M	app/src/main/java/com/droiddeck/launcher/SessionActivity.kt
A	app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt
A	app/src/main/java/com/droiddeck/launcher/gpu/MaliSessionSelection.kt
M	app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanBroker.kt
M	app/src/main/java/com/droiddeck/launcher/session/SessionPrefs.kt
M	app/src/main/java/com/droiddeck/launcher/session/SessionService.kt
M	app/src/main/java/com/droiddeck/launcher/session/SessionState.kt
M	app/src/main/res/values/strings.xml
A	app/src/test/java/com/droiddeck/launcher/gpu/MaliSessionSelectionTest.kt
A	docs/development/mali-phase3.md
M	tools/gamescope/PATCHES.md
M	tools/gamescope/build-in-arch.sh
M	tools/gamescope/check-diagnostics.sh
A	tools/gamescope/patches/0210-mali-persistent-session.patch
A	tools/gamescope/patches/0211-mali-normal-wayland-session.patch
M	tools/linuxfs/overlay/usr/local/bin/droiddeck-session
A	tools/mali-vulkan/run_phase3_tests.py
M	tools/mali-vulkan/test_diagnostics.py
M	tools/mali-vulkan/test_normal_launch.py
A	tools/mali-vulkan/test_phase3.py
M	tools/mali-vulkan/tests/normal_android_release.c
```
