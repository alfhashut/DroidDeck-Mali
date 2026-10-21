# KDE host comparison and private socket ownership (0221)

The confirmed source bug is the private parent alias's missing socket lock.
KWin's native wrapper can replace that alias before the real KWin process starts.
The fix preserves the existing desktop launcher, native KWin wrapper and Mali
render/presentation implementation. It does not enable a different output backend.

## Evidence and scope

The latest phone run, with 0220, proves the complete required registry, the alias
at the shell wrapper entry, and the full 90-second startup allowance. It still
reports zero commits, frames and Android presentations. No Android crash occurred.
**A first KDE frame on the phone remains FAIL/PENDING.**

The audit used the current source and the `0.5.0` versions of the desktop scripts,
Gamescope 3.16.29 plus overlays through 0220, and the exact KWin 6.7.5 tarball pinned
by `tools/desktop-kde/build-kwin.sh`. The KWin source SHA-256 was verified as
`6baa910b732d93c48c90f9c1cc685cc93d0b8de0cdf138c24192c045bc3a48e2`.
The helpers and backend below are unchanged by DroidDeck's nested-scale patch.

## Actual launch paths

Normal Desktop:

```text
MainActivity / SessionActivity: select Desktop; await Android compositor
SessionService.runStandardSession: stage managed files, prepare environment/binds
LinuxRuntime: bind the Android runtime directory at /run/droiddeck
droiddeck-session lxqt: locale/tmp/fonts/readiness/desktop preparation
droiddeck-desktop: shared software desktop environment; dbus-run-session
startplasma-wayland -> managed kwin_wayland_wrapper -> native KWin wrapper
native wrapper -> kwin_wayland -> Plasma
KWin host = Android compositor's /run/droiddeck/wayland-0
```

The early `lxqt` branch executes the desktop launcher before Gamescope setup.
**Normal Desktop does not host KWin inside normal Gamescope.** Gamescope may be
used for games launched later from that desktop.

Mali Desktop:

```text
MainActivity / SessionActivity: validated Mali-G52 route; Android compositor ready
SessionService.runMaliSession: stage files, start/retain broker, isolated guest env
droiddeck-session mali-wayland -> Gamescope --mali-wayland-session
Gamescope creates a dynamic nested socket, e.g. /run/droiddeck/wayland-1
child droiddeck-session lxqt: reuse normal preparation and desktop launcher
private runtime /run/droiddeck/mali-desktop; wayland-0 aliases that nested socket
same droiddeck-desktop -> dbus-run-session -> startplasma-wayland
same managed/native KWin wrappers -> kwin_wayland -> Plasma
KWin host = private wayland-0 alias; KWin's own server = private wayland-1
```

The Mali child has its compositor-only proxy/broker/shared-staging variables
removed before exec. The client stays on the existing QPainter/software desktop
route. Gamescope still uploads and presents through proxy -> Bionic broker ->
Android system Vulkan -> Mali-G52 -> AHB/SYNC_FD -> Android compositor.

## Environment and preparation comparison

| Behavior | Normal Desktop | Mali Desktop | Finding |
| --- | --- | --- | --- |
| `XDG_RUNTIME_DIR` | `/run/droiddeck` | Child changes to `/run/droiddeck/mali-desktop` | Intentional cleanup isolation. |
| Parent `WAYLAND_DISPLAY` | `wayland-0`, real Android socket | `wayland-0`, symlink to Gamescope's generated socket | Name compatibility is correct; ownership also needs a held lock. |
| Parent lock before 0221 | Android holds `wayland-0.lock` | No held private `wayland-0.lock` | **Bug: native wrapper can replace the alias.** |
| `DISPLAY` | Initially unset by env-i; native wrapper sets it if Plasma requests Xwayland | Same | No discovered first-frame difference. |
| `XDG_SESSION_TYPE` | `wayland` | `wayland` | Same. |
| `XDG_CURRENT_DESKTOP`, `XDG_SESSION_DESKTOP` | `KDE`, `KDE` | Same shared desktop script | Same. |
| `DESKTOP_SESSION`, KDE session variables | `plasma`; full session, version 6, applications-as-scope=0 | Same | Same. |
| `QT_QPA_PLATFORM` | `wayland;xcb` in desktop environment | Same | KWin main selects its own internal QPA platform during startup in both routes. |
| KWin renderer | `KWIN_COMPOSE=Q` | Same | QPainter/software, no PanVK selection. |
| Qt/GL client rendering | `QT_QUICK_BACKEND=software`, `LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe` | Same | Intentionally software KWin, independent of Gamescope's GPU renderer. |
| Driver environment | Standard service may initially supply Zink/Turnip/cache controls; desktop resets software GL controls | Minimal Mali service env; private proxy variables stripped from nested child | No supported connection to the missing first request. No driver controls changed. |
| Scale | `DROIDDECK_NESTED_SCALE=1`; patched KWin | Same shared launcher/patch | Same. |
| KWin wrapper argv | `--wayland-display wayland-0 --socket wayland-1 --width ... --height ... --no-lockscreen`, then Plasma's args | Same | Kept unchanged. Native wrapper additionally passes its allocated listening FD/name. |
| Plasma argv and D-Bus | `dbus-run-session -- startplasma-wayland` | Same | No parallel desktop launcher introduced. |
| systemd/user session | Existing proot setup; `KDE_APPLICATIONS_AS_SCOPE=0`; no user systemd created here | Same shared desktop setup | The systemd activation warning occurs in both failing and successful host cases. |
| Locale/tmp/fonts/bookmarks/GPU app entries | Existing `lxqt` and desktop preparation | Reuses those same branches | Same launcher preparation, not a replacement script. |
| Extra Android service preparation | Standard audio/controller/store/client/maintenance setup | Existing isolated Mali service has a smaller setup | Unchanged; not needed for the demonstrated KWin first compositor frame. Not a claim of parity for every desktop feature. |
| CPU affinity | Existing `program_cores`/taskset behavior | Same inherited desktop logic | Unchanged. |

## Host output and Wayland comparison

| Behavior | Normal Android KDE host | 0220 Mali nested host | Finding |
| --- | --- | --- | --- |
| Output creation | Android compositor's custom server; no wlroots backend/allocator/scene | wlroots headless metadata output plus Pixman renderer, SHM allocator and scene | Different implementations with intentionally different output ownership. |
| `wl_output` | v4, `DroidDeck-1`, current/preferred mode, actual configured dimensions | v4, `HEADLESS-1`, custom dimensions | KWin 6.7.5's host registry handler does not bind wl_output. |
| Advertised refresh | Initially 60000 mHz; Android refresh updates propagated | 0 mHz from headless_add_output's initial mode | Incomplete metadata, **not the demonstrated first-frame blocker**. No refresh was invented or changed. |
| Physical size | Fixed virtual 340x190 mm | Unknown, 0x0 mm | Not consumed by this KWin host backend. |
| Scale/transform | Scale 1, normal transform | Scale 1, normal transform | Same effective nested output scale. |
| Output enable/commit | Custom Android rendering loop, not a wlr_output state machine | Metadata output is not enabled/committed for fake headless presentation | Leave unchanged: it is not the Android output or the frame scheduler. |
| Seat | Android seat, pointer/keyboard/touch, keymap/repeat | wlroots seat, pointer/keyboard, XKB keymap/repeat; existing outer input forwarding | Touch capability differs but does not block initialization; existing interactive input path retained. |
| xdg-shell/configure | Android custom xdg-shell v1; first empty commit sends configure | wlroots xdg-shell v3; KWin binds v1; initial commit configures size/fullscreen/activation | Actual KWin host probe confirms configure and matching ack. |
| Subsurfaces | Android custom surface tree | wlroots xdg/subsurface scene helpers, sync/desync and viewport handling | Both support KWin's single-pixel background plus content subsurfaces. |
| SHM formats | Wayland SHM baseline ARGB8888/XRGB8888 | Pixman renderer advertises and imports both, among its supported formats | KWin QPainter uses XRGB primary content and ARGB cursor buffers. Both are tested. |
| Scene/output association | No wlroots scene | Scene composed explicitly into SHM and then existing Mali upload/composite | Does not require wlr_scene_output or its headless presentation scheduling. |
| Frame scheduling/callbacks | Android render loop fires pending callbacks | Existing normal Mali paced loop; callbacks after successful Present | No schedule or callback behavior changed in 0221. |
| Presentation | Android CLOCK_MONOTONIC presentation feedback | Forward actual outer Android timestamps/sequence/flags; clear ZERO_COPY | No simulated headless clock or GPU timing substituted. |
| First configure/commit ordering | Connect, bind, create toplevel, empty commit, configure, ack, software buffers | Same sequence once the parent alias is protected | Previously KWin could connect to its own preallocated socket before reaching any of these steps on Mali's server. |
| Parent/child ownership | Service/proot owns session; native wrapper owns KWin server FD and restarts KWin | Existing Gamescope child plus service/broker ownership; same native KWin wrapper | Logger adds a waiting supervisor for the actual child; preserves FDs, signals and exit status. |
| Socket cleanup | Parent socket and lock held by Android; desktop excludes wayland-0* | Cleanup restricted to private directory; parent alias and lock excluded | 0221 supplies the missing lock; outer Android/Gamescope/broker sockets are untouched. |

Normal Gamescope is a third implementation, not the normal KDE host. Its
`wlsession_init`/`wlserver_init` create a headless backend plus multi-backend,
custom Vulkan renderer, keyboard group, seat, xdg listeners, Xwayland and gaming
protocols. Xwayland output construction enables/commits a sized mode with a
nonzero refresh and exposes its global. It starts the multi-backend and forwards
frame completion from Gamescope's compositor. It is not a wlroots scene-output
desktop implementation that can simply replace the Mali adapter. Calling that
entry wholesale would introduce unrelated renderer/Steam setup and risk the
validated proxy/Bionic/AHB path. Existing wlroots handlers and scene helpers
remain reused without importing that unrelated initialization.

## Confirmed bug and fix

KWin 6.7.5 `src/helpers/wayland_wrapper/kwin_wrapper.cpp` calls `wl_socket_create()`
in the native wrapper's constructor. `wl-socket.c` starts at wayland-0, opens its
lock file and takes an exclusive nonblocking flock. After acquiring it, it unlinks
an existing writable pathname, including a symlink, before binding its own socket.
The wrapper does this before launching KWin; the supplied `--socket wayland-1`
argument does not prevent the initial automatic allocation.

Normal Android's held lock makes it skip wayland-0. The private alias had no such
owner. Shell-entry alias checks therefore passed, then the native wrapper could
replace that alias. KWin's explicit parent-at-wayland-0 then connected to its own
preallocated server instead of Gamescope. That can stall the synchronous
registry/configure roundtrips used during backend initialization while KWin's
main thread cannot service its own server. The zero Mali commits and live KWin
match this self-connection stall.

The desktop package already includes util-linux/flock. The existing runtime
lock preload forwards real flock calls and preserves nonblocking ownership
conflicts; it only masks unsupported-filesystem errors. The runtime directory is
the app's internal `.wayland-rt` bind, not the shared-storage log directory. No
preload or filesystem behavior was changed.

The Mali-only managed wrapper now opens and holds the private alias's
`wayland-0.lock` through exec and the session lifetime. A competing owner fails
cleanly. The native wrapper skips that pathname and creates private wayland-1,
just as it does with normal Android's locked parent. No outer lock is linked,
opened or unlinked. No output, renderer, KWin argv or watchdog workaround was
needed for this demonstrated blocker.

## Host evidence, not phone validation

Small C protocol hosts used a public prebuilt wlroots 0.20.2 library and the
installed x86-64 KWin 6.7.5. No Gamescope or KWin executable was compiled.

| Host case | Result |
| --- | --- |
| KWin connected directly to unchanged 0220 server | 4 accepted commits and a first CPU-composed frame. |
| Native KWin wrapper, private alias without held lock | KWin alive; no connection to Mali server; 0 commits and no frame. |
| Native wrapper with held private alias lock | Configure/ack, SHM upload, 4 accepted commits and first CPU-composed frame. |
| Updated managed wrapper plus native wrapper and actual logging shim | Same first CPU frame; actual credential PID matched logged child PID; alias survived native allocation. |
| Failure summary against the unlocked native-wrapper reproduction | KWin connected=no to Mali, but actual descendant process alive=yes; argv/PID recorded. |

These probes did not run Plasma, the Android compositor, the broker or GPU
presentation. The lack of Android presentations in a CPU-only probe is expected.
They isolate the wrapper bug; they do not establish KDE phone success.

## Logs and handshake states

The existing managed `kwin_wayland_wrapper` also acts as a Mali-only logging shim
when invoked as `kwin_wayland`. The native wrapper still performs its normal
socket allocation, D-Bus setup and restart policy. Only its PATH gets a private
kwin-bin symlink to that managed script. The shim launches `/usr/bin/kwin_wayland`,
preserves inherited Wayland/Xwayland FDs and records to
`$BL_DEBUG_DIR/kwin-wayland.log`:

- Exact actual child argv, PID, supervisor PID and parent PID.
- Runtime, parent alias/target, KWin server socket, relevant session/Qt/KWin/driver
  environment, stdout and stderr.
- Actual child wait result: numeric exit status or terminating signal, preserving
  signal termination toward the native wrapper. An exit status of 15 is not
  mislabeled as signal 15.
- A second alias check after native socket allocation, before executing KWin.

The logging supervisor forwards normal termination signals and gives its actual
child Linux's parent-death SIGKILL protection, so a hard kill of the shim cannot
leave that child running. Failed lifetime setup exits cleanly with a log message.

0221 adds `mali-desktop-handshake.log` via Wayland's existing protocol logger and
client/resource listeners. Client identity uses SO_PEERCRED PID/UID/GID and /proc
process identity; actual global resource creation is observed, not just registry
advertisements or bind requests. Per-client progress includes registry requests,
global binds, surface/xdg/toplevel creation, initial commit, configure serials and
matching acks, SHM pools/buffers/formats, attach/damage, raw buffer-commit requests,
wlroots-accepted commits, subsurfaces, frame callback requests/done, presentation
requests/presented/discarded, disconnect and protocol error.

Sync callback done events are distinguished from surface frame callbacks. KWin
records are kept separate from wayland-info clients. At watchdog failure a
compact summary is also printed to session stderr, followed by a /proc descendant
check for a KWin that never connected. PID start times guard against PID reuse;
unreadable identity is reported as unknown. Text records are bounded at 4096;
dropped records, untracked clients, lost object history and scan limits are
explicit. Counters continue after text saturation. Existing frame-journal loss
and timing semantics are untouched.

The artifacts use the existing app-provided session log directory and existing
recursive session collection/redaction. No removed logging API or external
Download-directory override was restored.

## Validation still required

Patch order ends with 0219, 0220, **0221-mali-kde-handshake-trace.patch**. All 29
earlier patches remain byte-identical. Tests protect the unlocked-alias failure,
held-lock lifetime, dynamic parent socket names, actual child FD inheritance,
stdout/stderr/status/signals, cleanup/restart lock release, required global
bind/use, v1 xdg configure serial rejection, both SHM formats, subsurface and frame
lifecycles, presentation/disconnect/error reporting and unchanged normal/Steam/
interactive paths. No renderer, broker/proxy, Vulkan feature/synchronization,
AHB/release ownership, PanVK or shared-staging implementation changed.

Lightweight results: the full Phase 3 runner passed **55 tests**, including the
focused wrapper and real wlroots protocol fixtures. Steam/Phase 4–6/frame-journal
regressions passed **48 tests**; existing normal Desktop/Steam/session/runtime
contracts passed **89 tests**. Python syntax (including the embedded supervisor),
shell syntax and small C translation-unit syntax checks passed. Full-stack patch
application passed without fuzz/offsets, and tracked/new-file `git diff --check`
passed. No APK, complete Gamescope, NDK or driver build was run.

Changed files:

| File | Purpose |
| --- | --- |
| `tools/linuxfs/desktop/kwin_wayland_wrapper` | Mali-only parent lease and real-child logging/FD/signal/lifetime handling; original non-Mali exec retained. |
| `tools/gamescope/patches/0221-mali-kde-handshake-trace.patch` | Add read-only desktop handshake observation, failure snapshots and configure-error tests. |
| `tools/gamescope/build-in-arch.sh` | Run the bad-configure-serial test in the existing CI build. |
| `tools/mali-vulkan/run_phase3_tests.py` | Include focused wrapper comparisons and lifecycle tests. |
| `tools/mali-vulkan/test_desktop_startup.py` | Exercise real automatic Wayland socket allocation and the unlocked-alias failure, preserving dynamic names and outer socket inodes. |
| `tools/mali-vulkan/test_desktop_protocols.py` | Assert actual resource binds, PID separation, both SHM formats, callback/feedback counters, disconnects and configure errors. |
| `tools/mali-vulkan/test_kwin_wrapper.py` | Protect all previous patch bytes and unchanged GPU/watchdog functions; execute logging, inherited-FD, signal, hard-kill and lock-lifetime scenarios. |
| `tools/mali-vulkan/test_phase3.py` | Require 0221 last in the explicit patch stack. |
| `tools/gamescope/PATCHES.md` | Document 0221 and distinguish the wrapper fix from rendering changes. |
| `docs/development/mali-phase3.md` | Record 0220 device evidence and link the diagnosed native-wrapper gap. |
| `docs/development/mali-kde-host-comparison.md` | Full behavior comparison, source/host evidence, fixes and remaining checks. |

CI must compile/link ARM64 Gamescope and run `cursor_routing`,
`mali_desktop_protocols`, `mali_desktop_unconfigured`, `mali_desktop_bad_ack`,
component fingerprint/SONAME checks and APK/runtimeAssets validation. The existing
managed wrapper asset must match the new Gamescope overlay in the device build.

On Mali-G52, keep shared staging and PanVK OFF. Collect desktop/session/Wayland,
kwin-wayland and mali-desktop-handshake logs. Verify alias and lock after native
allocation, actual KWin credentials, configure/ack, SHM and accepted commits,
first visible KDE frame, actual Android presentation/release and continuing
callbacks. Then verify input, Stop/relaunch, no stale KWin/helper/Gamescope/proot
processes or sockets, balanced release/drain and preserved interactive/Steam
behavior. The existing 90-second first-frame and 15-second post-frame/interactive
watchdogs remain unchanged.
